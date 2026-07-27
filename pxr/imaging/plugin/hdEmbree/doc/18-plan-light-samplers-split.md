# Plan: Split `lightSamplers.cpp` one implementation file per light type

Status: source-layout cleanup, scoped to `renderer/lights/lightSamplers.{h,cpp}`.
Companion to `17-plan-bsdf-split.md`. Split from the former combined
source-organization plan, which
listed this file as an undesigned "candidate".

**Decision:** Anders explicitly approved this measured source partition as a
scoped exception to Goal 1's line-reduction gate. The exception does not weaken
the general rule or pre-approve further shared interfaces; the performance,
image-diff, export-count, and documentation gates in this plan remain
mandatory.

This plan supersedes an earlier draft that split the file in two along an
infinite/finite boundary (dome + distant against everything else). That draft
rated itself marginal and it was: it removed three of the four
forward-declaration groups and left the type dispatcher still carrying
per-shape knowledge. The per-type partition below removes all four and empties
the dispatcher, for the same kind of work. The infinite/finite draft is
recorded under "Rejected alternative" at the end.

## Issue

`lightSamplers.cpp` is 1,646 lines: a 1,465-line anonymous namespace followed
by a thin `HdEmbreeLightSampler` dispatch surface (`:1488-1644`).

Length is not the complaint. Two concrete symptoms are:

**1. Definitions cannot be topologically ordered**, because six independent
light types are interleaved. Four forward-declaration groups exist only to work
around that:

| Forward declaration | Definition |
| --- | --- |
| `_EvalLightBasic` `:610` | `:806` |
| `_EvaluateDomeLightDirection` `:786`, `:791` | `:1378`, `:1411` |
| `_IntersectSphereLight` `:798` | `:1123` |
| `_EvalSphereLightSolidAngle` `:512` | `:994` |

**2. The dispatcher carries per-shape knowledge.**
`_EvaluateLightDirection` (`:1252-1309`) is meant to be a 6-way type switch. It
is not. The sphere branch (`:1267-1281`) returns early with its own
solid-angle PDF override, and a `useShapingAwareFinitePdf` local (`:1261`,
`:1305`) is set by the rect and disk branches so a shared post-step can run.
Both belong to the shapes, not the switch. The same leak appears in the visitor:
`operator()(HdEmbree_Rect)` (`:1543-1573`) and `operator()(HdEmbree_Disk)`
(`:1593-1622`) each inline a 28-line shaping-proposal split, while the other
five overloads are one-line forwards.

## The boundary: shared infrastructure versus per-type geometry

The partition is **shared infrastructure against per-type geometry**, and the
per-type side divides cleanly six ways.

The six light types share an *interface* (sample a direction, evaluate a
direction), not an implementation. What differs is geometry — where a point on
the shape is, where a ray hits the shape — and geometry does not compose across
shapes. What is shared is the radiometry applied once the geometry is resolved,
plus small clamping and finiteness helpers.

No symbol is claimed by a proper subset of light types in a way that would force
a shared-between-exactly-two file, with one exception: the shaping-aware PDF
block (`:819-939`) is used by rect and disk only, and is placed in common
because both callers need it and neither owns it. This is *not* the claim that
every symbol serves one type or all six — plenty of infrastructure has a single
caller. The precise placement rule and the verified export set are under
`lightSamplerCommon` below.

## Measurement

Every anonymous-namespace symbol, assigned:

| Owner | Symbols | Lines moved |
| --- | --- | --- |
| Rect | `_AreaRect` `:43`, `_SampleRect` `:403`, `_IntersectRectLight` `:1039`, `_SampleRectDirectionalShaping` `:1311`, + the shaping split in `operator()` `:1545-1572` | 134 |
| Sphere | `_AreaSphere` `:51`, `_SampleSphere` `:430`, `_CanSampleSphereBySolidAngle` `:457`, `_SphereSolidAngle` `:484`, `_EvalSphereLightSolidAngle` `:993`, `_IntersectSphereLight` `:1122`, + `operator()` `:1577-1590` and the dispatcher's sphere branch `:1267-1281` | 220 |
| Disk | `_AreaDisk` `:64`, `_SampleDiskPolar` `:519`, `_SampleDisk` `:527`, `_IntersectDiskLight` `:1081`, `_SampleDiskDirectionalShaping` `:1344`, + `operator()` `:1595-1621` | 137 |
| Cylinder | `_AreaCylinder` `:73`, `_SampleCylinder` `:551`, `_IntersectCylinderLight` `:1179`, + `operator()` `:1631-1638` | 118 |
| Distant | `:612-783` entire, 7 symbols | 172 |
| Dome | `:154-254` (5 symbols), `:263-360` (6 symbols), `:1377-1482` (3 symbols) | 305 |
| Common | `_pi` `:28`, small math, colour/blackbody `:85-124`, `_ShapeSample`, `_SampleLightTexture` `:362`, `_SampleRectLightTexture` `:381`, `_MakeAreaShapeSample`, `_InvalidLightSample`, `_EvalLightBasic`, shaping-aware PDF `:819-939`, `_EvalAreaLight` `:941-991` | 331 |
| Dispatch | `_EvaluateLightDirection` (shape branches removed), the class surface | 140 |

No symbol appears twice. The largest per-type file is dome at 305 lines; the
median is 155.

Two assignments are not obvious and are forced by the boundary, not chosen:

- **`_SampleRectLightTexture` (`:381`) goes to common, not rect.**
  `_EvalAreaLight` (`:965`) branches on
  `std::holds_alternative<HdEmbree_Rect>` to call it, and `_EvalAreaLight` is
  shared. A TU-local rect function cannot be called from common. The
  type-specific branch therefore stays in common, visible and commented as the
  wart it is. See "Risks" for the follow-up that removes it.
- **`_pi<T>` (`:28`) goes to common.** Nine of the extracted functions use it
  across five of the six types. It is declared in `lightSamplerCommon.h` as a
  plain `constexpr float`, which is what `23-plan-auto-types.md` wants it to
  become anyway — that plan's item for *this* file is then already done, and it
  sweeps only the remaining definitions elsewhere in the tree. A later plan
  cannot retroactively make this one build.

## Proposed design

Eleven files: six per-type `.cpp` implementations, one shared declaration
header, and two pairs.

### Six per-type implementations, no per-type headers

`rectLight.cpp`, `sphereLight.cpp`, `diskLight.cpp`, `cylinderLight.cpp`,
`distantLight.cpp`, `domeLight.cpp`.

Responsibility, stated at the top of each file: *sample and evaluate a
`<type>` light — its area formula, its point sampling, its analytic ray
intersection, and its shaping-aware proposal split where it has one.*

Each exports 2 functions (dome 3) and keeps everything else `_Foo`-prefixed and
TU-local. **Each has exactly one consumer, `lightSampler.cpp`**, so six
separate headers would be six include-guard ceremonies serving six include
lines in one file. The declarations go in one shared header instead.

This matches existing house style rather than deviating from it:
`renderer/integrator/{lighting,pathIntegrator,surfaceShading,unlitIntegrator,
visibility,volumeTransport}.cpp` and `renderer/aov/aovOutput.cpp` are already
bare `CPPFILES` entries (`CMakeLists.txt:70-80`) whose declarations live in a
shared private header.

### `lightSamplerDispatch.h` (~180 lines, declarations only)

Responsibility: *the per-type sample/evaluate entry points that
`HdEmbreeLightSampler` dispatches to.*

13 declarations: `ty::SampleXLight()` and `ty::EvaluateXLightDirection()` for
each of the six types, plus the second `ty::EvaluateDomeLightDirection()`
overload. No definitions. Nothing else may be added to it — it is a dispatch
interface, not a shared implementation header.

The name is deliberate: `lightSamplerTypes.h` would read as "declares the light
types", which `light.h` in the same directory actually does.

*Tension worth naming, so it is not re-litigated:*
`14-plan-renderer-impl-header.md` is dismantling `rendererImpl.h`, a shared
private declaration header. It is being dismantled for size — 1,364 lines, 62
**definitions**, and 4,940 lines of transitive includes imposed on 10 includers
— not for the pattern. 13 declarations in ~180 lines with 7 includers is not
that, and the "nothing else may be added" cap above is what keeps it from
becoming that.

The four finite shapes share one signature shape. Note the enclosing
`namespace ty { }` — a qualified name (`ty::SampleRectLight`) can only
introduce a *definition* of an already-declared entity, never an initial
declaration:

```cpp
namespace ty {

/// Samples a direction from `position` toward the rect light, returning
/// radiance, direction, distance, and the inverse solid-angle PDF.
///
/// `rect.width` and `rect.height` are expected positive and finite. When
/// `light.shaping.directionalDistribution` is valid, `u1` selects between an
/// area proposal and a directional-shaping proposal and is rescaled into the
/// chosen one; the returned PDF is the mixture of both.
///
/// Returns an invalid sample (`valid == false`) when either dimension is zero
/// or non-finite, when `position` lies on the unlit +Z side of the rect's
/// local plane, or when the directional proposal misses the rect.
HdEmbreeLightSampler::LightSample SampleRectLight(
    HdEmbree_LightData const& light,
    HdEmbree_Rect const& rect,
    GfVec3f const& position,
    float u1,
    float u2);

/// Evaluates the rect light along a fixed direction from `position`, for MIS
/// against a BSDF sample. Returns the same PDF `SampleRectLight()` would have
/// produced for that direction.
///
/// Returns an invalid sample when the ray misses the rect, is parallel to its
/// plane, or hits behind `position`.
HdEmbreeLightSampler::LightSample EvaluateRectLightDirection(
    HdEmbree_LightData const& light,
    HdEmbree_Rect const& rect,
    GfVec3f const& position,
    GfVec3f const& direction);

}
```

Two types do not fit that shape and must not be forced into it: **distant**
takes no `position`; **dome** takes no `position` but takes the receiver normal
and `SamplingMode`, and exports both `EvaluateDomeLightDirection` overloads
because `HdEmbreeLightSampler` makes both public statics.

**All 13 declarations get their own contract**, not "identical to rect". The
invariants and failure modes differ per type and each must be stated:

| Type | Contract must state |
| --- | --- |
| Rect | width/height expected positive and finite; zero or non-finite gives a zero area and therefore an invalid sample; `position` on the unlit +Z side of the local plane; directional proposal missing the shape |
| Disk | radius expected positive and finite; `radius == 0` gives a zero area and an invalid sample (the degenerate `(0,0)` uv at `:1116-1117` is incidental to that, not the failure) |
| Sphere | the solid-angle path requires a uniform-scale orthogonal transform (`_CanSampleSphereBySolidAngle` `:457`) **and** `position` strictly outside the sphere (`:497`); **otherwise it silently falls back to uniform-area sampling with a different PDF** — the single most surprising behavior in the file |
| Cylinder | no end caps, so a ray through the open ends misses; **has no shaping-aware proposal split at all**, unlike rect and disk, so a shaped cylinder is area-sampled only; radius and length expected positive and finite |
| Distant | `angle == 0` returns a **delta** sample (`delta = true`, `pdfSolidAngleInverse = 1`) that MIS must not weight; `EvaluateDistantLightDirection` on a delta light returns invalid unless `dot(direction, axis) >= 1 - 1e-5` (`:707-710`) — **the tolerance is on the cosine, so the angular half-width is ~4.5 mrad, not 1e-5 rad**; a degenerate light-to-world Z axis returns invalid |
| Dome | with no built distribution (`_HasDomeDistribution` `:155` false) it uniformly samples the sphere at pdf `1/4π`; in `ReflectionHemisphere` mode a zero or non-finite normal makes the fold return the zero vector and the sample **silently falls back to full-sphere evaluation** (`:1471-1481`); distance is always `float` max |

**"Positive and finite" is an input invariant, not a guarded precondition.**
Nothing validates shape dimensions. Do not write a contract that promises what
happens when the invariant is violated, because the two non-finite cases do not
even agree with each other: a NaN dimension yields a NaN
`pdfSolidAngleInverse`, and `pdfSolidAngleInverse > 0.0f` is false for NaN, so
the sample comes back invalid — but an *infinite* dimension yields an infinite
area and an infinite `pdfSolidAngleInverse`, and `inf > 0.0f` is **true**, so
the sample comes back valid carrying an infinite PDF. State the invariant and
declare behavior outside it unspecified. Adding real validation is a behavior
change and must not be smuggled into a layout plan; if it is wanted, it is its
own change with its own test.

`21-plan-api-contracts.md` does not cover these 13; it keeps `LightSample` and
the light-shape structs.

### `lightSamplerCommon.{h,cpp}` (~120 header / ~250 cpp)

Responsibility: *the radiometry every light type applies once its geometry is
resolved, plus the unit-interval and finiteness clamps they all share.*

**Two independent axes decide where each symbol goes. Do not conflate them.**

*Axis 1 — exported or TU-local.* A symbol called from any per-type `.cpp` must
be declared in `lightSamplerCommon.h` and therefore becomes `ty::Foo` per
`14-plan-renderer-impl-header.md`. A symbol whose only callers are inside
`lightSamplerCommon.cpp` stays `_Foo` and stays out of the header.

The exported set is exactly 14. The "called from" column lists only per-type
callers — uses from inside `lightSamplerCommon.cpp` itself do not justify a
header declaration and are excluded:

| `ty::` export | Called from |
| --- | --- |
| `Pi` (`inline constexpr float`) | sphere, disk, cylinder, distant, dome — **not rect**, whose area, sampling, and intersection are all linear |
| `Sqr` | sphere (`:1017`), cylinder (`:559`), distant (`:768`), dome (`:1456`) |
| `ClampUnit` | sphere (`:1015`, `:1018`), distant (`:766`, `:769`), dome (`:177`, `:183`, `:221`, `:277`, `:1455`) |
| `WrapUnit` | dome (`:175`, `:272`) |
| `IsFinite` | distant (`:623`, `:692`), dome (`:291`, `:306`, `:331`, `:352`, `:1382`, `:1422`) |
| `ShapeSample` (struct) | all four finite — it is the parameter type of `EvalAreaLight` |
| `MakeAreaShapeSample` | the four finite intersection functions |
| `InvalidLightSample` | all six |
| `ShapingAwareFiniteAreaProposalWeight`, `ShapingAwareFiniteDirectionalProposalWeight` | rect, disk |
| `EvalLightBasic` | distant (`:675`), dome (`:1397`, `:1434`) |
| `SampleLightTexture` | dome (`:1393`, `:1433`) |
| `EvalAreaLight` | rect, sphere, disk, cylinder |
| `ApplyShapingAwareFinitePdf` | rect, disk |

Re-derive this table from the actual call sites when implementing commit 2
rather than trusting it — an earlier revision of this plan had four of these
rows wrong while claiming they were verified. The *membership* is what the
completion criteria check, and every row still has at least one per-type
caller; only the attribution was wrong.

Constants are `inline constexpr`, not bare `constexpr`. A namespace-scope
`constexpr` is implicitly `const` and therefore has internal linkage in C++17,
giving each TU its own copy — harmless for a float today, but it contradicts
the `ty::` shared-entity convention and becomes an ODR trap the moment a
header-inline function references one.

TU-local to `lightSamplerCommon.cpp`, each having exactly one caller inside it:
`_DotZeroClip` (only `EvalAreaLight`), `_SampleRectLightTexture` (only
`EvalAreaLight`), `_GetLuminance` / `_BlackbodyTemperatureAsRgb` and the colour
objects (only `EvalLightBasic`), and the inner shaping-PDF chain
`_PdfSolidAngleFromInverse` / `_WorldToLocalDirectionPdfScale` /
`_DirectionalShapingPdfSolidAngle` / `_ShapingAwareFinitePdfSolidAngle` (only
`_ApplyShapingAwareFinitePdf`).

*Axis 2 — inline in the header or out-of-line in the `.cpp`.* This applies only
to the exported set and is a performance decision, not an interface one.
Definitions go in the header for `Pi`, `Sqr`, `ClampUnit`, `WrapUnit`,
`IsFinite`, `InvalidLightSample`, `MakeAreaShapeSample`, and the two weights —
they are per-light-sample-per-path-vertex and trivially small. `EvalLightBasic`,
`SampleLightTexture`, `EvalAreaLight`, and `ApplyShapingAwareFinitePdf` are
declared in the header and defined in the `.cpp`.

These four are **not** the plan's main cross-TU cost; see "The unavoidable
cost" under Validation before treating them as the perf question.

`_linRec709`, `_xyzColorSpace`, and `_rec709LuminanceComponents` (`:89-113`) are
namespace-scope objects with dynamic initialization and need exactly one
definition — they force the `.h`/`.cpp` pair regardless.

### `lightSampler.{h,cpp}` (~140 cpp)

Note the rename: `lightSamplers` → `lightSampler`. The file holds one class,
`HdEmbreeLightSampler`.

Keeps `HdEmbreeLightSampler` unchanged as the module's renderer-facing
interface (19 renames it `ty::LightSampler`), plus
`_EvaluateLightDirection` as a flat 6-way `std::get_if` chain with exactly one
call per branch and no locals. All seven `operator()` overloads become one-line
forwards:

```cpp
HdEmbreeLightSampler::LightSample HdEmbreeLightSampler::operator()(
        HdEmbree_Rect const& rect) {
    return ty::SampleRectLight(_lightData, rect, _hitPosition, _u1, _u2);
}
```

This is the file a reader opens first, and it becomes the one place that lists
every light type twice — once for sampling, once for direction evaluation —
with each entry naming the function that implements it.

### What the split actually buys

- **All four forward-declaration groups disappear.** The two sphere ones
  (`_EvalSphereLightSolidAngle` `:512`, `_IntersectSphereLight` `:798`) go too:
  alone in `sphereLight.cpp` the definitions order without a cycle. The call
  graph is `SampleSphereLight` → `_EvalSphereLightSolidAngle` →
  `_IntersectSphereLight` → `_MakeAreaShapeSample`, so the textual definition
  order is that chain reversed — intersection first, then the solid-angle
  evaluator, then the exported sampler. The earlier infinite/finite draft kept
  both of these declarations.
- **The dispatcher and the visitor stop knowing about individual shapes.** The
  sphere solid-angle override moves into `EvaluateSphereLightDirection`; the
  `useShapingAwareFinitePdf` local and the two 28-line shaping splits move into
  the rect and disk `Sample*` functions. Seven uniform one-line overloads.
- **"How does light X work" is a file name.** Today it is a bisection of a
  1,646-line anonymous namespace whose blocks are interleaved by concern
  (all the area formulas together, all the intersections together) rather than
  by type.

## Naming

Per `14-plan-renderer-impl-header.md`: symbols that become header declarations
move from `_Foo` to `ty::Foo`; symbols that stay local to one `.cpp` keep
`_Foo`. The 13 dispatch exports and the shared helpers in
`lightSamplerCommon.h` become `ty::`. Per-type internals (`_AreaRect`,
`_SampleDiskPolar`, `_IntersectCylinderLight`, …) stay `_Foo` — they are now
genuinely TU-local, which they were not before.

The `HdEmbree_`-prefixed *type* names in these files (`HdEmbree_LightData`,
`HdEmbree_Dome`, …) are untouched here; `19-plan-ty-namespace.md` renames them
(`ty::LightData`, `ty::DomeLight`, …). The mixed
`ty::SampleRectLight(HdEmbree_Rect const&, ...)` form this plan produces is an
interim state that 19 resolves — do not pre-empt it here, and do not treat it as
the endpoint.

**File naming: `rectLight.cpp`, not `rect.cpp`.** `light.h` in the same
directory defines `HdEmbree_Rect`, and a bare `rect.cpp` beside it reads as
geometry, not lighting.

**Do not rename the test.** `testenv/testHdEmbreeLightSamplers.cpp` and the
`testHdEmbreeLightSamplers` CTest target keep their names; they are quoted in
`AGENTS.md` and in developer muscle memory, and renaming them buys nothing.

## Sequencing

Nine commits, bottom-up so each one builds:

1. **Cylinder unit coverage** — `testHdEmbreeLightSamplers.cpp` has
   `_MakeDomeLight`, `_MakeSphereLight`, `_MakeRectLight`, `_MakeDiskLight`,
   and `_MakeDistantLight`, but **no `_MakeCylinderLight` and no cylinder
   test**. Cylinder is the file that establishes the interface shape in commit
   3, so it cannot be the one type with nothing verifying the move. Add
   `_MakeCylinderLight` plus a sample/evaluate PDF-consistency test in the
   style of `TestSphereSampleMatchesDirectionalEvaluation` (`:399`). Lands
   against the *current* code, so it is a real regression check for everything
   after it. Per `AGENTS.md`, run the adversarial test-review agent on this
   commit before continuing.
2. `lightSamplerCommon.{h,cpp}` — move the shared radiometry, `_pi`, and both
   texture lookups; no other change. `lightSamplers.cpp` includes it.
3. `cylinderLight.cpp` + `lightSamplerDispatch.h` — smallest type, no shaping,
   no solid-angle path. Establishes the interface shape and creates the
   dispatch header with its first two declarations.
4. `distantLight.cpp` — self-contained already (`:612-783` is one block).
5. `diskLight.cpp` — first type with a shaping split to relocate.
6. `rectLight.cpp` — second shaping split.
7. `sphereLight.cpp` — relocates the dispatcher's sphere branch.
8. `domeLight.cpp` — largest, and the only one exporting a public overload pair.
9. `lightSamplers.{h,cpp}` → `lightSampler.{h,cpp}` rename, and the dispatcher
   flattening that is only possible once 3-8 have landed.

Commits 4-8 are independent of each other and can be reordered; keep 1-3 first
and 9 last.

**Each move commit also audits the comments in the block it moves**, per
`AGENTS.md` Goal 4: a definition's logical sections must say WHAT and WHY, not
HOW. Several moved blocks carry HOW comments today (e.g. `:556`, "Compute
cylinder sample position _pi_ and normal _n_ from $z$ and $\phi$", which
restates the next line and misnames the variables after a PBRT copy). Fix them
where the code passes under your hands; change no algorithm. This does not
weaken the pure-move property that matters — comments cannot change behavior, so
a reviewer checking the move is genuinely a move diffs with comments stripped.
Do not open a separate comment pass; that doubles the commit count for a diff
nobody reads twice.

## Dependencies

- `05-plan-naming-core.md` — must land its `LightSample` quantity-family rename
  first. That plan renames fields across every function moved here; doing the
  move first doubles its diff.
- `14-plan-renderer-impl-header.md` — establishes the `ty` membership rule the
  new headers follow.
- `01-plan-build-surface.md` lands first, so all `CMakeLists.txt` entries are
  already private when this plan adds its own.
- Must precede `19-plan-ty-namespace.md`, which needs the final filenames and
  renames `HdEmbreeLightSampler` and the `HdEmbree_` light shapes, and
  `23-plan-auto-types.md`, whose `_pi` item for this file this plan completes.
- `21-plan-api-contracts.md` follows, and does **not** cover the 13 dispatch
  declarations — this plan writes those contracts in its own commits. 21 keeps
  `LightSample` and the light-shape structs.
- Independent of `16-plan-source-organization.md` and
  `17-plan-bsdf-split.md`; no shared files.

## Build changes

`renderer/lights/lightSamplers` is a `PRIVATE_CLASSES` entry once
`01-plan-build-surface.md` has landed, which it does before this plan.

- The six per-type implementations are header-less, so they go in **`CPPFILES`**
  beside the existing `renderer/integrator/*.cpp` entries — not in
  `PRIVATE_CLASSES`, which implies a `.h`/`.cpp` pair.
- `renderer/lights/lightSamplerCommon` goes in **`PRIVATE_CLASSES`**.
- `renderer/lights/lightSamplerDispatch.h` goes in **`PRIVATE_HEADERS`**, the
  block `01` creates when it converts `PUBLIC_HEADERS`.
- `renderer/lights/lightSampler` replaces `lightSamplers` in `PRIVATE_CLASSES`.

Three include sites follow the header rename: `renderer/renderer.h:15`,
`testenv/testHdEmbreeLightSamplers.cpp:8`, `CMakeLists.txt:87`.

## Validation

- **Do not claim bit-identical output.** `_ClampUnit`, `_WrapUnit`, `_Sqr`, and
  `_EvalAreaLight` are currently inlinable into the sampling paths and will not
  all be across TU boundaries; floating-point contraction can change.
- **Primary check, every commit:**
  `pixi run cmake --build build --target testHdEmbreeLightSamplers` and
  `pixi run ctest --test-dir build -R testHdEmbreeLightSamplers
  --output-on-failure`. After commit 1 this suite covers all six types. It is
  cheap; there is no reason to tier it.
- **Image check, tiered.** The fixtures are the per-type stages in
  `/home/anders/code/typhoon-tests/usdlux/`, which have committed EXR
  references under `usdlux/reference/`:

  ```sh
  cd /home/anders/code/typhoon-tests
  pixi run pytest usdlux --typhoon-provider /home/anders/code/openusd-omniverse
  ```

  A full sweep is **328 frames** (`rect`/`disk`/`sphere`/`cylinder` are 63
  frames each, `distant` 35, `iesLibPreview` 30, `visibleRect` 10, `dome` 1 —
  see `usdlux/typhoon-suite.toml`). Running that after all nine commits is
  ~2,950 renders and is not justified. Tier it:

  | Commit | Fixtures |
  | --- | --- |
  | 2 (common extraction) | all — it is the only commit every type depends on |
  | 3-8 (one type each) | that type's stage only, plus `visibleRect` for rect. `rect.usda` and `disk.usda` already animate the full `shaping:cone`/`focus`/`ies` set across their 63 frames, so they are their own shaping check — do **not** add `iesLibPreview`, which is a `SphereLight` |
  | 9 (rename + dispatcher flattening) | all — this is the commit that rewires dispatch |

  Do **not** cite `iesTest.usda`: `typhoon-suite.toml`'s `[skip]` block excludes
  it, along with `ies_scale`, `iesUp`, and `iesDown`.

  Record the pre-plan renders once (`--typhoon-dry-run -s` prints the exact
  `usdrender` command; run it with
  `-s "{settings}.ty:randomNumberSeed = 1"` into a baseline directory outside
  `/tmp`) and diff against that baseline, not against the shipped references,
  so contraction drift is measured against the code this plan started from:

  ```sh
  oiiotool --diff --fail 0.0005 --failpercent 0.05 <baseline>.exr <after>.exr
  ```

  Any non-zero difference must be explained, not assumed.
- **The unavoidable cost: 17 new cross-TU boundaries, not 4.** The four
  out-of-line `lightSamplerCommon` exports are the *smaller* half. The 13
  dispatch functions are the other half, and they are the ones that matter:
  today `operator()(Rect)`, `_SampleRect`, `_AreaRect`, and `_EvalAreaLight`
  are all in one TU and the compiler can inline the entire chain into the
  visitor. Afterwards, every light sample and every light evaluation makes at
  least one out-of-line call, on the outermost frame of that chain.

  **This cannot be inlined away.** It is inherent to putting the types in
  separate TUs; only LTO could recover it, and the Pixi Release configuration
  does not enable IPO. So the perf question is not "did I inline the right
  helpers" — it is *"is one out-of-line call per light sample material against
  the transcendentals and matrix transforms inside it?"* Expected answer: no,
  comfortably. But that is the measurement, and if it comes back material the
  correct response is to reconsider the split, not to tune helper inlining.

  Codex-style mitigation, recorded and **not** adopted: defining the
  `HdEmbreeLightSampler::operator()` overloads directly in their per-type
  `.cpp` files removes one layer of the call. It does not remove the boundary —
  `std::visit` still lands out-of-line — and it scatters one class's member
  definitions across six files, destroying the "one file lists every light
  type" property that is a stated benefit of this plan. Take it only if
  measurement forces the choice.

- **`perf stat` before commit 2 and after commit 9 only** — not per commit.
  Two workloads, because no single fixture covers the split:

  | Workload | Command source | Covers |
  | --- | --- | --- |
  | `material-fidelity/_assets/standard_shader_ball.usda` | the renderable wrapper — it authors `/Render/RenderSettings` and sublayers the full asset; get the exact `usdrender` line from `--typhoon-dry-run -s` per `AGENTS.md` | rect + common. `environment.usda` has **5 RectLights** and nothing else; the asset's sphere emitters are off by default |
  | `usdlux/dome.usda` | same | dome, the largest per-type file and the only textured-distribution path |

  Do not describe the shaderball as exercising "every moved line" — it
  exercises the rect and common paths and nothing else. *There is no many-light
  fixture in typhoon-tests* — every `usdlux` stage has exactly one light, and
  `usdlux/shapes/` is an empty directory. That is acceptable here because this
  plan does not touch light *selection*, only per-sample sampling, which runs
  once per light per path vertex.

  **"No material regression" means the after-mean wall time is within 1% of the
  before-mean**, on both workloads. `perf stat -r 5` only resolves that if the
  reported run-to-run standard deviation is well under 1% — quote it. If the
  intervals straddle the threshold, raise the repetition count and interleave
  before/after runs in one session rather than declaring a result.

  If a regression is real, **diagnose before fixing**: a `perf record`
  self-cost profile tells you whether the cost is in the 13 dispatch calls or
  the 4 common ones. Only the second is fixable by inlining, and even then
  promotion grows code size and can move cost rather than remove it — inline
  only the boundary the profile implicates, then remeasure. Do not revert the
  plan over a result that has not been attributed.
- One new test (commit 1) and no others. If review concludes the
  reflection-hemisphere folding code needs coverage it never had, that is its
  own change with its own adversarial test review.
- **The 13 dispatch calls, not the 4 common exports, are the perf question.**
  See "The unavoidable cost" above: the split necessarily makes every light
  sample and every light evaluation an out-of-line call, and no amount of
  `inline` on the common helpers changes that.

## Documentation

- `AGENTS.md` "Directory Map" — the `renderer/lights/light.h`,
  `lightRegistry.*`, and `lightSamplers.*` entry.
- `AGENTS.md` "Lights" — the dome sampling paragraph should name
  `domeLight.cpp`.
- `AGENTS.md` "Focused Tests" — no change (target name is unchanged); confirm.
- `ARCHITECTURE.md` — the lights source map.
- `doc/README.md` — this plan's entry and the `lightSamplers.cpp` hot-file
  serialization bullet.
- `README.md` — no change expected; confirm.

## Risks and decisions

- **Cost: ~+210 lines (12%), 11 files where there are 2.** Roughly 80 lines of
  ceremony plus ~180 lines of declarations and their contract comments, against
  1,746 lines today. This is a partition, not an abstraction, so Goal 1's "net
  reduction of twice what the abstraction adds" does not literally apply — but
  the increase is real. What is bought is the four forward-declaration groups,
  the empty dispatcher, and per-type addressability. *An earlier revision of
  this plan gave each type its own header; that cost ~+550 lines and 16 files
  for no navigational gain, since all six had the same single consumer.*
- **The interesting physics ends up in `lightSamplerCommon`, not the per-type
  files.** `_EvalAreaLight` (`:942-991`) owns the area-to-solid-angle Jacobian,
  the `normalize` division, the texture multiply, and the shaping evaluation.
  What remains in `rectLight.cpp` is geometry. That is a coherent division, but
  "how is a rect light sampled" stays a two-file answer. Accept it; the
  alternative is duplicating the radiometry six times.
- **`lightSamplerCommon` is the file most likely to rot into a junk drawer.**
  Cap its *header* at the 14 exports tabled above — that list is the file's
  contract, and anything added to it that fewer than two light types call
  belongs in the type that calls it. Its `.cpp` may grow only helpers private to
  those 14.
- **One wart the split relocates and does not fix.** `_EvalAreaLight` branches
  on `std::holds_alternative<HdEmbree_Rect>` (`:965`) to pick
  `_SampleRectLightTexture` over `_SampleLightTexture` — a per-type decision
  inside a shared function. Both lookups therefore stay in common. Do not try to
  fix it inside this plan: the fix is to have each shape store its
  texture-lookup uv in one convention, deleting `_SampleRectLightTexture` and
  the branch (~20 lines), and it silently changes rect texture orientation if
  the flip is got wrong. It wants its own commit, its own image diff, and
  `TestRectTextureOriginUsesLocalPositiveXY`
  (`testHdEmbreeLightSamplers.cpp:837`) as its guard — after this plan lands.

## Rejected alternative: infinite vs finite

The earlier draft split the file in two: `infiniteLightSamplers.{h,cpp}` (dome
+ distant, sampled in direction space, ~490 lines) against everything else
(sampled by area). The boundary is real — lights at infinity have no surface
point, no area measure, and no area-to-solid-angle Jacobian — and the interface
was narrow, 5 exported symbols.

It was rejected because it is dominated by the per-type split on both of its
own arguments:

- It removed three of four forward-declaration groups, leaving the two
  sphere-internal ones. The per-type split removes all four.
- It left `_EvaluateLightDirection` and the rect/disk `operator()` overloads
  carrying per-shape logic, since all the shapes stayed in one file.

Its remaining advantage — a reader need not know whether a light is finite to
find it — is not an advantage over per-type files, where the file name *is* the
light type.

## Completion criteria

- Eleven files: six per-type `.cpp`, `lightSamplerDispatch.h`, and the
  `lightSamplerCommon` and `lightSampler` pairs. Each carries its
  one-sentence responsibility at the top.
- `lightSamplerDispatch.h` declares exactly 13 functions inside
  `namespace ty { }`, each with its own tailored contract covering the failure
  modes tabled above — not "identical to rect".
- `lightSamplerCommon.h` declares exactly the 14 exports tabled above, and no
  symbol called from a per-type `.cpp` is missing from it.
- Every moved block's comments say WHAT and WHY, not HOW; no algorithm changed
  while moving.
- **No ordering-only forward declarations remain in any `.cpp`.** (Headers
  contain declarations by definition; the criterion is that no definition in a
  `.cpp` is preceded by a declaration of itself.)
- `_EvaluateLightDirection` is a `std::get_if` chain with one call per branch
  and no locals; all seven `operator()` overloads are one-line forwards.
- `testHdEmbreeLightSamplers` covers all six types and passes.
- Image diffs against the recorded `usdlux` baseline within tolerance at each
  commit's fixture tier, with any difference explained; `perf stat` after-mean
  within 1% of before-mean on **both** the shaderball (rect/common) and
  `dome.usda` workloads, with the standard deviation quoted, and any regression
  attributed to the dispatch or the common boundary before any fix.
- `CMakeLists.txt`, `AGENTS.md`, `ARCHITECTURE.md`, and `doc/README.md` match
  the final layout.

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
