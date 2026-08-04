# hdEmbree Optimization Notes

This document records profiling workflows, measurements, and optimization
opportunities for hdEmbree. Measurements should always identify the scene,
build configuration, renderer settings, and fixed random seed used.

## Profiling Build

The normal build remains a Release build in `build`:

```sh
pixi run configure
pixi run build
```

The profiling build uses a separate `build-profile` tree configured with
`RelWithDebInfo`, `-O3`, debug information, and frame pointers:

```sh
pixi run configure-profile
pixi run build-profile
```

`build-profile` installs the profiled hdEmbree plugin into the active Pixi
environment while retaining the regular Release OpenUSD runtime. Running
`pixi run build` restores the normal Release plugin.

Verify that the installed profile plugin contains line information:

```sh
readelf -S .pixi/envs/default/plugin/usd/hdEmbree.so \
    | rg 'debug_info|debug_line'
```

## Reproducing A Test Render

Use a pytest dry run to obtain the exact `usdrender` invocation without
rendering:

```sh
cd /path/to/typhoon-tests
pixi run pytest material-fidelity \
    --typhoon-provider /path/to/openusd \
    -k input_coat_darkening \
    --typhoon-dry-run -s
```

Use a fixed random seed for repeatable profiling. The
`input_coat_darkening` fixture authors a 512 by 512 resolution and 64 samples
per pixel, so the scene-authored sample count is retained to represent the
actual test workload. Add
`-s "{settings}.ty:randomNumberSeed = 1"` to the direct `usdrender`
invocation; the anonymous session opinion leaves the fixture unchanged.

## Hardware Counters

On systems where `/usr/bin/perf` is outside Pixi's clean environment, restore
the system paths explicitly:

```sh
pixi run --clean-env -x /usr/bin/env \
    PATH=/path/to/openusd/.pixi/envs/default/bin:/usr/bin:/bin \
    /usr/bin/perf stat -r 5 -d \
    -o /tmp/typhoon-input-coat-darkening-stat.txt -- \
    usdrender --complexity high --renderer Embree \
    -s "{settings}.ty:randomNumberSeed = 1" \
    /path/to/typhoon-tests/material-fidelity/surfaces/open_pbr_surface/input_coat_darkening.usda \
    --outputRoot /tmp/typhoon-profile-stat
```

Initial measurements from 2026-07-05:

| Measurement | Result |
| --- | ---: |
| Renderer time, average | approximately 2.133 s |
| End-to-end `usdrender` | 2.6625 +/- 0.0200 s |
| Throughput | approximately 7.87 million samples/s |
| Average CPU utilization | 16.6 logical CPUs |
| P-core IPC | 2.76 |
| E-core IPC | 2.29 |
| Branch misses | approximately 1.0% |
| P-core L1 data miss rate | 0.19% |

The workload is moderately backend-bound, but the initial counters do not
show a single dominant branch or cache failure.

## Sampling Profile

Collect frame-pointer call stacks for all renderer worker threads:

```sh
pixi run --clean-env -x /usr/bin/env \
    PATH=/path/to/openusd/.pixi/envs/default/bin:/usr/bin:/bin \
    /usr/bin/perf record \
    -o /tmp/typhoon-input-coat-darkening.data \
    -F 499 -e cycles:u -g --call-graph fp -- \
    usdrender --complexity high --renderer Embree \
    -s "{settings}.ty:randomNumberSeed = 1" \
    /path/to/typhoon-tests/material-fidelity/surfaces/open_pbr_surface/input_coat_darkening.usda \
    --outputRoot /tmp/typhoon-profile-record
```

Inspect the result with:

```sh
perf report -i /tmp/typhoon-input-coat-darkening.data
```

The initial profile captured approximately 23,000 samples with no lost
samples. The main self-cost ranges across P-cores and E-cores were:

| Hotspot | Self cost |
| --- | ---: |
| Embree triangle traversal | 10.7-11.1% |
| Embree instance traversal | approximately 2.3% |
| `HdGetValueData` | 5.9-7.2% |
| `mxcpp::EvalOpenPbr` | 5.4-6.2% |
| String comparison | 3.4-3.8% |
| `oqmc::sobolReversedIndex` | 3.1-4.1% |
| String hashing | 2.7-3.2% |
| `ty::Renderer::_BuildShadingContext` | 2.6-3.1% |
| `malloc` and `free` combined | approximately 2.3-3.5% |

Dome texture sampling, directional PDF evaluation, and lat-long conversion
together account for roughly another 10% of sampled cycles. These costs are
separate self-cost entries, but optimization results must be measured because
their potential improvements are not necessarily additive.

## OpenUSD Trace

Enable OpenUSD's global trace collector for a direct render:

```sh
PXR_ENABLE_GLOBAL_TRACE=1 \
pixi run usdrender \
    -s "{settings}.ty:randomNumberSeed = 1" \
    /path/to/typhoon-tests/material-fidelity/surfaces/open_pbr_surface/input_coat_darkening.usda \
    --outputRoot /tmp/typhoon-profile-trace \
    > /tmp/typhoon-input-coat-darkening-trace.txt 2>&1
```

`ty::Renderer` has coarse trace scopes around pre-render setup, Embree
scene commit, preview tracing and resolve, full-resolution sample tracing and
resolve, convergence checks, and AOV finalization. The scopes deliberately sit
outside per-ray and per-BSDF loops to avoid materially perturbing the render.

The initial trace split 2.111 seconds in `ty::Renderer::Render()` as:

| Phase | Time | Share |
| --- | ---: | ---: |
| Full-resolution tile tracing | 2050.2 ms | 97.1% |
| Resolve 64 sample passes | 35.7 ms | 1.7% |
| Preview tracing and resolve | 21.5 ms | 1.0% |
| Pre-render setup | 2.4 ms | 0.1% |
| Embree scene commit | 0.136 ms | negligible |

For this scene, scene commit, setup, and AOV resolve are not primary
optimization targets.

## End-to-End `usdrender` Overhead

The measurements previously in this section predated the C++ rewrite and
included Python imports, PySide context creation, and
`UsdAppUtilsFrameRecorder`. Current `usdrender` is a native C++ executable
that drives `UsdImagingGLEngine` directly. CPU mode creates no graphics
context. Re-baseline end-to-end timings before using the historical 2.66 s
result or its startup breakdown for optimization decisions.

## Prioritized Opportunities

### 1. Cache `ty::BufferSampler` metadata

Every primvar sample currently calls `HdVtBufferSource::GetData()`,
`GetTupleType()`, and `HdDataSizeOfTupleType()`. `GetData()` reaches
`HdGetValueData`, which consumes 5.9-7.2% of sampled cycles in the initial
profile.

`ty::BufferSampler` references an immutable buffer source owned alongside
the sampler. Cache its base pointer, element size, tuple type, and element
count in the sampler. This is the most focused, comparatively low-risk first
optimization.

### 2. Cache common primvar sampler pointers

`_BuildShadingContext()` repeatedly searches the prototype primvar hash map
for `st`, display color, tangent, bitangent, computed tangent, and computed
bitangent at every hit. Resolve and store these common sampler pointers while
populating the prototype context. This should address part of the measured
hashing cost and reduce per-hit control flow.

### 3. Replace string material dispatch with an enum

`EvalGraph::_EvalMaterialModel()` compares the compiled material model string
against each supported model during every shading evaluation. Resolve the
model to an enum when compiling the graph and dispatch on that enum. This is a
likely source of the measured 3.4-3.8% string-comparison cost.

### 4. Avoid rebuilding constant OpenPBR closures per hit

`input_coat_darkening` has effectively constant OpenPBR parameters, but
`EvalOpenPbr()` rebuilds its closure tree for every shading point. The function
itself consumes 5.4-6.2%, with additional allocation and vector-growth costs.

Potential approaches, in increasing order of scope, are:

- reserve the expected number of closure nodes;
- constant-fold context-independent graph nodes;
- cache context-independent material-model results;
- represent immutable cached closure trees without copying and reallocating
  their node storage for each hit.

Any closure caching must preserve graphs driven by textures, primvars,
position, normals, tangents, or other shading-context-dependent inputs.

### 5. Reuse dome-light calculations

Dome sampling repeatedly performs direction-to-lat-long conversion, texture
sampling, and directional PDF evaluation. Carry already-computed UV,
luminance, or PDF information through the light-sampling path where possible,
and check for duplicated texture fetches between sampling and MIS evaluation.

### 6. Investigate OpenQMC reversed-index reuse

`oqmc::sobolReversedIndex` consumes 3.1-4.1% of sampled cycles. Determine
whether a reversed index is recomputed across multiple sample dimensions for
the same pixel and sample. Cache or incrementally update it only if that
preserves the exact OpenQMC sequence.

## Textured Renders Are Much Slower Than Untextured (2026-07-07)

Textured assets rendered roughly an order of magnitude slower than the same
mesh with constant inputs. Because `perf` was unavailable on the test host
(`perf_event_paranoid=4`), the cause was isolated with A/B render timing on a
single brass sphere (`ND_UsdPreviewSurface` + two `ND_tiledimage`, 2048x2048
JPEG color and roughness, `usdrender --renderer Embree --disableGpu -s
"{settings}.ty:randomNumberSeed = 1"`, 64 logical CPUs, renderer time):

| Variant | Time | Isolates |
| --- | ---: | --- |
| Constant `diffuseColor`/`roughness` (no texture) | 1.70 s | baseline |
| `ND_constant` nodes, same graph topology | 1.76 s | graph plumbing is ~free |
| `ND_tiledimage`, tiny 4x4 textures | 3.25 s | fixed per-tap overhead |
| `ND_tiledimage`, 2048^2 JPEG | 5.72 s | full texture cost |

Splitting the full-texture case with an env-gated no-op in `Sample2D`:

| Segment | Cost |
| --- | ---: |
| MaterialX graph plumbing | approximately 0.06 s |
| Image-node work (footprint 3x eval, string ops, request build) | approximately 0.72 s |
| OIIO `texture()` call itself | approximately 3.08 s |

The `texture()` call dominated. The texture:constant ratio also grew with
thread count (1.78x at 1 thread, 2.41x at 16, 3.36x at 64), pointing at shared
locking inside OIIO's tile cache rather than raw per-tap arithmetic.

Root cause: untiled inputs (JPEG, PNG) were cached with `autotile = 64`, which
splits a 2048^2 image into ~1024 tiles. Path-traced taps scatter across the
surface and cross tile boundaries constantly; each crossing is a locked
tile-cache lookup. Sweeping `autotile` (output stays bit-identical because it
only changes caching granularity, not filtering) on the JPEG scene:

| `autotile` | Time |
| --- | ---: |
| 64 (previous default) | 5.9 s |
| 256 | 4.2 s |
| 512 (new default) | 3.95 s |
| 1024 | 3.8 s |
| pre-generated mipped tiled `.tx` | 3.4 s |

Applied fixes in `oiioTextureSystem.cpp`:

1. Raised `autotile` from 64 to 512. On the brass sphere this cut renderer time
   from 5.72 s to 3.73 s (about 35%) with an exact `oiiotool --diff` match to
   the previous output. 512x512 blocks (1 MB float RGBA) are a middle ground;
   larger blocks are marginally faster but cost more resident cache in
   many-texture scenes.
2. Cached the resolved OIIO `TextureHandle` (and its `ustring` and UDIM flag)
   per file path in a lock-free `tbb::concurrent_unordered_map`, so the hot path
   no longer constructs a `ustring` and calls `get_texture_handle` -- both
   globally locked -- on every tap. This was a smaller win on the two-texture
   sphere but removes shared-lock contention that grows with texture and thread
   count.

For texture-heavy production scenes, feeding pre-generated tiled, mipped `.tx`
inputs remains the fastest option and avoids the auto-tile/auto-mip path
entirely.

## Materials-On Renders Were ~46x Slower In usdview (2026-07-08)

The ALab `tool_garden_hose01` asset rendered ~46x slower in usdview with
Enable Scene Materials on (10.9 s vs 0.24 s at 8 spp, 1121x793, 64 threads),
scaling to tens of minutes at converged sample counts. Temporary shading-cost
diagnostics (per-sample counters and timers printed with the render
statistics; removed again once the investigation concluded) showed the per-tap and
per-context costs were normal but every material graph evaluation cost
~410-450 us versus ~6 us for the brass reference material, and the ratio
shrank with fewer threads (76 us/eval at 8 threads) — lock contention, not
arithmetic.

A dedicated reeval counter isolated it: the asset's UsdPreviewSurface wires
seven UsdUVTexture inputs whose `st` comes from a UsdPrimvarReader. Each
texture computes a filter footprint by re-evaluating its texcoord input three
times (base/dx/dy), so every material eval ran 21 connected-input
re-evaluations, 461 million in the test render — 91% of all material eval
time. Each re-evaluation ran one geomprop node whose primvar lookup called
`TfToken(name)`: a locked global-table operation, hammered from 64 threads.

Fixes (all output bit-identical, `oiiotool --diff` PASS on the repro and the
brass scene; 297/297 MaterialXCpp tests pass):

1. `_SampleGeomProp` now looks up samplers in a string-keyed mirror map
   (`ty::PrototypeContext::primvarMapByString`) instead of constructing a
   TfToken per call. This was the dominant fix: 155 s -> 22 s on the repro
   scene (19.4 us -> 0.44 us per re-evaluation).
2. Texture footprints read the base texcoord from the cached input value and
   only re-evaluate the upstream subgraph for the dx/dy probes (21 -> 14
   re-evaluations per eval): 22 s -> 17 s.
3. `_SampleGeomProp` tries common primvar types (Vec2f/Vec3f/float) before
   matrices — `ty::BufferSampler::Sample` only accepts an exact
   tuple-type match, so order does not affect results.
4. `_EvalGeomPropValue` reads the primvar name by reference instead of
   copying the string per evaluation, and nested input re-evaluations reuse
   a per-thread depth-indexed scratch pool instead of allocating fresh
   containers per call.

Net: the hose-material repro (sphere, seven-texture UsdPreviewSurface, 512^2,
256 spp) went from 154.8 s to 17.1 s (~9x) with unchanged output. Remaining
gap versus brass (42.5 us/eval vs 5.7 us/eval) is the larger node count and
the remaining 14 re-evaluations; candidates are caching context-independent
node outputs across footprint probes and reducing Value copies.

Repro recipe: bind a material with `UsdPreviewSurface` + several
`UsdUVTexture` nodes whose `st` connects to a `UsdPrimvarReader_float2`
(the ALab assets' standard wiring). The temporary diagnostics line
`Input reevals` showed the multiplier directly while it existed; re-add
counters around `EvalGraph::_EvaluateNodeOutput` to measure this again.

## Compile-Time Geomprop Binding Handles (2026-07-28)

MaterialX geomprop nodes now compile names to material-owned integer handles.
Each mesh prototype resolves those handles to sampler and uniform-value vectors
when its material binding changes. Hit-time evaluation therefore does no string
hashing, string copying, or `TfToken` construction. This supersedes the
`primvarMapByString` mitigation above and removes that mirror map.

Five fixed-seed repetitions used the profile build, `perf stat -r 5 -d`, the
stage-authored resolution and sample settings, `--complexity high`, and
`ty:randomNumberSeed = 1`. The measured workloads were the material-fidelity
`geompropvalue_color3`, `standard_surface/textured`, and
`standard_surface/glass` stages:

| Workload | Wall before | Wall after | Renderer before | Renderer after | Samples/s before | Samples/s after |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| geomprop | 0.2646 s | 0.2727 s | 0.1068 s | 0.1046 s | 2.468 M | 2.512 M |
| textured | 1.9531 s | 1.9814 s | 1.6290 s | 1.6476 s | 2.576 M | 2.550 M |
| glass | 3.3913 s | 3.4678 s | 3.0580 s | 3.1508 s | 1.372 M | 1.332 M |

These small fixtures do not demonstrate an end-to-end speedup: wall time moved
by +1.4% to +3.1%, while renderer time and throughput moved in both directions.
Their startup and fixed render work dominate, and none reproduces the original
64-thread, seven-texture geomprop re-evaluation contention. The durable result
is removal of the known hot-path operation; a production-scale repeat of the
garden-hose workload is still required before claiming a performance gain.

The older external material-fidelity harness could not run because its provider
command still passes the removed `usdrender --disableCameraLight` option. The
current Typhoon golden-image suite passed all 436 cases in 244.04 seconds,
including geomprop, textured, glass, displacement, and uniform geomprop cases.
Focused renderer and MaterialX C++ regressions additionally cover binding-table
refresh, defaults, and bounds checks.

## Duplicate Camera-Hit Material Evaluation (2026-07-08)

The shading-cost diagnostics showed `Color mat evals` tracking `Path mat
evals` almost one-to-one (0.57 vs 0.58 per camera sample on the brass
sphere; 25.5 of 69 material-evaluation CPU-seconds on the ALab garden
hose). `_ComputeColor` built a shading context, evaluated the material
graph, and resolved the shading normal on every camera hit — but with
lighting enabled it then called `_TracePath`, which re-intersects the same
ray and re-derives all of that at the hit itself, so the first evaluation
was never consumed. The closure and resolved normal are only used by the
camera-light/AO fallback branch.

Fix: `_ComputeColor` dispatches to `_TracePath` before building any surface
data; the context build, material evaluation, and normal resolution now run
only on the no-lighting fallback path.

Validation: brass sphere 4.50 s -> 3.08 s renderer time (~32%), `Color mat
evals` 0.57/sample -> 0 with lighting on, and exact `oiiotool --diff`
matches for both the lit scene and the
`-s "{settings}.ty:enableLighting = false"` fallback. All
hdEmbree/MaterialXCpp unit tests pass.

Follow-up (2026-07-13): the renderer now selects `_IntegratePath` or
`_IntegrateUnlit` before intersection. Each integrator owns its camera hit and
returns that retained hit with its radiance for AOV evaluation. This removes the
remaining duplicate primary intersection from the lit path; the names above
describe the historical implementation measured by this optimization.

## Cache Reflection-Only Closure Classification (2026-07-27)

`_IsReflectionOnlyClosure()` recursively classifies an immutable BSDF closure
tree at every path vertex that needs direct-light or path policy. A future
performance change could classify the tree once when the closure is built and
store the result with the compiled closure. Keep that separate from dispatch
readability changes because it changes closure state and ownership.

The `std::visit` to `std::get_if` readability refactor was measured on the
OpenPBR carpaint material-fidelity scene at 256x256, 64 spp, 16 bounces, fixed
seed, and adaptive sampling disabled. Five-run means were 1.084 s versus
1.094 s wall time, 0.7194 s versus 0.7266 s renderer time, and 5.830 versus
5.775 million samples/s before versus after. The approximately 1% difference
was within the observed run-to-run spread, and the images were bit-identical.
Hardware-counter measurements were unavailable because
`kernel.perf_event_paranoid=4` and passwordless sudo was not configured.

## Smooth-Normal Terminator Patch Benchmark (2026-07-30)

The smooth shadow-terminator offset needs the triangle-corner normal sampler on
each non-refined surface hit. Resolving it at hit time would add a normals-map
lookup and two runtime casts before direct-light sampling is known to happen.
`PrototypeContext` now caches the sampler pointer and its supported
vertex/varying or face-varying kind when prototype primvars are built. The hit
path reads that cache directly; unsupported and constant samplers produce no
lift.

Five fixed-seed repetitions compared detached `HEAD` (`4b5b13aa7`) with this
change on the same machine under the performance power profile. Both Release
builds rendered `input_coat_darkening` at its authored 256x256, 64 spp, and 16
bounces with `--complexity high` and `ty:randomNumberSeed = 1`:

| Build | Wall | Renderer | Samples/s |
| --- | ---: | ---: | ---: |
| Before | 2.3794 s | 2.0240 s | 2.078 M |
| After | 2.2650 s | 1.9214 s | 2.183 M |

The complete patch improved wall and renderer time by 4.8% and 5.1%, while
throughput improved by 5.1%. Core frequency was comparable (3.663 GHz before,
3.731 GHz after). This small fixture shows no aggregate regression, but it
exercises only one material-heavy workload and compares the whole bump-normal
and terminator patch. It does not isolate the sampler cache or establish that
the cache itself is an optimization.

## Plan 27 Corrected-HEAD Counter Baseline (2026-07-31)

The follow-up to the performance-regression investigation corrected smooth-shadow
triangle inputs, made their construction lazy, gated exact object-position
interpolation on compiled graph requirements, cached fixed-name tangent
samplers, and hoisted default reflective-normal correction.

Carpaint and glass were rendered at 256x256, fixed 32 spp, 16 bounces, one
light sample, fixed seed one, and adaptive sampling disabled. Unscaled raw
hybrid-PMU instructions are the atom/core sum:

| Build | Case | Raw instructions | Delta from old profiling HEAD |
| --- | --- | ---: | ---: |
| RelWithDebInfo/profile | carpaint | 501.893B | -5.18% |
| RelWithDebInfo/profile | glass | 920.558B | -7.63% |
| Release | carpaint | 499.401B | — |
| Release | glass | 914.705B | — |

The subsequent reviewed implementation stores diffuse/specular defaults once
and follows an allocation-free linked index containing only authored-normal
leaves. Five-repeat Release counters against the corrected-HEAD rows above
were:

| Case | Before | Sparse preparation | Delta |
| --- | ---: | ---: | ---: |
| carpaint | 499.401B | 498.265B | -0.23% |
| glass | 914.705B | 913.652B | -0.12% |

These are aggregate post-review deltas: restored finite/degenerate guards and
the pinned Embree interpolation association were added with the O(1) path, so
they do not isolate sparse preparation. The sub-percent changes are directional
because they are smaller than hybrid-PMU run-to-run uncertainty. An initial
sparse-ID `std::vector` version
measured +0.07% and +0.21% respectively and was rejected because its extra
allocation erased the intended saving. Raw accepted measurements are
`/tmp/hdembree-27-sparse-linked-release-{carpaint,glass}.txt`.

The corrected profiling build remains +3.43% and +3.72% above the historical
`f66ecfb1f` carpaint and glass baselines respectively. Most of the recorded
regression is recovered, but the residual remains unattributed. The comparison
also includes a correctness change to smooth-shadow geometry and therefore
does not isolate individual optimizations. The final reviewed implementation's
complete Typhoon rendered gate passed 438 cases in 278.77 seconds. Release was
restored after profiling.

## Normal-Lifecycle Refactor Benchmark (2026-08-04)

The normal-lifecycle refactor resolves the final composited graph normal before
validating leaf normals, prepares path closures exactly once, and skips closure
normal work for shadow-surface evaluation. Five fixed-seed Release repetitions
compared detached `b73f9c0b1` with the complete change on the same machine.
Both builds used `--complexity high` and `ty:randomNumberSeed = 1`; scene-authored
resolution, sampling, and path settings were unchanged.

| Case | Metric | Before | After | Delta |
| --- | --- | ---: | ---: | ---: |
| `openPbr_parameter_parity` | Renderer time | 0.3942 s | 0.3952 s | +0.25% |
|  | Samples/s | 28.285 M | 28.205 M | -0.28% |
|  | End-to-end time | 0.5835 s | 0.5730 s | -1.81% |
| `transparency_material_gallery` | Renderer time | 1.7550 s | 1.6734 s | -4.65% |
|  | Samples/s | 6.354 M | 6.658 M | +4.77% |
|  | End-to-end time | 2.0046 s | 1.8922 s | -5.61% |

The material-heavy result is neutral within run-to-run variation. The
visibility-heavy result is consistent with removing graph-normal resolution,
leaf validation, and closure preparation from shadow rays, but the aggregate
comparison does not isolate those operations. `perf stat` end-to-end relative
standard deviations were 2.53% before and 0.37% after for the material case,
and 1.04% before and 1.10% after for the visibility case.

## Validation Rules

For each optimization:

1. Use a fixed random seed and unchanged scene-authored render settings.
2. Compare at least five `perf stat` repetitions before and after.
3. Use renderer-reported time for path-tracing throughput and end-to-end time
   for whole-command regressions.
4. Re-record `perf.data` after rebuilding so the profile build ID matches the
   installed plugin.
5. Run focused image-fidelity tests before broader suites.
6. Test more than one workload before generalizing: material-heavy,
   texture-heavy, many-light, and deep-path scenes should be represented.

The initial instrumentation was validated with only
`input_coat_darkening`: one test passed and 77 cases were deselected. The full
material-fidelity suite was not run.
