# Investigation: performance regression since `f66ecfb1f`

Status: counter bisection complete; causal attribution remains provisional
pending historical renderer-work counters and Release confirmation. The HEAD
correctness/per-event work is implemented, fully regression-tested, and
re-baselined below in both Release and profiling builds.

Date: 2026-07-31.

## Scope

Compare `typhoon-anders` HEAD `793bcf237` with
`f66ecfb1f8efe70f75a80436cfdec3eeeaaba4d5` using:

- `open_pbr_surface/carpaint.usda`
- `open_pbr_surface/glass.usda`

from `/home/anders/code/aousd-materials-test-suite/test-suite/surfaces/`.

The laptop's wall-clock rate is not stable enough to use elapsed time as the
primary signal. The investigation therefore uses fixed-camera-sample renders
and retired instruction counts. Fixed samples per pixel are **not fixed
renderer work**: a transport change can alter continuation, average path
length, direct-light eligibility, and shadow-ray count. The measurements below
establish instructions per camera sample, not instructions per path vertex.
Cycle-stack samples identify where work is spent, but their percentages are not
treated as elapsed-time measurements.

## HEAD implementation

The corrective implementation uses one mesh-owned genuine-triangle data path:

- prototype contexts borrow the mesh's triangulated indices and object-space
  points alongside, but distinct from, the authored-st derivative arrays;
- smooth-shadow construction fetches corresponding corners and normals, applies
  the instance transform, and uses Embree barycentrics;
- construction is lazy and cached per interaction at the first qualifying
  direct-light visibility sample;
- compiled graphs record whether reachable nodes require exact object-space
  position. Coarse triangles use the genuine-corner path, refined geometry
  retains `rtcInterpolate1`, and graphs needing no object-space position skip
  both;
- fixed-name tangent samplers are cached in the prototype context;
- closure construction records an allocation-free linked index of
  authored-normal nodes. Preparation stores
  the diffuse default once, computes the reflective default at most once when
  an un-authored reflective leaf exists, and visits only recorded authored
  leaves for individual validation and correction.

The smooth-shadow regression test compares the renderer helper against the
genuine-corner calculation and proves that the removed authored-st
pseudo-triangle differs materially. A far-origin interpolation test compares
the cached triangle arithmetic directly with `rtcInterpolate1`. Graph tests
cover explicit object position, world-space position, and object-position
defaults injected during graph normalization.
This implementation does not supply the historical event counters requested
below and does not retroactively change the attribution claims.

### Corrected-HEAD counters and validation

The fixed-32-spp workloads were repeated after the implementation. Raw
instruction values remain the unscaled atom/core sum:

| Build | Case | Raw instructions | Instructions/camera sample | Delta from old profiling HEAD |
| --- | --- | ---: | ---: | ---: |
| profiling | carpaint | 501.893B | 239,321 | -5.18% |
| profiling | glass | 920.558B | 438,956 | -7.63% |
| Release | carpaint | 499.401B | 238,133 | — |
| Release | glass | 914.705B | 436,166 | — |

The profiling comparison uses the old-HEAD values in the checkpoint tables
below. Against `f66ecfb1f`, corrected profiling HEAD is still +3.43% for
carpaint and +3.72% for glass. The implementation therefore recovers most, not
all, of the recorded regression. It also changes the broken smooth-shadow
inputs, so the delta combines corrected transport with reduced per-event work
and is not causal attribution.

The five-repeat Release pass additionally measured:

| Case | Raw cycles (atom + core) | Task clock | Elapsed |
| --- | ---: | ---: | ---: |
| carpaint | 230.572B | 71.401 s | 3.894 +/- 0.068 s |
| glass | 425.444B | 132.317 s | 6.653 +/- 0.117 s |

The later reviewed-HEAD comparison used the Release instruction rows above
and five repetitions of the allocation-free sparse implementation:

| Case | Before | Sparse preparation | Delta |
| --- | ---: | ---: | ---: |
| carpaint | 499.401B | 498.265B | -0.23% |
| glass | 914.705B | 913.652B | -0.12% |

The aggregate also contains restored finite/degenerate guards and the pinned
Embree interpolation association, so it does not isolate sparse preparation.
The direction is favorable but below hybrid-PMU uncertainty. A rejected
`std::vector` sparse-index prototype measured +0.07%/+0.21%; the accepted
linked index adds no per-tree allocation. Raw accepted files are
`/tmp/hdembree-27-sparse-linked-release-{carpaint,glass}.txt`.

Validation completed on the exact Release source:

- `pixi run build`;
- five registered focused hdEmbree CTest targets, the sampling and light
  sampler executables, and all 346 MaterialXCpp cases;
- adversarial review of the new tests, with all findings resolved;
- complete Typhoon golden-image gate: 438 passed in 278.77 seconds.

Raw files are `/tmp/hdembree-27-head-{release,profile}-{carpaint,glass}.txt`.
The Release plugin was restored after profiling.

## Reproduction

Every checked revision was built and installed with:

```sh
pixi run build-profile
```

The counter-bisection workload fixes the seed and sample count and disables
adaptive sampling:

```sh
powerprofilesctl launch --profile performance -- \
    pixi run --clean-env -x /usr/bin/env \
    PATH="$PWD/.pixi/envs/default/bin:/usr/bin:/bin" \
    /usr/bin/perf stat --no-scale -r 3 -e instructions \
    -o /tmp/<revision>-<case>-raw-instructions.txt -- \
    usdrender --complexity high --renderer Embree \
    -s "{settings}.ty:randomNumberSeed = 1" \
    -s "{settings}.ty:convergedSamplesPerPixel = 32" \
    -s "{settings}.ty:enableAdaptiveSampling = false" \
    <stage.usda> --outputRoot /tmp/<output>
```

Every render reported:

- resolution: 256 x 256
- samples per pixel: 32
- total samples: 2,097,152
- maximum bounces: 16
- light samples: 1
- sampler: `openqmc_sobolbn`

## Hybrid-PMU counter rule

The first `perf stat -d` pass used perf's default scaled counts. Those displayed
`cpu_atom` and `cpu_core` values must **not** be added: perf independently
scales each hybrid-PMU event by its running-time fraction, so adding the
estimates double-counts work when scheduling changes.

The bisection was rerun with `--no-scale`. The value used below is:

```text
raw cpu_atom/instructions + raw cpu_core/instructions
```

Each PMU counts only while the workload runs on that CPU type, so their
unscaled raw sum is the retired instruction count covered across both types.
The table contains the sum of the two three-run means. Perf reports uncertainty
for each PMU separately, not for their correlated sum. Treat sub-percent
differences as directional until repeated on a homogeneous pinned CPU set.
The 5-12% steps are well outside that qualification.

## Fixed-camera-sample instruction results

### Glass

| Checkpoint | Subject or range endpoint | Raw instructions | Delta from previous checkpoint |
| --- | --- | ---: | ---: |
| `f66ecfb1f` | baseline | 887.553B | — |
| `82817ed3b` | through mesh-context ownership | 894.482B | +0.78% |
| `95c9258d4` | split renderer helpers | 896.868B | +0.27% |
| `4b2a9d5d0` | through suite-gate policy | 898.971B | +0.23% |
| `1d34d4ff8` | split MaterialXCpp BSDF implementation | 905.922B | +0.77% |
| `86577fb1a` | split light samplers by type | 906.068B | +0.02% |
| `4b5b13aa7` | add bump plan 26 | 906.151B | +0.01% |
| `8e883c76f` | fix bump-mapped shading consistency | 974.775B | **+7.57%** |
| `793bcf237` | stabilize MaterialX object-space positions | 996.589B | **+2.24%** |

Total baseline-to-HEAD increase: **+12.29%** per fixed set of camera samples in
the profiling build.

The measured extraction boundaries are smaller than the two newest steps:

- renderer-helper split: +0.27%, near counter uncertainty
- BSDF split: +0.77%, probably real but needs a pinned repeat
- light-sampler split: +0.02% instructions; any cross-TU penalty would instead
  appear as more cycles per instruction

The residual `f66ecfb1f` to `4b5b13aa7` increase is still +2.10%. It is not
accepted or fully attributed. `82817ed3b` is a range endpoint, not proof that
the mesh-context ownership commit itself added 0.78%.

### Carpaint

| Checkpoint | Raw instructions | Delta from previous checkpoint |
| --- | ---: | ---: |
| `f66ecfb1f` | 485.261B | — |
| `4b5b13aa7` | 494.721B | +1.95% |
| `8e883c76f` | 520.328B | **+5.18%** |
| `793bcf237` | 529.290B | **+1.72%** |

Total baseline-to-HEAD increase: **+9.07%** per fixed set of camera samples in
the profiling build.

The carpaint result rules out a glass-transmission-only explanation for the
`8e883c76f` step. It does not rule out changed average path length in carpaint.
The residual `f66ecfb1f` to `4b5b13aa7` increase is +1.95%.

## Broader counter pass

An initial five-repeat, authored-adaptive `perf stat -d` pass showed no obvious
branch-prediction, cache, or IPC collapse. These ratios are per-PMU ratios and
remain useful even though the scaled absolute hybrid counts from that pass
cannot be summed:

| Case | Revision | Atom/core IPC | Atom/core branch-miss rate | Core L1D miss rate |
| --- | --- | ---: | ---: | ---: |
| carpaint | `f66ecfb1f` | 2.08 / 2.39 | 0.92% / 1.03% | 0.28% |
| carpaint | HEAD | 2.06 / 2.42 | 0.90% / 0.96% | 0.31% |
| glass | `f66ecfb1f` | 1.98 / 2.35 | 1.06% / 1.03% | 0.39% |
| glass | HEAD | 2.05 / 2.47 | 0.99% / 0.96% | 0.40% |

This points to more executed work per camera sample, not a broad
microarchitectural cliff. It does not distinguish more path events from more
work per event.

## Exact `8e883c76f` stack comparison

The fixed-camera-sample glass render was recorded at `4b5b13aa7` and its direct
child `8e883c76f`:

```sh
/usr/bin/perf record -F 499 -e cycles:u -g --call-graph fp -- \
    usdrender <the fixed-camera-sample arguments above>
```

The plugin has no usable build ID in the perf data. A report made after another
revision overwrites the installed plugin silently resolves against the wrong
symbols. Exact binaries were therefore retained under:

- `/tmp/hdembree-symfs-4b5b13aa7/`
- `/tmp/hdembree-symfs-8e883c76f/`

Selected self-cycle percentages:

| Symbol | Parent atom/core | `8e883c76f` atom/core |
| --- | ---: | ---: |
| `EvalOpenPbr` | 6.31% / 6.88% | 6.41% / 6.88% |
| `_BuildShadingContext` | 4.81% / 4.54% | 4.98% / 4.49% |
| `_TryBuildSurfaceInteraction` | 2.97% / 3.32% | 3.45% / 3.34% |
| `_TryEvalSurfaceClosureAtHit` | 0.55% / 0.49% | **1.36% / 1.48%** |
| `ResolveObjectSpaceNormal` | 1.56% / 0.49% | 1.42% / 0.52% |
| `TransformNormalToWorld` | not separately hot | 0.62% / 0.67% |
| `ComputeSmoothTriangleShadowOffset` | absent | 0.35% / 0.37% |
| `PrepareShadingNormals` | absent | 0.10% / below 0.10% |

`PrepareShadingNormals` also contributes inlined work charged to its callers,
so its out-of-line self percentage understates its cost.

Do not add changes in self-cycle percentages and compare that sum with the
7.57% instruction increase. The two revisions have different total-cycle
denominators; inlined work is charged to callers; and the table lists only
selected symbols. The table identifies candidate hot paths but cannot quantify
their share of the regression or prove that path-event counts changed.

### Added work in `8e883c76f`

Two changes add plausible per-hit cost:

1. `_TryBuildSurfaceInteraction` now constructs a smooth shadow offset for
   every eligible unrefined, undisplaced triangle hit. It:
   - fetches a cached parametric frame;
   - fetches three corner normals;
   - transforms and normalizes all three normals;
   - aligns their hemispheres;
   - reconstructs triangle positions;
   - calls `ComputeSmoothTriangleShadowOffset`.
2. Every evaluated closure is traversed and mutated by
   `PrepareShadingNormals`. It prepares reflection-safe normals and tangent
   frames for each applicable BSDF lobe. The roughly threefold
   `_TryEvalSurfaceClosureAtHit` self-cost increase is consistent with this.

These stacks show that the new code executes and is non-trivial. They do not
show how much of the total delta is per-hit cost: `8e883c76f` also changes TIR,
specular-reflection validity, normal bases, and continuation validity. Renderer
event counts are required before assigning a percentage to either mechanism.

### Verified smooth-shadow triangle-input bug

The smooth-shadow call site does not currently satisfy
`ComputeSmoothTriangleShadowOffset`'s contract. The function requires genuine
corresponding world-space triangle corners and Embree barycentrics. The call
site instead gets `triangleDPdu` and `triangleDPdv`, reconstructs:

```text
p0 = posHitWld - u * dPdu - v * dPdv
p1 = p0 + dPdu
p2 = p0 + dPdv
```

and passes that pseudo-triangle with Embree's `u` and `v`.

For a triangle with a valid authored `st` parameterization, the mesh cache
contains `dP/ds` and `dP/dt`, not the barycentric edges `p1 - p0` and
`p2 - p0`. The formula still reconstructs the hit point by affine consistency,
but the corner-relative dot products used for lift and height are evaluated
against texture-parameterized vectors. The resulting offset therefore depends
incorrectly on the local UV Jacobian and can be grossly scaled or skewed.

Both investigated stages select the shaderball's `surface_geometry =
"triangulated"` variant. That mesh authors face-varying `primvars:st`, so both
profiles exercise this bug. The existing unit test calls
`ComputeSmoothTriangleShadowOffset` directly with real corners and therefore
cannot catch the call-site parameterization mismatch.

This changes the optimization baseline:

- do not preserve the current smooth-shadow inputs as behavior-neutral;
- first supply genuine triangle positions and re-baseline images and counters;
- add a call-site/data-path regression test whose result is invariant when only
  the triangle's UV scale changes;
- only A/B lazy evaluation against the corrected implementation.

Historical attribution remains separate. Renderer-work counters at the
original `4b5b13aa7` and `8e883c76f` revisions must measure those revisions
unchanged, including the bug, to explain the recorded regression.

## `793bcf237` object-position step

This direct child adds another 1.72% on carpaint and 2.24% on glass. For every
non-displaced shading-context build it now calls `rtcInterpolate1` on the
prototype vertex buffer to reconstruct object-space position. Previously it
only transformed the already available world-space hit position.

That interpolation is the likely source of this isolated step, but this is
inference from the diff rather than an exact parent/child stack comparison.
Record one before assigning the delta to `rtcInterpolate1`.

Do not reuse `triangleDPdu` and `triangleDPdv` as barycentric position edges.
When `st` exists those caches are texture-parameterized derivatives, not
necessarily `p1 - p0` and `p2 - p0`. A triangle fast path needs separate cached
position corners or barycentric edges. That same genuine-position
representation can serve the corrected smooth-shadow call and an object-space
position fast path; it should be designed and measured as one cache rather than
two unrelated caches. The existing ST derivative cache remains required by
material surface derivatives.

The stronger likely fast path is to record compiled-graph input requirements
and skip exact object-space position reconstruction unless the graph consumes
object-space position. Any FMA or cache replacement must retain the
procedural-cell boundary stability this commit intentionally fixes.

## Attribution blocker: renderer-work counters

Before implementing an optimization, add temporary instrumentation to both
`4b5b13aa7` and `8e883c76f`. Count at least:

- successful path surface interactions;
- continued path segments and total scattering depths;
- direct-light attempts;
- visibility/shadow rays actually traced;
- closure nodes visited by `PrepareShadingNormals`;
- closure nodes with authored normal or tangent state.

Avoid a relaxed atomic on every ray while measuring performance. Accumulate
per worker or per render tile and merge at frame completion. Use the
instrumented builds to compare event counts, not timings: instrumentation
overhead is acceptable if identical at both revisions. Then report:

```text
instructions / camera sample
instructions / surface interaction
instructions / visibility ray
closure nodes prepared / surface interaction
authored-normal nodes / closure nodes prepared
```

Until those counts exist, the `8e883c76f` result means only that the historical
commit executes 5.18% more instructions for carpaint and 7.57% more for glass
at the same camera-sample count. It still contains the triangle-input bug
described above.

## Build-type qualification

The bisection used `build-profile`: `RelWithDebInfo`, `-O3`, debug information,
and frame pointers. Release also uses `-O3`, but omits frame pointers and can
have different register pressure, code layout, and instruction counts.

Repeat the headline checkpoints in Release before calling +9.07% and +12.29%
user-visible regressions. Collect at least:

```sh
perf stat --no-scale -r 5 \
    -e instructions,cycles,task-clock -- usdrender ...
```

On this hybrid CPU, raw P-core plus E-core instructions remains the work
counter. Cycles and task-clock add IPC and aggregate CPU-work context; summed
cycles across concurrent cores at different frequencies do **not** directly
measure critical-path wall time.

## Existing benchmark reconciliation

`OPTIMIZATION.md` reports that the complete `8e883c76f` patch improved
`input_coat_darkening` Release wall and renderer time by 4.8% and 5.1%. That
entry explicitly covers one fixture and says it does not establish a general
result, so it is not proof against the AOUSD regression. The repository
nevertheless now contains opposite-looking conclusions for the same commit.

Rerun `input_coat_darkening` with raw hybrid-PMU counters and fixed camera
samples. Cross-link that result with this investigation and annotate whether
the old wall result was thermal/frequency noise or a real workload-specific
improvement. Preserve the historical measurement rather than replacing it.

## Optimization hypotheses and constraints

These are candidates to A/B after the historical renderer-work counts split
event-count changes from per-event cost and, for smooth shadows, after the
triangle-input correctness fix establishes a new baseline.

### Lazy smooth-shadow offset

`smoothShadowOffsetExt` is consumed only for the NEE visibility-ray origin, but
is constructed eagerly for every eligible surface interaction. Compute it once
only when a qualifying light sample is about to call `_Visibility`, or move its
construction to the latest equivalent point before direct lighting.

First replace the pseudo-triangle with genuine world-space corners and
re-baseline the resulting intended behavior. The subsequent lazy A/B must
preserve the corrected inputs and arithmetic; preserving the current inputs
would preserve a correctness bug.

The glass delta being larger is consistent with wasted work on vertices that
do not need a visibility ray, but is not evidence by itself because glass path
length also changed.

The existing barycentrically interpolated `normalSrfWldExt` cannot reconstruct
the three corner normals required by `ComputeSmoothTriangleShadowOffset`.
There is still duplicated sampling between smooth-normal resolution,
shadow-offset construction, and surface-derivative construction that may be
shared explicitly. Computing the complete correction in object space and
transforming only its result is not generally equivalent under non-uniform
transforms because the calculation mixes positions and normals through dot
products.

### O(1) default closure normals

For closure leaves without authored normals, diffuse-like leaves share the base
shading normal and specular leaves share one
`EnsureValidSpecularReflection(normalGeomWldOut, omegaOutWld, baseNormal)`
result. Store those two per-interaction defaults at tree level and visit only
nodes with authored normal/tangent state. Preserve authored-normal validation,
invalid-normal accounting, and the distinction between diffuse and corrected
specular defaults.

This is stronger than merely skipping trees with no authored normals, because
specular correction still depends on `omegaOutWld`.

### Object-space position requirements

Add compile-time graph input-requirement metadata so most materials can skip
object-space position reconstruction. For triangle graphs that require it,
evaluate a genuine cached `p0`, `p1 - p0`, and `p2 - p0` representation or
retain `rtcInterpolate1`; do not reinterpret the surface-derivative cache. A
genuine-position cache should also provide the smooth-shadow triangle corners.
Removing `_GetCachedTriangleParametricFrame` from the smooth-shadow path then
removes its two hit-time direction transforms, but does not remove the ST
derivative cache used by surface-derivative evaluation.

### Adjacent tangent lookup

`_BuildShadingContext` performs two fixed-token `primvarMap.find()` calls for
the authored tangent frame and two more when falling back to the computed
frame. This predates the measured regression and is not the dynamic geomprop
lookup forbidden by `AGENTS.md`, but cached sampler pointers would remove
known hit-time hash lookups while this path is being revisited.

## Original investigation order and deviation

The following order was specified before implementation but was not followed:
steps 4-7 proceeded without completing historical renderer-work counters in
step 1 or a historical Release comparison in step 2. The Release rows above
are corrected-HEAD measurements, not the missing `f66ecfb1f`/old-HEAD Release
baseline. This is a process deviation and is why causal attribution remains
provisional; the implementation and its measured corrected-HEAD delta must not
be read as satisfying those historical evidence requirements.

1. On the unchanged historical revisions, add renderer-work counters and
   compare `4b5b13aa7` with `8e883c76f`. Include both visited closure nodes and
   nodes with authored normal/tangent state.
2. Confirm the historical headline instruction, cycle, and task-clock deltas
   in Release.
3. In parallel with steps 1-2:
   - bisect the residual pre-`4b5b13aa7` 1.95-2.10% increase;
   - record exact parent/child stacks for `793bcf237`.
4. Fix the smooth-shadow triangle inputs on HEAD using genuine positions. Add
   a call-site/data-path UV-scale-invariance regression test, then re-baseline
   carpaint and glass images, renderer-work counters, and Release counters.
5. A/B lazy smooth-shadow construction against the corrected implementation.
6. A/B the O(1) default-normal path.
7. Add graph input requirements and evaluate the shared genuine-triangle
   position cache as the `posObj` fast path.
8. Rerun and reconcile the non-gating `input_coat_darkening` benchmark.

## Image differences

The authored-adaptive endpoint renders are not image-identical:

- carpaint: 23.9% of pixels differ
- glass: 4.82% of pixels differ
- glass adaptive convergence differs by 17 pixels

This is expected to some extent because `8e883c76f` deliberately changes
normal, tangent, transmission, and shadow-terminator behavior. It also
reinforces that equal camera-sample count is not equal transport work. The
fixed 32 spp results remove adaptive-sample-count variation but not path-length
or visibility-ray variation.

## Artifacts

Raw counter files:

- `/tmp/hdembree-bisect-*-glass-raw-instructions.txt`
- `/tmp/hdembree-bisect-*-carpaint-raw-instructions.txt`
- `/tmp/hdembree-{head,f66ecfb}-{carpaint,glass}-stat.txt`

Stack data and reports:

- `/tmp/hdembree-bisect-4b5b13aa7-glass.data`
- `/tmp/hdembree-bisect-8e883c76f-glass.data`
- `/tmp/hdembree-bisect-{4b5b13aa7,8e883c76f}-glass-report.txt`

All `/tmp` artifacts are ephemeral. While this investigation remains active,
copy exact DSOs beside any perf data that must survive a reboot, or budget for
a reproducible re-record. Do not commit machine-specific profiling binaries to
the source tree. The tables above are the durable result record.
