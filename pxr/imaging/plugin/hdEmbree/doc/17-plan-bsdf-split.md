# Plan: Split `bsdf.cpp` into a `bsdf/` directory of layered modules

Organizing idea: one module per kind of BSDF. The layout is not purely that —
`mathPrimitives`, `shadingFrame`, and `fresnel` are foundations rather than
kinds, and conductor and generalized Schlick share one module — so the title
says "layered modules" and the by-kind rule is stated where it applies.

Status: source-layout cleanup, scoped to
`renderer/materials/MaterialXCpp/materials/bsdf.cpp`. Companion to
`18-plan-light-samplers-split.md`. Split from the former combined
source-organization plan, which listed
this file as an undesigned "candidate".

Note on the path: this file is
`renderer/materials/MaterialXCpp/materials/bsdf.cpp`. It is unrelated to the
vendored `renderer/materials/BSDL/include/BSDL/bsdf_*.h` family, which this
plan does not touch.

Note on `Bsdf`: `bsdf.h:19` declares `namespace Bsdf` inside `mxcpp`, not a
class. `Bsdf::EvalGGXSpecular` and friends are free functions in a nested
namespace, and `Bsdf::DielectricData`, `Bsdf::ClosureTree`,
`Bsdf::BsdfSample`, and `Bsdf::NodeId` are namespace-scoped types. This plan
says "the `mxcpp::Bsdf` namespace API" throughout and never "the Bsdf class".

## Issue

`bsdf.cpp` is 5,107 lines. It is also the translation unit that includes the
three committed lookup tables:

| Header | Lines |
| --- | --- |
| `bsdfDielectricBothLut.h` | 2,085 |
| `bsdfDielectricTransmissionLut.h` | 2,088 |
| `bsdfDielectricReflFrontLut.h` | 615 |
| **total included table data** | **4,788** |

plus a fourth table, `_kGgxMissingEnergy`, authored inline at `bsdf.cpp:38-120`.
So the real size of the unit a reader or compiler faces is about 9,900 lines,
of which roughly half is constant data with no algorithmic content.

The file contains at least twelve distinct subjects — microfacet
distributions, Fresnel formulas, reflection-only interfaces, dielectrics, thin
film, diffuse models, sheen, energy compensation, a legacy compatibility path,
the closure-tree walk, and the public namespace API. None of them is marked. A
reader looking for "how do we sample a rough dielectric" has to know it is at
`:2239`, and a reader changing the Charlie sheen distribution has to know it
is 1,900 lines away from where sheen is evaluated.

## Two splits were considered. The layered one fails; the by-kind one works.

### Rejected: traversal above, "lobe math" below

The obvious split separates closure-tree traversal (`_EvalNode`,
`_EvalThroughput`, `_ApproxWeight`, `_PdfNode`, `_SampleNode`,
`_CausticClassPruner`, `bsdf.cpp:2823-4473`) from everything below it as one
undifferentiated lobe layer.

Counting every `_`-prefixed symbol defined below and referenced from
traversal: **64 symbols**, in one header. That is the catch-all `bsdfImpl.h`
the project rejects — it relocates the problem rather than naming anything.
**Reject.** Record the number so this is not re-proposed as-is.

### Accepted: one file per kind, with traversal as its own kind

Those 64 symbols are not an undifferentiated blob. Partitioned by subject,
traversal's fan-out becomes ten small, named, individually reviewable
interfaces. Same total count, different proposition: nobody reads a 64-symbol
header, they read the one header for the kind they are changing.

Full cross-module dependency matrix, counting distinct symbols a consumer
needs from a provider. Modules are listed in dependency order:

```
consumer \ provider    math   frame fresnel  energy thinFilm microf   sheen diffuse reflOnly dielec  legacy   trav     api   lines
math                      .       .       .       .       .       .       .       .       .       .       .       .       .      67
frame                     2       .       .       .       .       .       .       .       .       .       .       .       .     149
fresnel                   3       1       .       .       .       .       .       .       .       .       .       .       .     121
energyCompensation        2       .       1       .       .       .       .       .       .       .       .       .       .     614
thinFilm                  7       .       1       .       .       .       .       .       .       .       .       .       .     283
microfacet                2       3       .       1       .       .       .       .       .       .       .       .       .     434
sheen                     1       .       .       .       .       .       .       .       .       .       .       .       .      34
diffuse                   1       1       .       .       .       .       .       .       .       .       .       .       .      94
reflectionOnlyInterfaces  2       1       1       .       3       .       .       .       .       .       .       .       .      78
dielectric                3       5       3       2       3       7       .       .       .       .       .       .       .     684
legacySurface             3       1       2       .       .       .       .       .       .       1       .       .       .     202
closureTraversal          4       7       5       6       1      11       1       5       4      20       .       .       .    1680
mxcpp::Bsdf API           2       3       3       9       .      10       2       .       .       3       4       4       .     627
```

**Strictly lower-triangular: zero entries on or above the diagonal, so no
cycles.** Nothing depends on `closureTraversal`; nothing depends on the public
API; no kind depends on a kind listed below it.

Two things had to be fixed to get there, and an earlier draft of this plan had
both wrong:

- **`_BsdlLayerRoughnessFromAlpha` `:466` moves from energy compensation to
  `microfacet`.** It reads `_kMinMicrofacetAlpha` and was the only back-edge
  from energy into microfacet. It is a roughness helper, not a compensation
  term.
- **`energyCompensation` and `thinFilm` sit *below* `microfacet`,
  `reflectionOnlyInterfaces`, and `dielectric`, not above them.**
  `_EvalMicrofacetReflectionIsotropic`/`Anisotropic` call
  `_TurquinMicrofacetMsScale` (`:1890`, `:1927`), and both reflection-only
  interfaces call `_ApplyThinFilm`. The module boundaries were right; the
  ordering was not, which also made the commit sequence backwards.

No module needs splitting to break a cycle. The order above is the only
constraint.

## Target layout

`bsdf.h` and `bsdf.cpp` keep their paths and contain **only the
`mxcpp::Bsdf` namespace API** — the 4 configuration functions, the 18
`Eval*`/`Sample*`/`Pdf*` entry points, `PruneCausticClassLobes`,
`SampleSubsurfaceEntry`, and the namespace-scoped closure types. `bsdf.h` is
included by `rendererImpl.h`, `adobeOpenPbr.h`, and both MaterialXCpp tests;
those includes do not change.

```
materials/
    bsdf.h                          unchanged
    bsdf.cpp                     627 lines   the mxcpp::Bsdf namespace API, nothing else
    bsdf/
        mathPrimitives.h          67   10   scalar and vector primitives
        shadingFrame.h           149   11   tangent frame, normal resolution, hemisphere sampling
        fresnel.h                121    8   catalogue of Fresnel formulas and cos-theta conventions
        energyCompensation.{h,cpp} 614  12   directional-albedo tables, multiple-scattering compensation
        thinFilm.{h,cpp}         283    5   polarized Fresnel, Airy summation, spectral sensitivity
        microfacet.{h,cpp}       434   24   GGX distributions, VNDF sampling, roughness<->alpha
        sheen.h                   34    3   Charlie distribution, Ashikhmin visibility
        diffuse.{h,cpp}           94    5   Oren-Nayar, EON/FON, Burley, translucent
        reflectionOnlyInterfaces.{h,cpp}
                                  78    4   conductor and generalized-Schlick interfaces with thin film
        dielectric.{h,cpp}       684   23   IOR resolution, interface coefficients, coupled rough transport, delta lobes, TIR
        legacySurface.{h,cpp}    202    4   UsdPreviewSurface summary path
        closureTraversal.{h,cpp}1680    4   Eval/Pdf/Sample/Throughput/ApproxWeight over the closure tree, caustic pruning
        ggxEnergyLut.h            85         table  (from bsdf.cpp:38-120)
        dielectricReflFrontLut.h 615         table  (renamed)
        dielectricBothLut.h     2085         table  (renamed)
        dielectricTransmissionLut.h 2088     table  (renamed)
```

Second column is lines, third is the module's incoming interface — how many
distinct symbols every other module needs from it, combined.

The four LUT headers move into `bsdf/` and drop their `bsdf` prefix, which is
stutter once they are inside a `bsdf/` directory. After this plan they are
included by `bsdf/energyCompensation.cpp` **and by
`tests/testMaterialXCppBsdf.cpp`**, which cross-checks two of them against
BSDL's own tables (see Sequencing, commit 1).

Include paths from inside `bsdf/` gain one level: `"../spectral.h"` becomes
`"../../spectral.h"`, `"../nodes/helpers/mathHelpers.h"` becomes
`"../../nodes/helpers/mathHelpers.h"`.

### Which files are header-only

Everything at the bottom of the DAG is currently inlined into the traversal
functions, and traversal runs per path vertex. Moving those bodies into `.cpp`
files would trade real performance for layout. So:

- **header-only, all `inline`**: `mathPrimitives.h`, `shadingFrame.h`,
  `fresnel.h`, `sheen.h`. Small bodies, hot, no state. `sheen` is header-only
  because its three functions total 34 lines, not because it is unimportant.
- **`.h`/`.cpp` pair with the hot small entry points `inline` in the header**:
  `microfacet`, `dielectric`, `diffuse`, `reflectionOnlyInterfaces`.
  Distributions, visibility terms, and predicates stay inline; the 30-100 line
  eval/sample bodies go in the `.cpp` and are measured.
- **`.h`/`.cpp` pair, everything out of line**: `energyCompensation` (holds
  4,873 lines of table that must not enter another TU), `thinFilm` (cold —
  only reached when thin-film weight is non-zero), `legacySurface` (cold —
  only reached when a material has no closure tree), `closureTraversal`.

`perf stat` decides any case that is not obvious. See Validation.

## What goes in each file

- **`mathPrimitives.h`** — `_kEpsilon` `:29`, `_ClampFinite01` `:1179`,
  `_LerpVec` `:491`, `_SafeVec` `:1010`, `_Luminance` `:1004`,
  `_MaxVec` `:1341`, `_SqrtVec` `:1314`, `_CosVec` `:1323`, `_ExpVec` `:1329`,
  `_SquareVec` `:1335`.
- **`shadingFrame.h`** — `_Frame` `:761`, `_MirrorAcrossSurface` `:497`,
  `_FaceForwardNormal` `:1818`, `_NormalizeOrFallback` `:1824`,
  `_IsSameSide` `:1849`, `_ResolveReflectionNormal` `:1835`,
  `_AbsCosTheta` `:866`, `_Tan2Theta` `:872`, `_SampleCosineHemisphere` `:803`,
  `_CosineHemispherePdf` `:814`, `_ScaleDiscreteSpecularSample` `:2423`,
  `_SampleDeltaReflection` `:2434`.
- **`fresnel.h`** — `_SchlickFresnel` `:516`, `_SchlickFresnelScalar` `:525`,
  `_MaterialXDielectricFresnel` `:536`, `_GeneralizedSchlickFresnel` `:557`,
  `_ConductorF0` `:585`, `_ReflectionFresnelCosTheta` `:1606`,
  `_TransmissionFresnelCosTheta` `:1616`, `_TransmissionScale` `:1636`.

  This file is the *catalogue* of Fresnel formulas and the cos-theta
  conventions that go with them. Membership test, in both directions:

  > A Fresnel form belongs here if and only if it is a bare function of floats
  > and vectors. Anything that reads a `Bsdf::XxxData` closure struct belongs
  > to that closure's kind, however Fresnel-shaped it looks.

  That keeps this file a leaf: it depends only on `mathPrimitives` and
  `shadingFrame`, so every other module can include it freely. It is why
  `_GeneralizedSchlickFresnel` and `_ConductorF0` live here rather than in
  `reflectionOnlyInterfaces` — filing them by kind would make
  `energyCompensation` and `closureTraversal` reach into a kind module for a
  Fresnel form.

  Three entries have a single consumer today (`_MaterialXDielectricFresnel`
  only from `energyCompensation`, the two cos-theta helpers only from
  `closureTraversal`). Keep them — a catalogue's value is being the one place
  to look, and these are 10-21 line pure functions.

  `_ThinWalledWindowReflectance` `:503` does *not* belong here despite sitting
  next to them today: it is a thin-walled policy adjustment, not a Fresnel
  form, and its one caller is at `:1710`. It goes to `dielectric`.
- **`energyCompensation`** — `:135-408` table lookups, `:1028-1287`
  compensation, `_kGgxEnergy*`/`_kBsdlDielectricIor*` constants, and the four
  LUT headers. `_kGgxMissingEnergy`/`_kGgxEnergyCosTheta` (`:43-120`) move to
  `bsdf/ggxEnergyLut.h` so all four tables live the same way.
- **`thinFilm`** — `:1288-1605` and `_kThinFilmAiryIterations` `:37`, less the
  five generic vector helpers that go to `mathPrimitives.h`.
- **`microfacet`** — `:409-490` alpha/roughness helpers **including
  `_BsdlLayerRoughnessFromAlpha`**, `:599-640` isotropic GGX, `:820-1003`
  VNDF and anisotropic GGX, `:1855-1996` microfacet reflection
  eval/pdf/sample, `:2796-2809` perceptual-roughness helpers,
  `_kMinMicrofacetAlpha`/`_kEffectivelySmoothMicrofacetAlpha`/`_kSmoothRoughnessThreshold`.
- **`sheen.h`** — `_Charlie_D` `:652`, `_Ashikhmin_V` `:661`,
  `_ApproxSheenDirAlbedo` `:742`.
- **`diffuse`** — `_OrenNayarFactor` `:667`, `_kFonConstantA/B` `:677`,
  `_FonDirectionalAlbedoApprox` `:681`, `_EvalEonDiffuse` `:696`,
  `_BurleyFactor` `:731`, `_PdfTranslucent` `:2336`, `_EvalTranslucent` `:2345`.
- **`reflectionOnlyInterfaces`** — `_ConductorReflectionFresnel` `:1770`,
  `_SampleDeltaConductorReflection` `:2562`,
  `_GeneralizedSchlickReflectionFresnel` `:1792`,
  `_SampleDeltaGeneralizedSchlickReflection` `:2577`.

  Conductor and generalized Schlick are the two reflection-only interface
  kinds, and each has the identical shape: a closure-aware Fresnel that builds
  `_ThinFilmParams` and calls `_ApplyThinFilm`, plus a delta reflection
  sample. Their bare base formulas are in `fresnel.h`. "Reflection-only" is
  already a concept here — `_IsReflectionOnlyNode`, owned by
  `07-plan-closure-classification.md`.

  **Do not put these in `fresnel.h`.** Both depend on `thinFilm`
  (`_GeneralizedSchlickReflectionFresnel` even names `_ThinFilmModel::Schlick`
  at `:1803`), and `fresnel.h` is included by `microfacet`, `dielectric`,
  `energyCompensation`, `legacySurface`, `closureTraversal`, and the public
  API. Merging would make `thinFilm` — 283 cold lines, reached only when film
  weight is non-zero — a near-universal dependency and push `fresnel.h` off
  the bottom of the DAG.
- **`dielectric`** — `_ResolveDielectricIor` `:416`/`:430`,
  `_ThinWalledWindowReflectance` `:503`, `:1644-1769` interface
  reflectance/coefficients/selection, `:1997-2335` coupled rough dielectric
  including the straight-shadow interface search, `:2355-2383` TIR,
  `_SampleDeltaTransmission` `:2385`, `:2454-2561` delta dielectric lobes,
  `_kTransmissionExactFresnelMaxAlpha`/`_kTransmissionFresnelBlendMaxAlpha`.
- **`legacySurface`** — `:2592-2769`, `_ComputeLegacyF0` `:641`,
  `_ClearLegacyBsdfSummary` `:2810`.
- **`closureTraversal`** — `:2770-2795` forward declarations,
  `_CausticClassPruner` `:2823`, `_EvalLayerBaseThroughput` `:2984`,
  `_EvalNode` `:2994`, `_EvalThroughput` `:3336`, `_ApproxWeight` `:3501`,
  `_PdfNode` `:3636`, `_FinalizeSubtreeSample` `:3862`, `_SampleNode` `:3888`.

### Why `closureTraversal` stays one file

Its six functions are 589, 342, 226, 165, 161, and 135 lines. Each is one
visible dispatch over the closure variant, which Goal 2 says to leave linear,
and they are mutually recursive — the forward-declaration block at
`bsdf.cpp:2770-2795` exists precisely because `_SampleNode` reaches
`_PdfNode` and `_EvalNode`. Splitting eval from sample from pdf would put a
recursion cycle across three translation units for no gain.

At 1,680 lines it becomes the largest file in the tree. That is the honest
outcome: it is one subject, and this plan makes that legible rather than
pretending it is smaller.

## Naming and namespace

`14-plan-renderer-impl-header.md` establishes the rule: header-declared and
cross-TU gets a namespace; TU-contained gets `static _Foo` at that namespace's
scope — here `mxcpp`, not `ty`. **No anonymous namespaces**, in headers or
sources. `MaterialXCpp/` is exempt from `ty`, not from this;
`nodes/helpers/proceduralHelpers.h` is the one existing header violation and
should be fixed here or raised against `19-plan-ty-namespace.md`, which owns the
rule. `static` cannot apply to a type, so any TU-contained `struct` here keeps
external linkage and needs a name unique across the plugin — 19 carries the
uniqueness check.

**Internal module symbols go into a nested namespace, not directly into
`mxcpp`.** Blanket promotion into `mxcpp` collides with symbols that already
exist there:

| Would-be name | Existing definition |
| --- | --- |
| `mxcpp::Clamp01` | `nodes/helpers/mathHelpers.h:48` (and `:55` template overload) |
| `mxcpp::SaturateVec` | `surfaceShaderUtils.h:17` |

The home is **`mxcpp::Bsdf::detail`**. `mxcpp::Bsdf` already exists and is
already the BSDF namespace, so nesting inside it scopes these correctly
without inventing a sibling; the `detail` level is what keeps
`Bsdf::ApplyThinFilm` from reading as module interface next to
`Bsdf::EvalSurface`. That is a readability boundary within the plugin, not a
build one — after `01-plan-build-surface.md` nothing here is installed either
way. `mxcpp::Bsdf` is the interface the rest of the renderer calls, and
`detail` is what tells a reader which of the two a symbol belongs to.

So `_ApplyThinFilm` becomes `mxcpp::Bsdf::detail::ApplyThinFilm`,
`_GGX_D_Anisotropic` becomes `mxcpp::Bsdf::detail::GgxDAnisotropic`,
`_kMinMicrofacetAlpha` becomes `mxcpp::Bsdf::detail::kMinMicrofacetAlpha`.
Within `bsdf/` the files open `namespace Bsdf { namespace detail {`, so
call sites stay unqualified. Public types the modules consume
(`Bsdf::DielectricData`, `Bsdf::ClosureTree`, `Bsdf::BsdfSample`,
`Bsdf::NodeId`) stay in `Bsdf` and are reachable unqualified from inside
`detail`. Anything internal to one `.cpp` keeps its `_Foo` form and becomes
`static` at `mxcpp::Bsdf::detail` scope. After this plan the `_` prefix means
exactly one thing in these files: not used outside this file.

### Delete the duplicates rather than moving them

Two of the collisions above are not naming problems, they are pre-existing
duplicated code. `bsdf.cpp` already includes `nodes/helpers/mathHelpers.h`
(`:16`), so those helpers are in scope today. Commit 2 deletes both
duplicates:

| Local | Replaced by | Bodies |
| --- | --- | --- |
| `_Clamp01` `:123` | `mxcpp::Clamp01(float)`, `mathHelpers.h:48` | `std::clamp(x,0,1)` vs `ClampValue(v,0,1)` → `std::clamp` — identical |
| `_SaturateVec` `:1022` | `mxcpp::Clamp01(const T&)`, `mathHelpers.h:55` | componentwise `std::clamp(v[i],0,1)` both sides — identical |

**`_SaturateVec` is replaced by the `Clamp01` vector overload, not by
`mxcpp::SaturateVec`.** The two are equivalent, but `SaturateVec` lives in
`surfaceShaderUtils.h` — 448 lines, itself pulling in `paramMap.h` and
`surfaceClosure.h` — which `bsdf.cpp` does not include today. Acquiring that
dependency in every `bsdf/` translation unit to delete a six-line helper is a
bad trade, and it is a dependency edge the matrix above does not show.
`Clamp01`'s non-arithmetic overload resolves to
`ClampValue(v, T(0.0f), T(1.0f))`, which clamps componentwise over
`T::dimensions()` — identical to `_SaturateVec` for `Vec3f` — and it is
already in scope. Neither deletion adds an include.

**One near-duplicate must NOT be collapsed.** `_LerpVec(a,b,t)` at `:491` is
`a*(1-t) + b*t`; `mxcpp::MixVec` (`surfaceShaderUtils.h:26`) and
`mxcpp::Mix<T>` (`mathHelpers.h:62`) are both `bg + (fg-bg)*mix`. Those are
algebraically equal and **numerically different** — the local form is the
precise variant, exact at `t == 1`. Substituting it would change output bits
under cover of a layout commit. `_LerpVec` moves to
`mathPrimitives.h` unchanged. If the three are ever unified, that is a
deliberate behavior-affecting change with its own image diff, not part of this
plan.

Commit 2 must run the same check over the rest of `mathPrimitives.h` and
`shadingFrame.h` against `mathHelpers.h`, `colorHelpers.h`, and
`surfaceShaderUtils.h` before promoting anything. Record the result.

`05-plan-naming-core.md` owns the *quantity* names inside these functions
(`Ng`/`wi`/`wo`/`eta`). This plan does not rename parameters or locals; the
two passes touch disjoint identifiers.

### Why `mathPrimitives.h`, and not `math.h` or `mathHelpers.h`

- **`math.h`** would shadow the C standard header for any quoted include, and
  would break outright if `materials/bsdf/` were ever added to a `-I` path.
- **`mathHelpers.h`** would collide with `nodes/helpers/mathHelpers.h`, which
  `bsdf.cpp` includes today — two headers with the same basename live in one
  translation unit.

`mathPrimitives.h` collides with neither, and the `math` qualifier stops
"primitives" reading as geometric primitives, which is what the word means
everywhere else in this renderer.

(Header install layout is not a consideration here: every new header lands in
`PRIVATE_HEADERS` and none of them is installed.)

## Declaration contracts

Every symbol that becomes a header declaration gets a Goal 4 contract, not
just a one-line file responsibility. At minimum, each declaration states:

- **Input invariants** — which direction vectors must be unit length, which
  must lie in the upper hemisphere of the supplied normal, valid ranges for
  roughness (`[0,1]` perceptual) and alpha (`[kMinMicrofacetAlpha, 1]`),
  whether an IOR is a ratio or an absolute index, and which side of the
  surface `wo` is assumed to be on.
- **Output guarantees** — non-negativity, whether a returned PDF is solid
  angle or area measure, and whether a returned reflectance is already
  weighted.
- **Failure results** — what an invalid or degenerate input returns. The
  sampling functions return `Bsdf::BsdfSample`; state explicitly what marks a
  sample invalid and whether callers must check before dividing by the PDF.

`21-plan-api-contracts.md` owns systematic contracts elsewhere in the tree;
this plan writes its own, because the declarations do not exist before it.

## Sequencing

Bottom-up in DAG order, so every commit builds against already-moved
dependencies and never needs a temporary forward declaration.

1. Create `bsdf/`; move the three LUT headers under their unprefixed names and
   `_kGgxMissingEnergy`/`_kGgxEnergyCosTheta` into `bsdf/ggxEnergyLut.h`.
   **Update `tests/testMaterialXCppBsdf.cpp:8-9`**, which includes
   `bsdfDielectricReflFrontLut.h` and `bsdfDielectricTransmissionLut.h`
   directly and cross-checks them against BSDL's tables — that test is a
   legitimate second consumer of the LUT headers and stays one. Pure data
   motion: this diff must be **exactly zero**, and any difference is a
   transcription error.
2. `mathPrimitives.h`, `shadingFrame.h`, `fresnel.h`. Run the duplicate audit
   first; delete `_Clamp01` and `_SaturateVec`; leave `_LerpVec` alone.
   Header-only, so no inlining is lost — but the two deletions substitute
   `ClampValue`-based bodies for `std::clamp`-based ones, so verify the diff
   is zero rather than assuming it.
3. `energyCompensation`.
4. `thinFilm`.
5. `microfacet`.
6. `sheen`.
7. `diffuse`.
8. `reflectionOnlyInterfaces`.
9. `dielectric`.
10. `legacySurface`.
11. `closureTraversal` — after which `bsdf.cpp` contains only the
    `mxcpp::Bsdf` namespace API.

Eleven commits. **One module per commit, with no exceptions** — an earlier
draft bundled `microfacet`, `sheen`, and `diffuse`, which would have put the
highest-risk module in the sequence (24-symbol interface, hottest path) in a
commit where a performance or image failure could not be attributed. Commits 6
and 7 are small enough to feel like overhead; take the overhead. Do not cut a
release mid-sequence.

## Dependencies

- `14-plan-renderer-impl-header.md` — establishes the header-vs-TU membership
  convention. Only the convention is inherited; the namespace here is
  `mxcpp::Bsdf`, not `ty`.
- `04-plan-materialx-error-handling.md` and
  `15-plan-primvar-binding-cache.md` both edit
  `renderer/materials/MaterialXCpp/`. Neither touches `bsdf.cpp`, but rebase
  after them to keep the MaterialXCpp serialization in `doc/README.md` intact.
- `07-plan-closure-classification.md` edits `_IsReflectionOnlyNode`, which
  lives in `rendererImpl.h`, not here. No conflict, but keep the
  `reflectionOnlyInterfaces` naming consistent with whatever 07 lands.
- `01-plan-build-surface.md` lands first, so all `CMakeLists.txt` entries are
  already private when this plan adds its own.
- Must precede `19-plan-ty-namespace.md`, which needs the final filenames.
- `23-plan-auto-types.md` sweeps whatever the code ends up as; it runs last.

## Build changes

`renderer/materials/MaterialXCpp/materials/bsdf` is a `PRIVATE_CLASSES` entry
(`PUBLIC_CLASSES` until `01-plan-build-surface.md` lands, which it does before
this plan). Everything new goes in `PRIVATE_CLASSES` / `PRIVATE_HEADERS`
alongside it. `bsdf.h` remains the only header here any other subsystem
includes, which is a layering fact about who calls what, not an installation
one — nothing in this directory is installed.

Eight new `PRIVATE_CLASSES` pairs, under
`renderer/materials/MaterialXCpp/materials/bsdf/`: `energyCompensation`,
`thinFilm`, `microfacet`, `diffuse`, `reflectionOnlyInterfaces`,
`dielectric`, `legacySurface`, `closureTraversal`.

Eight new `PRIVATE_HEADERS`, same prefix: `mathPrimitives.h`,
`shadingFrame.h`, `fresnel.h`, `sheen.h`, `ggxEnergyLut.h`,
`dielectricReflFrontLut.h`, `dielectricBothLut.h`,
`dielectricTransmissionLut.h`.

Note there is nothing to remove: `CMakeLists.txt` currently lists no `*Lut.h`
entry at all — the three existing LUT headers are picked up implicitly as
includes of `bsdf.cpp` and were never in a header list. Commit 1 adds all four
under their new paths, which is a small improvement in its own right.

## Validation

### Correctness

- **Commits 1 and 2 must diff to exactly zero.** Commit 1 moves only constant
  data; commit 2 moves `inline` definitions into headers and deletes two
  verified-identical duplicates. Any difference is a mistake, not contraction.
- **Commits 3-11 do not claim bit-identical output.** They cross
  translation-unit boundaries and can change floating-point contraction.
  Requirement: pass `-s "{settings}.ty:randomNumberSeed = 1"` to `usdrender`,
  then
  `oiiotool --diff --fail 0.0005 --failpercent 0.05` against the pre-move
  render, with any non-zero difference explained as contraction rather than
  assumed to be.
- Material-fidelity suite per commit, not once at the end:
  `cd /home/anders/code/typhoon-tests && pixi run pytest material-fidelity
  --typhoon-provider /home/anders/code/openusd-omniverse`. Commit-specific
  cases that must be present: thin film (4), rough and anisotropic GGX
  reflection (5), sheen (6), Oren-Nayar/EON diffuse and translucency (7),
  conductors and generalized Schlick (8), coupled dielectric transmission and
  rough refraction (9), a non-metalness `UsdPreviewSurface`, the only thing
  that reaches the legacy path (10), and a layered OpenPBR material
  exercising every closure node type (11).
- `pixi run cmake --build build --target testMaterialXCpp` and
  `pixi run ctest --test-dir build -R testMaterialXCpp --output-on-failure`.
  `testMaterialXCppBsdf.cpp` needs its two LUT includes repointed in commit 1;
  its `"../materials/bsdf.h"` include does not change. If it needs any *other*
  edit, the split moved something that was not internal.

### Runtime performance

The dominant risk: **eight modules move bodies out of line** into their own
translation units, where today everything inlines into
`_SampleNode`/`_EvalNode`/`_PdfNode`. Four modules stay header-only
(`mathPrimitives`, `shadingFrame`, `fresnel`, `sheen`) and lose nothing, and
the hot entry points of the paired modules stay `inline` in their headers by
design. Measure per commit from 3 onward, so a regression is attributable.

- `perf stat -r 5` per the AGENTS.md profiling recipe, plus the
  renderer-reported samples/sec.
- Named workloads, all three every time: a **material-heavy** scene (many
  distinct closures), a **deep/layered-path** scene (high `maxBounces` with
  layered OpenPBR), and a **rough-dielectric-heavy** scene, which exercises
  the `dielectric` → `microfacet` → `energyCompensation` chain this plan
  stretches across three TUs.
- **Threshold: 1% on samples/sec.** Anything at or above that on any workload
  blocks the commit until explained. Remedies, in order of preference: move
  the offending entry point `inline` into its header; merge the module into
  the one that calls it; abandon that module's split. If several functions
  have to go back into headers to recover the regression, the boundary has
  failed its purpose and merging or reverting is the correct outcome — the
  target layout is the goal, not a constraint that overrides measurement.
- Record baselines and per-commit results in `OPTIMIZATION.md`.

### Build time

This plan argues partly from what the compiler faces, so measure it. Splitting
one TU into twelve can improve incremental builds and make clean builds worse
through repeated header parsing — both directions are plausible and neither is
assumed here.

- Clean `hdEmbree` build, before and after the full sequence.
- Rebuild after touching `closureTraversal` only.
- Rebuild after touching one leaf module (`sheen.h`) only.

Record all three in `OPTIMIZATION.md`. A clean-build regression is acceptable
if the incremental numbers justify it; it is not acceptable silently.

### Tests

No new tests. This is a move; `testMaterialXCpp` plus the fidelity suite is
the check. If review concludes a newly isolated module needs unit coverage it
never had, that is its own change, and the adversarial test-review agent runs
on it.

## Documentation

- `AGENTS.md` "Directory Map" — the `renderer/materials/MaterialXCpp/` entry
  gains the `bsdf/` map. The sentence about regenerating the committed
  transmission LUT should name `bsdf/energyCompensation` and `bsdf/*Lut.h`.
- `AGENTS.md` "Materials And Textures" — the transmissive-model policy
  paragraph and the OpenPBR/Standard-Surface lobe rules describe code that now
  lives in `bsdf/dielectric` and `bsdf/closureTraversal`; point them there.
- `ARCHITECTURE.md` — the MaterialXCpp source map gains the `bsdf/` layer
  diagram. Record the dependency order: it is the useful artifact, because a
  reader who knows the layering knows where a change can and cannot ripple.
- `README.md` — no change expected; confirm.

## Cost, honestly

This introduces a module abstraction: **twelve internal header interfaces,
eight new implementation translation units**, about **113 header-visible
declarations** across those twelve headers, and out-of-line calls where there
were inlined ones. Goal 1's "net reduction of at least twice
what the abstraction adds" is a real bar and this plan does not clear it on
line count — total lines are unchanged, and the header declarations are new
text.

**Decision:** Anders explicitly approved this plan as a scoped exception to
Goal 1's line-reduction gate. The exception applies to this measured source
partition only; it does not weaken the general rule or pre-approve additional
interfaces. The correctness, performance, build-time, dependency-DAG, and
documentation gates below remain mandatory.

The justification is not line count, it is these four, in order of weight:

1. **Reviewable interfaces.** The 64-symbol coupling exists today, undeclared.
   Making it twelve declared interfaces averaging nine symbols means a change
   to one kind can be reviewed against one interface.
2. **A checkable invariant.** The dependency order is strictly one-way, so a
   reviewer can verify mechanically that a change to `dielectric` cannot
   affect `fresnel` or `energyCompensation`. No such check is possible in one
   5,100-line anonymous namespace.
3. **4,873 lines of table leave the algorithmic translation unit**, and the
   test that verifies them stops being coupled to the file that uses them.
4. **Navigation.** Twelve named files against one file where subject
   boundaries are invisible.

The cost is real and stated: a reader tracing one rough-dielectric sample now
crosses six files — `bsdf.cpp` → `closureTraversal` → `dielectric` →
`microfacet` → `fresnel` → `energyCompensation`. Those six layers exist today
too, unlabelled; the difference is that finding them currently means scrolling.
Whether that trade is worth it is the decision this plan asks for, and it
should be made on the four points above rather than on an exemption.

## Risks and decisions

- **`dielectric` at 684 lines is the least clean module.** 23 symbols out, 25
  in — the only module that both exports and consumes broadly. It is genuinely
  three things — interface Fresnel coefficients, coupled rough transport, and
  delta lobes — held together by shared IOR resolution. Leave it whole; if it
  needs splitting later the seam is at `:1997`, and this plan's measurement
  makes that a well-posed follow-up rather than a guess.
- **`sheen.h` at 34 lines is the smallest module.** It stays separate because
  it is an independent lobe with its own literature, and header-only because
  three inline functions do not warrant a translation unit. It is the one
  module whose file could be folded into `microfacet` with no DAG consequence
  (Charlie and Ashikhmin are a distribution and a visibility term, structurally
  identical to `GGX_D` and `GGX_V`) if review prefers eleven modules.
- `_CollectCoupledTransmissionInterfaceForStraightShadow` `:2022` walks the
  closure tree and is the one function in `dielectric` that touches
  `Bsdf::ClosureTree`. It stays there because it answers a dielectric
  question; moving it to `closureTraversal` would create the only downward
  edge from traversal into a kind.

## Completion criteria

- The traversal-over-undifferentiated-lobes split is recorded as measured and
  rejected, with the 64-symbol number, so it is not re-proposed.
- `bsdf.cpp` is ~627 lines and contains only the `mxcpp::Bsdf` namespace API.
- `bsdf/` contains exactly the files in "Target layout", each with its
  one-sentence responsibility at the top of its header.
- **The dependency matrix is still strictly lower-triangular after the move**,
  in the order given. Any entry on or above the diagonal is a design error;
  check this explicitly in review of commit 11.
- Every header declaration carries the input invariants, output guarantees,
  and failure results described under "Declaration contracts". A declaration
  with only a one-line summary does not satisfy this.
- No symbol is promoted to plain `mxcpp::` or to bare `mxcpp::Bsdf::`;
  internal module symbols live in `mxcpp::Bsdf::detail`, and every remaining
  `_Foo` is TU-local. `_Clamp01` and `_SaturateVec` are deleted in favour of
  `mxcpp::Clamp01`'s scalar and vector overloads, adding no include;
  `_LerpVec` is moved unchanged.
- Every new module lands in `PRIVATE_CLASSES` / `PRIVATE_HEADERS` at every
  point in the sequence.
- `tests/testMaterialXCppBsdf.cpp` includes the LUT headers at their new
  paths and still passes.
- Commits 1-2 diff to zero; commits 3-11 within tolerance with any difference
  explained; samples/sec within 1% on all three named workloads; clean and
  incremental build times recorded.
- `CMakeLists.txt`, `AGENTS.md`, and `ARCHITECTURE.md` match the final layout.

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
