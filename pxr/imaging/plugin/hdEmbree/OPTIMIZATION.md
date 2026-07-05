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
cd /home/anders/code/typhoon-tests
pixi run pytest material-fidelity \
    --typhoon-provider /home/anders/code/openusd-omniverse \
    -k input_coat_darkening \
    --typhoon-dry-run -s
```

Use a fixed random seed for repeatable profiling. The
`input_coat_darkening` fixture authors a 512 by 512 resolution and 64 samples
per pixel, so the scene-authored sample count is retained to represent the
actual test workload.

## Hardware Counters

On systems where `/usr/bin/perf` is outside Pixi's clean environment, restore
the system paths explicitly:

```sh
pixi run --clean-env -x /usr/bin/env \
    PATH=/home/anders/code/openusd-omniverse/.pixi/envs/default/bin:/usr/bin:/bin \
    HDEMBREE_RANDOM_NUMBER_SEED=1 \
    /usr/bin/perf stat -r 5 -d \
    -o /tmp/typhoon-input-coat-darkening-stat.txt -- \
    usdrender --complexity high --renderer Embree --disableCameraLight \
    /home/anders/code/typhoon-tests/material-fidelity/surfaces/open_pbr_surface/input_coat_darkening.usda \
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
    PATH=/home/anders/code/openusd-omniverse/.pixi/envs/default/bin:/usr/bin:/bin \
    HDEMBREE_RANDOM_NUMBER_SEED=1 \
    /usr/bin/perf record \
    -o /tmp/typhoon-input-coat-darkening.data \
    -F 499 -e cycles:u -g --call-graph fp -- \
    usdrender --complexity high --renderer Embree --disableCameraLight \
    /home/anders/code/typhoon-tests/material-fidelity/surfaces/open_pbr_surface/input_coat_darkening.usda \
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
| `HdEmbreeRenderer::_BuildShadingContext` | 2.6-3.1% |
| `malloc` and `free` combined | approximately 2.3-3.5% |

Dome texture sampling, directional PDF evaluation, and lat-long conversion
together account for roughly another 10% of sampled cycles. These costs are
separate self-cost entries, but optimization results must be measured because
their potential improvements are not necessarily additive.

## OpenUSD Trace

Enable OpenUSD's global trace collector for a direct render:

```sh
PXR_ENABLE_GLOBAL_TRACE=1 \
HDEMBREE_RANDOM_NUMBER_SEED=1 \
pixi run usdrender --disableCameraLight \
    /home/anders/code/typhoon-tests/material-fidelity/surfaces/open_pbr_surface/input_coat_darkening.usda \
    --outputRoot /tmp/typhoon-profile-trace \
    > /tmp/typhoon-input-coat-darkening-trace.txt 2>&1
```

`HdEmbreeRenderer` has coarse trace scopes around pre-render setup, Embree
scene commit, preview tracing and resolve, full-resolution sample tracing and
resolve, convergence checks, and AOV finalization. The scopes deliberately sit
outside per-ray and per-BSDF loops to avoid materially perturbing the render.

The initial trace split 2.111 seconds in `HdEmbreeRenderer::Render()` as:

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

The initial measurements leave approximately 0.53 seconds between the
renderer-reported time and the end-to-end command time:

| Measurement | Time |
| --- | ---: |
| `HdEmbreeRenderer` | approximately 2.13 s |
| End-to-end `usdrender` | approximately 2.66 s |
| Difference | approximately 0.53 s |

Python `cProfile` and the OpenUSD trace give the following approximate
breakdown for `input_coat_darkening`:

| Phase | Time |
| --- | ---: |
| Python startup and USD module imports | approximately 115 ms |
| PySide and OpenGL context creation | approximately 98 ms |
| USD stage composition | approximately 47 ms |
| Plugin loading, including hdEmbree | approximately 18 ms |
| Hydra synchronization and execution | approximately 184 ms |
| Embree mesh population, within Hydra synchronization | approximately 48 ms |
| CPU-to-GPU AOV copies, within Hydra execution | approximately 65 ms |
| EXR plugin loading and teardown | approximately 1 ms |

These values are not strictly additive. hdEmbree renders asynchronously, so
some Hydra task execution overlaps `HdEmbreeRenderer::Render()`.

`usdrender` creates a PySide OpenGL context by default even when using the CPU
Embree renderer. Passing `--disableGpu` avoids this path. Five focused runs
measured:

| Configuration | End-to-end time |
| --- | ---: |
| Default GPU-enabled `usdrender` | 2.6625 +/- 0.0200 s |
| `usdrender --disableGpu` | 2.5749 +/- 0.0240 s |

This saves approximately 88 ms, or 3.3%. OpenUSD warns that color correction
is unavailable with the GPU disabled, but the GPU-enabled and GPU-disabled
linear EXRs for this fixture passed an exact `oiiotool --diff` comparison.
Broader fidelity coverage is still required before changing all test runs.

A direct C++ implementation that retains `UsdAppUtils::FrameRecorder` and the
same Hydra path would mainly remove Python startup, imports, and binding
overhead. A reasonable estimate is a 100-150 ms reduction, not the full
0.53 seconds. USD composition, Hydra and scene-index setup, mesh
synchronization, renderer/plugin initialization, AOV handling, and image
writing would remain.

Bypassing `FrameRecorder`, Hgi, or Hydra could remove more overhead, but that
would be a substantially different rendering path and would make the test
runner less representative. For a suite containing many short renders, a
persistent worker that amortizes interpreter, plugin, and render-framework
initialization may provide more benefit than translating the command-line
driver to C++.

## Prioritized Opportunities

### 1. Cache `HdEmbreeBufferSampler` metadata

Every primvar sample currently calls `HdVtBufferSource::GetData()`,
`GetTupleType()`, and `HdDataSizeOfTupleType()`. `GetData()` reaches
`HdGetValueData`, which consumes 5.9-7.2% of sampled cycles in the initial
profile.

`HdEmbreeBufferSampler` references an immutable buffer source owned alongside
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
