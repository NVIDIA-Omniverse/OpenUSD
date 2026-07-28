# Plan: Decompose `rendererImpl.h` into owned modules

Status: compile-boundary and readability cleanup. Introduces the `ty` namespace.

## Issue

`renderer/rendererImpl.h` is 1,364 lines. Counting definitions at file scope:
**48 free functions** (overloads counted separately; 42 unique names),
**12 constants and tokens**, and **2 inline `HdEmbreeRenderer` member
definitions** — 62 in total, inside three anonymous-namespace blocks
(`:41-346`, `:350-1207`, `:1209-1332`) plus the member block at `:1334-1361`.

Ten translation units include it: `renderer.cpp`, `camera/camera.cpp`,
`aov/aovOutput.cpp`, and seven of the eight `integrator/*.cpp`
(`medium.cpp` does not).

It mixes unrelated concerns — generic math, normal transforms, subdivision
surface derivatives, closure classification and transparent-shadow policy,
spectral transport, Embree ray initialization, camera depth of field, SSS exit
rays, and contribution-clamping policy. A reader entering any renderer file
inherits all of them.

Three consequences beyond size:

- **Include fan-out.** Measured over project-local headers, `renderer.h`'s
  transitive closure is 117 files / 33,042 lines; `rendererImpl.h`'s is 146 /
  39,346. All ten TUs include both, so the figure that matters is what
  `rendererImpl.h` adds **on top of** `renderer.h`: **28 headers / 4,940 lines**.
  It is not the whole geometry stack — `geometry/context.h`,
  `geometry/displacementEvaluation.h`, and `integrator/sss.h` already arrive via
  `renderer.h:12-19` and are unaffected by this plan. What is uniquely
  attributable:

  | cluster | headers | lines | attributable to |
  | --- | --- | --- | --- |
  | `hd/meshUtil.h`, `hd/meshTopology.h`, `hd/geomSubset.h`, `hd/topology.h`, `hd/tokens.h`, all five `pxOsd/*` | 10 | 2,027 | `surfaceDerivatives` |
  | `geometry/meshSamplers.h`, `geometry/primvarSampling.h` | 2 | 572 | `surfaceDerivatives` |
  | `MaterialXCpp/graph.h`, `paramMap.h`, `graphTypes.h`, `nodeRegistry.h`, `slots.h`, `shadingContext.h`, `valueHelper.h`, `materials/bsdf.h`, `materials/adobeOpenPbr.h`, `textureSystem.h` | 10 | 1,157 | `closureClassification` |
  | `MaterialXCpp/spectral.h` | 1 | 369 | `heroWavelength` |
  | `config.h`, `materials/oiioTextureSystem.h`, `tf/singleton.h`, `tf/envSetting.h`, `tf/instantiateSingleton.h` | 5 | 815 | audit — may be unused |

  These measurements predate plan `12`. That plan deletes `config.h` and its
  Tf environment/singleton dependency chain, so reproduce the closure and
  totals before implementing this plan.

  The consumers do not include any of this themselves — `surfaceShading.cpp:9-12`,
  `aovOutput.cpp:9-11`, and `pathIntegrator.cpp:9-11` each include only
  `renderer.h`, `rendererImpl.h`, and one or two more — so the dependency is
  entirely transitive and invisible at each call site.
- **Per-TU duplication.** Anonymous-namespace entities in a header have internal
  linkage, so every definition is instantiated ten times — including the seven
  `static const TfToken`s at `:55-63`, constructed at static-init time in each TU.
- **Rebuild coupling.** Editing any domain rebuilds nearly the whole renderer.

## Goals

- Give every definition one owner: a single `.cpp`, or a named module that
  declares it in a header.
- Stop shipping the 28 headers / 4,940 lines that `rendererImpl.h` adds on top of
  `renderer.h` to TUs that use none of them. Dependencies `renderer.h` already
  supplies are out of scope.
- Replace the header's anonymous namespaces with the `ty` namespace so shared
  helpers are one entity rather than ten.
- Make `_Foo` mean "contained to this file" again, and make `static` say so at
  the declaration.

## Non-goals

- Do not change renderer behavior or the Hydra-facing API.
- Do not force helpers into `HdEmbreeRenderer` members.
- Do not rename types or move `ty` beyond the code this plan relocates.
  `19-plan-ty-namespace.md` is required and completes the pass.
- Do not treat inlining as a performance decision here; see Risks.

## The `ty` namespace

Extracted code moves into `namespace ty`, nested inside `PXR_NAMESPACE`:

```cpp
PXR_NAMESPACE_OPEN_SCOPE
namespace ty {
// Gf/Hd/HdEmbree/RTC names resolve unqualified, exactly as they do today.
}
PXR_NAMESPACE_CLOSE_SCOPE
```

Nested, not global scope. The `mxcpp` namespace
(`materials/MaterialXCpp/graph.h:15`) sits at global scope because it is
genuinely pxr-free — it has its own `mxcpp::Vec3f`, which is why `_ToGf`/`_ToMx`
exist. The helpers here take `GfVec3f`, `GfMatrix4d`, `RTCRayHit`, and
`HdEmbreePrototypeContext`; at global scope they would need a using-directive in
a header, which leaks into every includer.

Membership rule, to be recorded in `AGENTS.md`:

- `ty::Foo` — declared in a header, used by more than one translation unit.
- `static _Foo` at `ty` scope in a `.cpp` — contained to one translation unit.
- **No anonymous namespaces.**

```cpp
PXR_NAMESPACE_OPEN_SCOPE
namespace ty {

static float _DotZeroClip(float x) { ... }   // TU-contained
static constexpr float _kVolumePdfEps = 1e-7f;

}
PXR_NAMESPACE_CLOSE_SCOPE
```

`static` marks TU-containment **at the declaration**, which an anonymous
namespace cannot: `lightSamplers.cpp`'s anonymous namespace is 1,465 lines
long, so a reader at `:900` has no way to see that the function in front of
them is file-local. That is the same complaint this plan makes about
`rendererImpl.h`, and it is why the `_` prefix currently has to carry meaning
the language should carry.

Write `static` explicitly even on `const`/`constexpr` objects, where namespace
scope already implies internal linkage. Uniformity is the point — one marker,
present on every TU-contained definition, so its absence means "shared".

**Types are the one gap:** `static` cannot be applied to a `struct`, `class`, or
`enum`. TU-contained types are declared `_Foo` at `ty` scope with no `static`
and therefore have external linkage, so their names must be unique across the
plugin. There are six in the whole renderer today, all already distinct;
`19-plan-ty-namespace.md` carries the list and the grep that keeps them unique.

This plan applies the rule to the functions and constants it extracts. Types
keep their `HdEmbree` prefix at `PXR_NAMESPACE` scope **temporarily**;
`19-plan-ty-namespace.md` is required and completes the pass. State that in
`AGENTS.md` as a known interim state, not as the endpoint — the mixed
`ty::TryComputeDisplacedSubdivNormalDerivativesToWorld(HdEmbreePrototypeContext
const&)` form this plan produces is a waypoint.

The third clause is this plan's own defect generalized: `rendererImpl.h`'s
anonymous namespaces are in a *header*, which is why its seven
`static const TfToken`s are constructed ten times at static-init. In a `.cpp`
an anonymous namespace is merely a less visible `static`; in a header it is a
bug. Banning both leaves one rule instead of two.

Under `renderer/materials/MaterialXCpp/`, **`mxcpp` already serves the role
`ty` serves here** — it is that component's own namespace and exists for the
same reason. The rule there is `mxcpp::Foo` vs `static _Foo` at `mxcpp` scope;
there is no nested `mxcpp::ty`. Record both halves in `AGENTS.md` together, so
the rule reads as one convention with two homes rather than two conventions.

A reader seeing `_ComputeThing(...)` then knows it is defined in the file they
are already reading, and `ty::ComputeThing(...)` means the definition is
elsewhere. Today `_Foo` means both. Extracted names drop the `_` prefix; call
sites become explicitly qualified (`ty::ResolveObjectSpaceNormal(...)`) rather
than relying on a using-directive.

This plan puts functions and constants in `ty`. Types are untouched here — they
keep their current names, prefixes, and files until the required
`19-plan-ty-namespace.md` renames them. `_HeroWavelengthState` stays at
`renderer.h:107-112`; `19` makes it `ty::HeroWavelengthState`.

## Call-site inventory

Measured over the ten including TUs, counting header-internal uses separately,
so private copies in files that do not include this header (`_DotZeroClip` in
`lights/lightSamplers.cpp:38`, `_IsFinite` in `delegate/adaptiveSubdivision.cpp`
and `geometry/displacementEvaluation.cpp`) are excluded. Reproduce this table
before moving anything; a plain name grep overstates consumers.

### Delete (no external and no internal use)

| line | symbol |
| --- | --- |
| 52 | `_rayHitContinueBias` |
| 341 | `_DotZeroClip` |

Nothing else is dead. Symbols with no external consumer but internal uses
(`_TryBuildSurfaceNormal`, `_IsReflectionOnlyNode`, `_InterpolateSubdivPosition`,
`_TryComputeSubdivLimitNormal`, `_IsEffectivelyZero`, `_IsEffectivelyOpaque`,
`_DifferenceOfProducts`) move with their group.

### Single consumer — into that `.cpp`, no new file

| lines | contents | destination |
| --- | --- | --- |
| 130-210 | `_IsCameraDepthOfFieldEnabled`, `_GetLensRadius`, `_SampleUniformDiskConcentric`, `_ApplyCameraDepthOfField` | `camera/camera.cpp` |
| 55-58, 1117-1211 | the four tangent/bitangent tokens, `_ComputeScreenSpaceDerivatives` | `integrator/surfaceShading.cpp` |
| 454-490 | `_TransparentShadowTransmission`, `_CombinePresenceAndTransmissionVisibility` | `integrator/visibility.cpp` |
| 1277-1318 | `_PopulateSssExitRayHit` | `integrator/pathIntegrator.cpp` |
| 1319-1331 | `_CosineWeightedDirection` | `integrator/unlitIntegrator.cpp` |
| 54 | `_volumePdfEps` | `integrator/volumeTransport.cpp` |
| 60-63 | `_tokensDielectricLayerThroughputMode*` | `renderer.cpp` |

`_tokensSt` (`:59`) is the one exception in this table: it is **not**
single-consumer after the split. `surfaceShading.cpp` uses it once, and the
`surfaceDerivatives` block uses it at `:780`, `:933`, and `:1070`. Do not export
it — define a file-local `_tokensSt` in each of the two `.cpp` files. That
matches existing practice: `delegate/mesh.cpp` and
`geometry/displacementEvaluation.cpp` already carry their own private copies.

These keep `_` prefixes and become `static` at `ty` scope in their owning
`.cpp` files. `_ComputeScreenSpaceDerivatives`
has exactly two call sites, `surfaceShading.cpp:172` and `:602`, and none in
`camera.cpp` — the "camera-projection fallback" in its comment describes its
inputs, not its owner. The two transparent-shadow helpers are `visibility.cpp`-only
and stay out of the shared closure module; they call into it and into
`rendererMath.h`.

### New module: `renderer/geometry/surfaceDerivatives.{h,cpp}`

Lines 618-1116, ~500 lines — the largest block and the reason the geometry
include stack is global today.

**Declared in `surfaceDerivatives.h`** (the module's whole interface — four
functions): `ty::ResolveObjectSpaceNormal`, `ty::ComputeTriangleSurfaceDerivatives`,
`ty::ComputeSubdivSurfaceDerivatives`,
`ty::TryComputeDisplacedSubdivNormalDerivativesToWorld`.

**Static at `ty` scope in `surfaceDerivatives.cpp`**, keeping their `_` prefixes
because they have no consumer outside it: `_InterpolateSubdivPosition`,
`_TryComputeSubdivLimitNormal`, `_TryBuildSurfaceNormal` (moved from `:110-128`).

**Deleted:** `_IsSubdivMesh` (`:618-623`). Its entire body is
`return prototypeContext->refined;`. It has one external caller
(`surfaceShading.cpp:462`) and one internal one (`:729`); replace both with
`prototypeContext->refined` directly. Exporting a one-line field accessor across
a module boundary, or keeping a helper that only forwards a member read, both
fail `AGENTS.md` Goal 2 — the reader jumps to a function to learn nothing.

| consumer | uses |
| --- | --- |
| `integrator/surfaceShading.cpp` | `_ResolveObjectSpaceNormal`, both `_Compute*SurfaceDerivatives` |
| `aov/aovOutput.cpp` | `_ResolveObjectSpaceNormal` |
| `integrator/pathIntegrator.cpp` | `_TryComputeDisplacedSubdivNormalDerivativesToWorld` |

**The header is declaration-only.** Signatures take `HdEmbreePrototypeContext const*`,
`HdEmbreeInstanceContext const*`, `HdEmbreeDisplacedSubdivFrame*`,
`RTCRayHit const&`, `RTCScene`, and `GfVec3f` — no `mxcpp::ShadingContext`; none
of these functions takes one. Forward declarations plus the two small Embree
headers that define `RTCRayHit` and `RTCScene` suffice. Only
`surfaceDerivatives.cpp` includes `hd/meshUtil.h`, `geometry/meshSamplers.h`,
`geometry/primvarSampling.h`, and `geometry/displacementEvaluation.h`.

Expected effect, scoped to what is actually attributable: the seven TUs that use
none of this (`renderer.cpp`, `camera.cpp`, `lighting.cpp`, `sss.cpp`,
`unlitIntegrator.cpp`, `visibility.cpp`, `volumeTransport.cpp`) shed the 12
headers / 2,599 lines in the first two clusters above. They keep everything
`renderer.h` supplies, including `geometry/context.h` and
`geometry/displacementEvaluation.h` — this plan cannot and does not change that.
The three consumers above must add
explicit includes for whatever they use directly today via transitive inclusion —
their include lists may grow. That is the intended outcome, not a regression:
each TU's dependencies become visible at the top of the file.

### New module: `renderer/integrator/closureClassification.{h,cpp}`

Lines 425-438 and 491-582, ~110 lines.

**Declared in `closureClassification.h`** (two functions):
`ty::IsReflectionOnlyClosure`, `ty::IsVolumeOnlyBoundary`.

**Static at `ty` scope in `closureClassification.cpp`**:
`_reflectionOnlyEps`, `_IsEffectivelyZero`, `_IsEffectivelyOpaque`,
`_IsReflectionOnlyNode`. The recursive node walker is an implementation detail
of the two exported predicates; the transparent-shadow helpers moving to
`visibility.cpp` do not need it — they use only `_Clamp01` and `_ToGf` from
`rendererMath.h`.

Consumers: `lighting.cpp` and `pathIntegrator.cpp` (`IsReflectionOnlyClosure`),
`pathIntegrator.cpp` and `visibility.cpp` (`IsVolumeOnlyBoundary`).

`07-plan-closure-classification.md` rewrites the `std::visit` at `:491-556` and
must land first; this then becomes a file move with no logic change — **except
for its test.** `07-plan:129` designs that test around `_IsReflectionOnlyNode`
being an inline function in an anonymous namespace in a header, reachable by
including it. Once it is `.cpp`-local that access disappears. Retarget the test
at `ty::IsReflectionOnlyClosure` through the module's supported boundary rather
than exporting the recursive helper for testing; the leaf cases 07 enumerates are
all reachable through a single-node closure tree.

### New header-only modules (inline, no `.cpp`)

| file | source lines | contents | consumer TUs |
| --- | --- | --- | --- |
| `renderer/rendererMath.h` | 49-50, 65-108, 353-390, 439-453, 583-594 | `_pi`, `_IsFinite`, `_TryNormalizeDirection`, `_DifferenceOfProducts`, `_Clamp01` (2), `_ToGf`, `_ToMx` (4) | the seven `integrator/*.cpp`, plus `camera.cpp` **after** step 2 — the moved DOF block calls `_pi<float>` (`:163`) and `_IsFinite` (`:181`). Not `renderer.cpp` or `aovOutput.cpp` |
| `renderer/integrator/transportPolicy.h` | 53, 392-424 | `_minLuminanceCutoff`, `_IsNearlyBlack`, `_ClampFireflyContribution`, `_GetMultiSampleMisLightPdf` | lighting, sss, visibility, pathIntegrator, volumeTransport |
| `renderer/rayUtil.h` | 211-224, 1212-1276 | `_CalculateHitPosition`, `_PopulateRay`, `_OffsetRayOrigin`, `_PopulateRayHit` | renderer, aovOutput, surfaceShading, unlitIntegrator, visibility, volumeTransport, pathIntegrator |
| `renderer/heroWavelength.h` | 595-617, 1334-1361 | `_RgbToSpectralValue`, `_SpectralValueToRgb`, `_SpectralScalarToRgb`, and the two inline `HdEmbreeRenderer` members | lighting, pathIntegrator, volumeTransport, sss |
| `renderer/geometry/normalTransforms.h` | 225-339 | `_TransformNormalToWorld` (2), `_TransformNormalToObject` (2), `_TransformNormalDerivativeToWorld` | aovOutput, sss, surfaceShading |

Notes on placement:

- All four `_ToMx` overloads must end up together. One of them
  (`_ToMx(GfMatrix4d)`, `:583`) is currently stranded in the middle of the
  closure block.
- **Constants moving into a shared header become `inline constexpr`.** At
  namespace scope a plain `constexpr` implies internal linkage, so each includer
  would still get its own copy and the "one entity, not ten" goal would not hold.
  This applies to `_pi` and `_minLuminanceCutoff`; `_reflectionOnlyEps` becomes
  `.cpp`-local and needs no change. The project builds at C++17, so
  `inline constexpr` is available. This is the **only** declaration-spelling
  change permitted while moving bodies — everything else is a verbatim move.
- `_CalculateHitPosition` takes `RTCRayHit const&`; it is Embree ray code, not
  generic math.
- The transport-policy four are contribution and cutoff *policy*, not math.
  Keeping them out of `rendererMath.h` is what gives that header a one-line
  responsibility: pure numeric and type-conversion leaves with no renderer
  semantics. `_GetMultiSampleMisLightPdf` could argue for `sampling/sampling.h`,
  but `08-plan-sampling-dispatch.md` owns that file; do not create the conflict.
- **`heroWavelength.h` lives at `renderer/` root, not `integrator/`, and includes
  `renderer.h`, not the reverse.** The three
  conversions take `_HeroWavelengthState const&` and read its members, so they
  need the complete type, which lives at `renderer.h:107-112`. Do *not* move the
  POD into the new header and have `renderer.h` include it: `renderer.h` is
  included by 15 files including `delegate/renderPass.h`,
  `delegate/renderDelegate.h`, and `delegate/light.cpp`, and it does **not**
  include `MaterialXCpp/spectral.h` today — that arrangement would push 369 lines
  of spectral implementation into the entire delegate. Including `renderer.h`
  from `heroWavelength.h` has no cycle, keeps the functions inline, needs no new
  TU, and costs nothing: all four consumers already include `renderer.h`. The two
  inline `HdEmbreeRenderer` members move here for the same reason — they need
  both the complete class and the spectral helpers. Root placement, as a sibling
  of `renderer.h`, is what `rendererImpl.h` does today; an `integrator/` header
  including the top-level façade would be inverted layering, a root-level
  implementation header including its sibling is not.
- **The member definitions are not in `ty`.** `HdEmbreeRenderer` is declared at
  `PXR_NAMESPACE` scope, so `HdEmbreeRenderer::_ApplyPathWeight` and
  `_GetPathThroughputRgb` must be defined there too. `heroWavelength.h` therefore
  has two blocks: `namespace ty` for the three free conversions, then plain
  `PXR_NAMESPACE` scope for the two members. State this in the file's header
  comment; it is the one place in this plan where a header carries both.
- Fallback if measurement contradicts the header-only choice: split into a
  `PRIVATE_CLASSES` pair with declarations plus a forward-declared
  `_HeroWavelengthState` in the header and `renderer.h` + `spectral.h` in the
  `.cpp`. That trades inlining of per-bounce transport for dropping 369 lines
  from four TUs. Do not do it speculatively — the four consumers already include
  `renderer.h`, so the header-only form costs them nothing today.
- Do not name it `renderer/spectral.h`; that collides confusingly with
  `renderer/materials/MaterialXCpp/spectral.h`, which it includes (369
  self-contained lines: `mathTypes.h` plus std).
- **Placement follows semantics, not a consumer census.** `transportPolicy` and
  `closureClassification` encode transport policy, so they live under
  `integrator/` even though `closureClassification` operates on compiled `mxcpp`
  closures — `AGENTS.md` scopes `materials/` to material conversion and
  evaluation. `heroWavelength` is the exception noted above: it defines
  `HdEmbreeRenderer` members and so belongs beside `renderer.h` at root. `rendererMath` and `rayUtil` are domain-neutral
  leaves and stay at `renderer/` root even though every current `rendererMath`
  consumer happens to be an integrator; otherwise a module would migrate every
  time a consumer is added or removed.

These are not tiny headers, so the "no new family of tiny headers" non-goal is
not in play: projected at ~135, ~130, ~100, ~55, and ~50 lines with boilerplate,
against a `renderer/` median of ~120 (`geometry/context.h` 126,
`integrator/sss.h` 122, `integrator/medium.h` 137). The genuinely small headers
here are `materials/mxcppAdapter.h` (20) and `materials/material.h` (24).

Size is not the admission test, though. Each module must have **two or more
consumer TUs and a one-line responsibility**; all five do. If `06`-`12` reduce
any of them to a single consumer before this plan runs, fold it into that TU
instead.

## Implementation sequence

One commit per group, each independently buildable.

1. Reproduce the call-site table above and delete the two dead symbols.
2. Move single-consumer code into its owning `.cpp` (one commit per destination
   file). No `ty`, no new files, no body changes.
3. Add `rendererMath.h`, then `transportPolicy.h`, `rayUtil.h`, in `namespace ty`.
4. Add `heroWavelength.h` (including `renderer.h`), and move the two inline
   `HdEmbreeRenderer` members into it. `renderer.h` is not edited.
5. Add `normalTransforms.h`.
6. Add `surfaceDerivatives.{h,cpp}`, then `closureClassification.{h,cpp}`.
7. Delete `rendererImpl.h`; drop includes and tokens each TU no longer needs.
8. Record the `ty` membership rule in `AGENTS.md`; update the `Directory Map`
   entry describing `renderer/rendererImpl.h`, and the matching
   `ARCHITECTURE.md` text.
9. Record before/after header line count, definition count, and per-TU include
   counts.

**CMake.** The two `.h`/`.cpp` pairs go in `PRIVATE_CLASSES`
(`CMakeLists.txt:156`), alongside the existing `renderer/integrator/sss`,
`renderer/materials/oiioTextureSystem`, and `renderer/debugCodes` entries. Do
**not** put them in `CPPFILES` + `PUBLIC_HEADERS`. After
`01-plan-build-surface.md` there is no `PUBLIC_HEADERS` list to put them in;
before it, doing so would install internal headers as public API.
Header-only modules need no CMake entry at all — `rendererImpl.h` appears
nowhere in `CMakeLists.txt` today.

## Dependencies

Land before this plan, because each edits or annotates code this one relocates:

- `05` — naming, for the quantity names inside the moved bodies.
- `07` — closure classification; rewrites `:491-556` and adds the test this plan
  cites.
- `09` — comment/dead-code fixes at `:800`, `:891`, `:929`, `:957`, all inside
  the `surfaceDerivatives` block.
- `10` — displacement cleanup; edits `:692`, `:700`, `:738`, `:903`, `:1061`,
  also inside that block.
- `12` — render-settings state; removes an include that only `rendererImpl.h`
  pulls in today (`12-plan:384-385`).

Runs alongside or after:

- `06` — owns the triplicated hit-context lookup extraction. Rebase on it; do
  not re-extract.
- `23` — the `auto` and `_pi` sweep runs after this, over wherever the code
  landed.
- `19` — required; completes the pass over `renderer/`, renames the types, and
  converts the remaining anonymous namespaces to `static`.

## Validation

- Build per commit; that is what exposes missing dependency assumptions.
- Focused tests by moved domain:
  - `surfaceDerivatives` → `testHdEmbreeSubdivision`, `testHdEmbreeWireframe`
  - `closureClassification` → **the table-driven classification test added by
    `07`**. Do not cite `testMaterialXCpp`: `07-plan:117-122` establishes that it
    compiles `rendererImpl.h` transitively but never calls the classification
    functions, so building it proves nothing.
  - `rendererMath.h` / `rayUtil.h` → no focused coverage exists.
    `testHdEmbreeSampling.cpp:7` includes only `sampling.h`, so running it is
    build-regression coverage, not validation of these modules. They are covered
    by compilation plus the final renderer comparison unless direct tests are
    deliberately added.
  - `renderer.cpp` token move → `testHdEmbreeRenderSettings`
  - `heroWavelength.h` → fixed-seed render of a spectral/dispersion scene; no
    unit test covers hero-wavelength transport.
- One fixed-seed comparison at the end of the sequence across representative
  surface, volume, SSS, and AOV scenes, not after every move.
- **Transitive dependency measurement is mandatory**, and `grep -rl` does not
  provide it — direct includers say nothing about what each TU actually compiles.
  Use the build's own depfiles (`ninja -t deps` in `build/`, or the `.d` files
  under `CMakeFiles/`) to capture each of the ten TUs' full header set before and
  after, and report the per-TU delta against the 28-header / 4,940-line budget in
  the Issue section. Reduced coupling is the point of the change, so it gets a
  real number.
- One `perf stat -r 5` comparison after step 6 — the only step that changes
  linkage. Per `AGENTS.md`, use more than one workload (material-heavy,
  texture-heavy, many-light, deep-path), hold resolution/samples/bounces/adaptive
  settings fixed, pass
  `-s "{settings}.ty:randomNumberSeed = 1"` to `usdrender`, and compare
  renderer-reported time and samples per second alongside end-to-end wall clock.
  Record the baselines in `OPTIMIZATION.md`.

## Risks and decisions

- **Out-of-line is the decision, not an open question.** `surfaceDerivatives`
  and `closureClassification` get real TUs with out-of-line definitions. Marking
  them `inline` in their headers would restore the implementation fan-out this
  plan exists to remove. If the `perf stat` above shows a measurable regression,
  reconsider the specific function that caused it as a separate change with its
  own measurement — `_ResolveObjectSpaceNormal` is the one per-hit-leaf function
  in the set and the likeliest candidate.
- **Bit-exactness: require exact equality first, everywhere.** Steps 1-5 keep
  every definition inline and must be bit-identical. Step 6 crosses a TU
  boundary, which *can* change floating-point contraction — but do not
  pre-authorize a tolerance, or a genuine relocation error will pass as numeric
  noise. Run `oiiotool --diff` for exact equality. If it differs, identify the
  specific function and show the difference is contraction (for example, that it
  disappears under `-ffp-contract=off`), document that in the commit, and only
  then agree a specific metric and threshold.
- Do not consolidate `_TryNormalizeDirection` with the medium code's
  `_SafeNormalized`; they have different normalization semantics and are
  deliberately separate.
- Do not create a dependency from `geometry/` back to `integrator/`.

## Completion criteria

- `rendererImpl.h` no longer exists. Each replacement module has an explicit
  inventory, a one-line responsibility, and minimal dependencies; line count is
  not itself a criterion.
- Each domain reads from its owning module without unrelated context, and each
  module's header declares only its interface — the `.cpp`-local inventories
  above are what enforce the `_Foo` / `ty::Foo` rule.
- Per-TU depfile deltas are recorded against the 28-header / 4,940-line budget
  attributable to `rendererImpl.h`, reporting removed transitive headers and
  added replacement-module headers as **separate** columns — every TU necessarily
  gains `rendererMath.h` or similar, so a raw count would look like a regression.
  Targets: net transitive dependency count falls for all ten TUs; the seven
  non-consumer TUs lose the 2,599-line mesh/primvar cluster; and no TU acquires an
  implementation dependency unrelated to what it calls. Anything `renderer.h`
  already supplies is out of scope for this criterion.
- The two new modules are in `PRIVATE_CLASSES`; nothing was added to
  `PUBLIC_HEADERS`.
- `_` prefixes on **functions and constants** appear only on genuinely
  TU-contained ones, and every such definition is `static` at `ty` scope. Types
  are out of scope, so `_HeroWavelengthState` and the `HdEmbree`-prefixed structs
  are untouched here.
- This plan introduces no new anonymous namespace, in a header or a `.cpp`.
- `AGENTS.md` records the membership rule and marks the type situation as an
  interim state that `19` completes.

## Mandatory final suite gate

After every plan-specific validation above, run the complete Typhoon suite as
the final gate:

```sh
cd ~/code/typhoon-test-suite
powerprofilesctl launch --profile performance -- pixi run pytest --renderer typhoon-local
```

All tests must pass. The expected baseline is approximately 235 seconds. If any
test fails or runtime is 250 seconds or above, stop: do not continue or land
the plan. Check with Anders before proceeding.
