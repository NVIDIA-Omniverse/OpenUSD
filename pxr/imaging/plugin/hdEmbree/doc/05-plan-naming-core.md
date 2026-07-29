# Plan: Unify variable naming for common quantities

Status: readability cleanup, derived from a full-codebase review against the
five project goals in `pxr/imaging/plugin/hdEmbree/AGENTS.md`. Targets
**Goal 3** (variable naming must stay consistent for common quantities) and
**Goal 5** (types and quantities must be named explicitly). It introduces a
compositional convention rather than preserving traditional single-letter
rendering notation.

Every change here is rename-only and must not change rendered output. This is
the lowest-risk, highest-signal readability work in the review. Part 1 (normal
naming) is the largest piece and also finishes aligning the code with the
normal-handling contract already implemented in the integrator (an immutable
outward geometric normal for topology and one exitant-facing shading frame for
the material); the names remain split across call sites.

---

# Part 1: Surface-normal naming in the integrator

## The problem

The same physical quantity is spelled many different ways across the integrator
core, so a reader cannot tell whether two names refer to the same vector or two
different ones.

### The geometric normal (world-space, outward, Embree `Ng`)

Geometric normals use inconsistent base names and qualifier order:

| Name | Actual meaning | Location |
| --- | --- | --- |
| `Ng` | Outward world-space geometric normal. | `renderer/integrator/pathIntegrator.cpp:224` |
| `incidentNg` | Exitant-facing geometric normal (toward the current `wo`); should be `normalGeomWldOut`. | `renderer/integrator/pathIntegrator.cpp:225` |
| `faceNormal` | Exitant-facing geometric normal (toward the current `wo`); should be `normalGeomWldOut`. | `renderer/integrator/pathIntegrator.cpp:459`, `renderer/renderer.h:775`, `renderer/integrator/sss.cpp:889` |
| `hitNg` | Geometric normal at a shadow blocker. The role qualifier is useful, but should follow the schema: `normalGeomBlockerWldExt`. | `renderer/integrator/visibility.cpp:119` |
| `hitNormal` / `objectHitNormal` | World/object-space geometric normals at an SSS boundary. | `renderer/integrator/sss.cpp:80-82` |
| `exitGeomNormal` | Outward geometric normal at an SSS exit. The exit qualifier is meaningful. | `renderer/integrator/sss.h:46` |
| `hitNormal` | Blocker geometric normal passed to straight-shadow Fresnel. | `renderer/rendererImpl.h:457-469` |

`entryGeomNormal` / `entryNormal` are not part of this family despite their
names: they currently hold an exitant-facing material-resolved normal used as
the SSS guide axis.

### Surface and material-resolved shading normals

One function obscures two distinct values with aliases:

- `renderer/integrator/pathIntegrator.cpp:227` initializes `normal` from the
  exitant-facing **surface** (pre-material) shading normal.
- `renderer/integrator/pathIntegrator.cpp:228` snapshots that surface-normal
  value as `differentialNormal`; the snapshot is required because `normal` is reassigned
  to the material-resolved value at line 329 and the surface-normal value is
  needed again at line 719.
- `renderer/integrator/pathIntegrator.cpp:409` aliases the now-resolved value as
  `bsdfNormal`, while the same value remains `normal` at other call sites.
- `_TryEvalSurfaceClosureAtHit` correctly has both the current
  `baseNormalIncident` surface-normal value and a resolved value, but calls
  the latter `shadingNormal` while the path integrator calls it `normal`.

### Concrete traps this naming hides

1. `renderer/integrator/sss.cpp:907` assigns the **shading** normal
   (`input.normal`) into `walkInput.entryGeomNormal`, a field documented as
   *outward geometric* in `renderer/integrator/sss.h:29`. The name says
   geometric; the value is a shading normal.
2. `renderer/integrator/visibility.cpp:24-30`: `_Visibility`'s second parameter
   is named `normal`. Surface callers pass `Ng`, but medium callers pass the
   light direction because there is no surface. Its real role is selecting the
   ray-origin offset, not representing one normal quantity.
3. `renderer/integrator/lighting.cpp:409`: `_ComputeMediumDirectLighting` calls
   `_Visibility(position, ls.wI, ls.wI, ...)`, passing a **light direction** as
   the `normal` argument, because an in-medium scatter point has no surface
   normal. Nothing at the call site explains this.

## Target convention

### Naming schema

Build names from a full quantity root followed by semantic suffixes in this
fixed order:

`<quantity><kind><transport/event role><coordinate space><orientation><representation or measure><reciprocal>`

Rules:

1. **Use full quantity roots, never single-letter variables.** Use `normal`
   instead of `N`/`n`, `omega` instead of `w`, `radiance` instead of `L`,
   `position` instead of `p`, `distance` instead of `t`, `anisotropy` instead
   of `g`, and `index` or a role-specific index instead of `i`/`j`/`k`.
   Exceptions include `u1`/`u2` for uniform random samples, `u`/`v` for direct
   texture or surface-parametric coordinates, `x`/`y` for pixel column/row
   indices, and `c` for a pixel or RGB channel index when clear from context.
   Derivatives are the other explicit exception: name them with the
   `d<Quantity>d<Variable>` convention - `dPdu`/`dPdv` for surface
   parameterization derivatives and `dPdx`/`dPdy` for screen-space (ray
   differential) derivatives - because that notation is clearer than any
   spelled-out or offset-style alternative.
2. **Add semantic suffixes in one order.** Kind identifies the physical
   variant; transport/event role identifies relationships; space follows role;
   orientation follows space; representation or measure follows orientation;
   and terminal `Inverse` always comes last. Skip inapplicable categories, but
   never reorder them.
3. **Use only the fixed suffix vocabulary.** To keep names readable, use:
   `Geom` (geometric), `Srf` (pre-material surface), `Shd` (material-resolved
   shading), `Wld` (world space), `Obj` (object space), `Ext` (authored
   exterior), and `Out` / `In` for facing `omegaOut` / `omegaIn`. Other suffixes
   remain unabbreviated unless this table explicitly defines an abbreviation.
   `pos` and `dir` are the fixed quantity roots for position and direction.
   Use `bary` as the prefix for barycentric-coordinate components, such as
   `baryU`, `baryV`, and `baryW`.
   Do not invent alternative spellings or shorten other quantity roots.
4. **The quantity is always the root.** Use `posHitWld`, not
   `hitPositionWorld`; `dirRayWld`, not `rayDirectionWorld`; and
   `normalGeomBlockerWldExt`, not `blockerNg`; and
   `pdfAreaInverse`, not `pdfInverseArea`. Prefixes such as
   `out...` use lowercase `out` to mark output pointer or reference parameters;
   uppercase `Out` within the quantity retains its transport meaning, as in
   `outNormalShdWldOut`.
5. **Add coordinate space after kind and role.** Use `Obj`, `Wld`, `Tangent`,
   `Texture`, or a more specific named local frame. Spatial values in shared
   state and function interfaces must never rely on an implicit space.
   Transform names encode both spaces as `<from>To<to>` using the same fixed
   tokens, for example `objToWld`.
6. **Use `In` and `Out` for directional transport sides.** `omegaIn` is the incident
   direction toward the next vertex/light; `omegaOut` is the exitant direction
   toward the previous vertex/camera. Do not use `In`/`Out` to mean object
   exterior or an outward normal. IOR names are the explicit optics exception:
   `iorIn` is the incident medium before a crossing and `iorOut` is the
   transmitted medium after it, independent of `omegaIn` and `omegaOut`.
7. **Describe normal orientation explicitly.** Use `Ext` for the authored
   exterior orientation and `Out` / `In` only when a normal is faced toward
   `omegaOut` / `omegaIn`. Thus `normalGeomWldExt` and `normalGeomWldOut`
   distinguish authored orientation from exitant-facing orientation.
8. **Use `eta` only for the ratio `iorIn / iorOut`.** Actual medium indices are
   always `iorIn` and `iorOut`. Document any external API whose eta convention
   differs at the conversion boundary.
9. **Retain standard multi-letter acronyms only when they remain clear.** `pdf`,
   `bsdf`, `rgb`, and `ior` are acceptable roots. Do not encode measure or
   space as a single letter: use `pdfSolidAngle`, not `pdfW`, and `pdfArea`, not
   `pdfA`. Reciprocal values append `Inverse`, for example
   `pdfSolidAngleInverse`.
10. **External, generated, and authored schema names are boundaries.** Do not
   rename fields such as Embree `RTCHit::Ng` or MaterialX port tokens. Copy them
   immediately into a compliant first-party local name. Vendored BSDL code is
   out of scope; first-party renderer, delegate, and MaterialXCpp implementation
   identifiers are in scope unless constrained by an external schema/API.

A name may omit a suffix only when that semantic dimension genuinely does not
apply. Do not omit a suffix merely because the current function happens to use
one space or one normal kind. This makes names searchable and prevents a later
second quantity from silently changing the first one's meaning.

### Common-quantity naming table

This is the project-wide naming contract. Types are renderer-native types;
first-party MaterialXCpp code uses the corresponding `mxcpp` type without
changing the semantic name.

| Name | Type | Meaning and invariants |
| --- | --- | --- |
| `posWld` | `GfVec3f` | World-space point. Add event/usage suffixes before space, such as `posHitWld`, `posEntryWld`, or `posRayOrgWld`, when multiple positions coexist. |
| `posObj` | `GfVec3f` | Object-space point belonging to the current prototype. |
| `normalGeomWldExt` | `GfVec3f` | Normalized geometric normal transformed from Embree `RTCHit::Ng`, corrected for authored orientation, and pointing toward the authored exterior. Immutable topology and boundary authority. |
| `normalGeomWldOut` | `GfVec3f` | Geometric exterior normal faced toward `omegaOutWld`. Never material-resolved. |
| `normalGeomObjExt` | `GfVec3f` | Object-space counterpart used only where object and world geometric normals coexist. |
| `normalSrfWldExt` | `GfVec3f` | Normalized smooth/displaced shading normal, view independent, aligned with `normalGeomWldExt`, and containing no material normal-map result. |
| `normalSrfWldOut` | `GfVec3f` | Surface normal faced toward `omegaOutWld`; material-normal fallback and differential source. |
| `normalShdWldOut` | `GfVec3f` | Material-resolved shading normal faced toward `omegaOutWld`; falls back to `normalSrfWldOut` and never owns topology or medium transitions. |
| `tangentWld`, `bitangentWld` | `GfVec3f` | World-space material frame paired with `normalShdWldOut`; use `Obj` or `Tangent` suffixes for other spaces. |
| `omegaInWld` | `GfVec3f` | Normalized incident direction from the interaction toward the sampled next vertex or light. Replaces `wi`, `wI`, and ambiguous `direction` when this meaning applies. |
| `omegaOutWld` | `GfVec3f` | Normalized exitant direction from the interaction toward the previous path vertex or camera. Replaces `wo` and `wO`. |
| `posRayOrgWld`, `dirRayWld` | `GfVec3f` | Origin and forward travel direction of a generic ray segment. Use `dirShadowWld`, `dirEntryWld`, etc. when it is not a local scattering omega. |
| `diffRay` | `RayDifferential` | Screen-space differential rays associated with `posRayOrgWld` and `dirRayWld`. |
| `iorIn`, `iorOut` | `float` | Absolute IORs in the optics convention: incident medium before a crossing and transmitted medium after it. They do not name the `omegaIn`/`omegaOut` sides. `1.0f` represents vacuum/air and glass is commonly about `1.5f`. |
| `eta` | `float` | Explicit ratio `iorIn / iorOut`. `1.0f` means no IOR change; special sentinel behavior such as zero must be documented by the owning API. |
| `radianceIn` | `GfVec3f` | Incident RGB radiance carried from a sampled/evaluated light. Replaces `Li`. Use `radianceInSpectral` for a hero-wavelength scalar. |
| `radianceEmitted` | `GfVec3f` | RGB radiance emitted by a light or surface. Replaces `Le`. |
| `radianceAccumulated` | `GfVec3f` | RGB radiance accumulated for the current camera path/sample. Qualify direct, indirect, or spectral variants when they coexist. |
| `throughputRgb` | `GfVec3f` | RGB path-throughput multiplier accumulated from the camera to the current segment. |
| `throughputSpectral` | `float` | Hero-wavelength counterpart to `throughputRgb`. Use `throughputWeight` only for an unapplied local returned multiplier. |
| `bsdfValue` | `GfVec3f` or `mxcpp::Vec3f` | Evaluated or sampled BSDF value. Replaces bare `f`; qualify spectral/scalar forms when required. |
| `pdf` | `float` | Non-negative probability density whose measure is explicit in its suffix or declaration contract. |
| `pdfSolidAngle`, `pdfSolidAngleInverse` | `float` | Density and reciprocal density with respect to solid angle. Replace `pdfW` / `invPdfW`. |
| `pdfArea`, `pdfAreaInverse` | `float` | Density and reciprocal density with respect to surface area. Replace `pdfA` / `invPdfA`. |
| `distanceWld` | `float` | World-space distance. Add roles such as `distanceRemainingWld`, `distanceScatterWld`, or `distanceOppositeWld`. Replace `dist` and ray-parameter `t` where they represent distance. |
| `u1`, `u2` | `float` | Independent uniform random samples in `[0, 1)`. Bare `u` / `v` are also allowed when they directly denote texture or surface-parametric coordinates. |
| `absorption` | `GfVec3f` | Per-channel absorption coefficient in inverse world units. Replaces `sigmaA`. |
| `scattering` | `GfVec3f` | Per-channel scattering coefficient in inverse world units. Replaces `sigmaS`. |
| `extinction` | `GfVec3f` | Per-channel extinction coefficient: `absorption + scattering`. Replaces `sigmaT`; do not call this transmission. |
| `absorptionIndex` | `GfVec3f` | Dimensionless imaginary part of a conductor's complex IOR. Replaces the optics symbol `kappa`; it is not a medium absorption or extinction coefficient. |
| `anisotropy` | `float` | Henyey-Greenstein anisotropy, clamped to the owning model's documented range. Replaces bare `g`. |
| `albedo` | `GfVec3f` | Unitless per-channel scattering/reflectance ratio, normally in `[0, 1]`; qualify surface or volume variants when both coexist. |
| `cosTheta` | `float` | Cosine of the relevant angle. Add a role suffix such as `cosThetaIn`, `cosThetaOut`, or `cosThetaLight` when multiple angles coexist. Keep bare `cosTheta` only when the role is unambiguous. |
| `index` | `int` or unsigned index type | Generic collection index. Prefer `indexBounce`, `indexChannel`, `indexLight`, etc. when the index has that semantic role; `i`, `j`, and `k` are allowed for purely positional loop indices, and `idx` for an obvious local index where a longer name adds no useful information. |

The table deliberately uses longer names where they preserve physical role,
space, orientation, or measure. Apply the whole schema rather than performing
blind textual substitutions.

## Suggested changes

1. **Separate the three normal stages explicitly in `pathIntegrator.cpp`.**
   - `Ng` → `normalGeomWldExt`.
   - `incidentNg` → `normalGeomWldOut`.
   - `differentialNormal` → `normalSrfWldOut`; preserve this value
     because material resolution later changes the shading normal.
   - mutable `normal` and redundant `bsdfNormal` → one
     `normalShdWldOut` value after resolution.
2. **Use the same normal names in `_SurfaceInteraction`.** Rename `Ng`,
   `baseNormalOut`, `GetIncidentGeometricNormal()`, and
   `GetIncidentBaseNormal()` to names derived from the table. The raw Embree
   field remains `rayHit.hit.Ng`; convert it immediately to
   `normalGeomWldExt`.
3. **Preserve meaningful role and space suffixes.**
   - `_SubsurfaceInput::faceNormal` → `normalGeomWldOut`.
   - shadow blocker `hitNg` → `normalGeomBlockerWldExt`.
   - SSS `hitNormal` / `objectHitNormal` →
     `normalGeomWldExt` / `normalGeomObjExt` within
     their local scopes; use `normalGeomExitWldExt` where entry and
     exit normals coexist.
   - `_TransparentShadowTransmission::hitNormal` →
     `normalGeomWldExt`.
4. **Name the SSS guide by behavior, not topology.** Rename
   `entryGeomNormal` / walk-state `entryNormal` to
   `normalShdEntryGuideWldOut` /
   `normalShdGuideWldOut`. The current value is material-resolved
   and drives Dwivedi sampling. Replacing it with a
   geometric normal would change rendered output and requires separate design
   and reference-image validation.
5. **Name ray-offset inputs by role.** `_Visibility::normal` →
   `dirOffsetReferenceWld`; surface callers supply a geometric normal,
   while medium callers use `omegaInWld` because no surface exists.
   `_Visibility::direction` → `dirShadowWld`. Keep a WHY comment at
   the medium call.
6. **Keep API outputs distinguishable.** `_TryEvalSurfaceClosureAtHit` should
   use `normalSrfWldOut` and `normalShdWldOut`
   internally, with outputs named
   `normalShdWldOutOutput` and
   `normalGeomWldExtOutput`.

---

# Part 2: Direction, IOR, radiance, coefficient, and case conventions

## Issue 1: `eta` used for a uniform random number

`renderer/rendererImpl.h::_CosineWeightedDirection` currently uses `eta` for a
uniform sample. Rename the vector parameter `uniform_float` to
`uniformSamples` and its elements to `u1` / `u2`. Reserve `eta` for the IOR
ratio.

## Issue 2: absolute IOR and eta ratio are conflated

`pathIntegrator.cpp` currently constructs `etaIncident` and `etaTransmitted`,
then passes their ratio into a parameter also named `eta`.

**Fix:** use `iorIn` and `iorOut` for the absolute medium indices, and pass
`const float eta = iorIn / iorOut`. `_PropagateRayDifferential` should accept
`eta`, with a declaration contract stating the numerator/denominator convention
and its `1.0f` and zero behavior. Audit first-party material/BSDF code: a value
named `eta` may remain only when it is this ratio; otherwise rename it to the
appropriate `ior...` name or document an externally constrained convention.

## Issue 3: `wi`/`wo` and `wI` encode direction semantics cryptically

Rename first-party scattering directions consistently:

- `wi` / `wI` → `omegaInWld`;
- `wo` / `wO` → `omegaOutWld`;
- transformed variants → `omegaInObj`, `omegaInLocal`, etc.;
- generic traversal values remain `dirRayWld`,
  `dirShadowWld`, `dirEntryWld`, or `dirExitWld`.

This includes `LightSample::wI`, all usages in lighting and visibility,
`renderer/lights/lightSamplers.cpp`, `delegate/light.cpp`, integrator and medium
APIs, and unconstrained first-party MaterialXCpp BSDF interfaces. Do not rename
vendored BSDL identifiers or MaterialX-authored port tokens. `21-plan-api-contracts.md`
must document `LightSample::omegaInWld` and its normalization convention.

## Issue 4: `Li`/`Le` and PDF-measure letters remain abbreviated

Rename first-party transported radiance and PDF fields:

- `Li` → `radianceIn`;
- `Le` → `radianceEmitted`;
- ambiguous accumulated `L`/`lightContrib` values → a role-specific
  `radiance...` name;
- `pdfW` / `invPdfW` → `pdfSolidAngle` / `pdfSolidAngleInverse`;
- `pdfA` / `invPdfA` → `pdfArea` / `pdfAreaInverse`.

Keep mathematical paper notation only in prose/formulas. Coordinate
`LightSample` field changes with `21-plan-api-contracts.md`.

## Issue 5: medium coefficient names hide their physical meaning

Across the renderer and first-party MaterialXCpp code, rename `sigmaA`,
`sigmaS`, and `sigmaT` to `absorption`, `scattering`, and `extinction`. Apply the
same full-root convention to transformed forms:
`sigmaTStar` → `extinctionStar`, `effectiveSigmaT` →
`extinctionEffective`, and `outSigmaT` → `extinctionOutput`. Preserve `sigma_t`
only in cited equations, source names,
or externally constrained interfaces.

## Issue 6: `renderer/integrator/sss.{h,cpp}` mixes snake_case and camelCase

Convert every first-party identifier to lower camelCase while also applying the
full roots above. Examples include `phase_log` → `phaseLog`,
`guided_fraction` → `guidedFraction`, `have_opposite_interface` →
`haveOppositeInterface`, `rand_a` / `rand_b` → `u1` / `u2`,
`cosTheta_ent` → `cosThetaEntry`, and the complete
`*Eff`, `*PdfFactor`, `*Stretched...`, and `*GuidedPdf` families. Keep exact
mathematical notation in equations and citations.

## Sequencing

Land small, individually reviewable rename families in this order:

1. Establish the schema/table documentation and rename `LightSample` direction,
   radiance, distance, and PDF fields together so its contract is coherent.
2. Rename normal families, preserving surface/material values and SSS guide
   behavior.
3. Rename `omegaIn` / `omegaOut` throughout integrator and first-party BSDF
   boundaries.
4. Rename `iorIn` / `iorOut` and enforce `eta = iorIn / iorOut`.
5. Rename radiance, PDF-measure, throughput, and medium-coefficient families.
6. Convert remaining SSS casing and descriptive locals.

Do not combine a change from the SSS material guide normal to a geometric
normal with these renames. That is a behavior change.

The repository-wide audit is deliberately deferred to
`20-plan-naming-audit.md`, after the structural and namespace plans stop moving
identifiers.

## Validation

- Every rename-only commit is bit-identical under `oiiotool --diff` with
  `usdrender -s "{settings}.ty:randomNumberSeed = 1"`. Exercise front/back double-sided,
  normal-mapped, glass, SSS, lit, volume, and dielectric scenes.
- `pixi run ctest --test-dir build -R 'testHdEmbree|testMaterialXCpp' --output-on-failure`
  passes.
- Run an identifier-aware scan over the identifiers changed by this plan. The
  exhaustive first-party scan belongs to `20-plan-naming-audit.md`.
- Specifically verify that normal role/space/orientation suffixes, omega
  direction/space suffixes, IOR side suffixes, and PDF measures agree with the
  table at every declaration and call site.

## Documentation

Explain this naming schema immediately before the common-quantity table in
`ARCHITECTURE.md`, then copy the complete finalized table there as the
authoritative project convention. Include the suffix order, no-single-letter
rule, transport-side meaning of `In`/`Out`, coordinate-space requirements and
fixed abbreviations, normal-orientation vocabulary, IOR/eta distinction, and
external API exceptions.

Update `AGENTS.md` to summarize the new roots (`normal`, `omega`, `radiance`,
`ior`, `eta`, full absorption/scattering/extinction names) and point to
`ARCHITECTURE.md` for the full schema. Update all affected
`ARCHITECTURE.md` prose so it uses
`normalGeomWldExt`, `normalGeomWldOut`,
`normalSrfWldExt`, `normalSrfWldOut`,
`normalShdWldOut`, `omegaInWld`, `omegaOutWld`, `iorIn`,
`iorOut`, and `eta` consistently.

## Mandatory final suite gate

After every plan-specific validation above, run the complete Typhoon suite as
the final gate:

```sh
cd /path/to/typhoon-test-suite
powerprofilesctl launch --profile performance -- pixi run pytest --renderer typhoon-local
```

Run the complete suite to completion; never interrupt it because of elapsed
time. All tests must pass. Report elapsed time. For a performance-sensitive
change, compare the same workload before and after on the same machine and
investigate regressions. Do not commit the plan implementation until your
human has reviewed the completed changes and explicitly approved
committing them.
