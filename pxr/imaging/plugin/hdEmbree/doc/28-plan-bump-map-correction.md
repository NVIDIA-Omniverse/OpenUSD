# Plan: Cycles-aligned specular normal correction and sample rejection

Status: proposed correctness follow-up to
[`26-plan-fix-bump.md`](26-plan-fix-bump.md). It supersedes plan 26's
integrator-level bump-direction rejection. The goal is to match Cycles'
normal-handling behavior as closely as practical, with no new render setting:
specular normal correction is always on, as if Cycles'
`SD_USE_BUMP_MAP_CORRECTION` were fixed true.

## Problem

Three separate defects, in decreasing severity.

**1. Our specular normal correction flattens facets onto the geometry.**
`EnsureValidSpecularReflection()` (`materials/bsdf/microfacet.cpp:15-79`) ports
Cycles' quartic solve faithfully but adds four bail-outs Cycles does not have,
each returning `normalGeomWldOut`. Every one discards the mapped normal and
replaces it with the flat geometric normal, so those facets reflect as if
unmapped. This is the cause of run 0241's magenta contamination; see
[Measured diagnostics](#measured-diagnostics).

**2. Reflection and transmission disagree about the interface.** Correction is
applied to reflective leaves and `defaultSpecularNormal`
(`bsdf/closureTraversal.cpp:1779-1829`), while transmission still uses the
uncorrected normal in several delta and rough paths:
`SampleDeltaDielectricTransmission()` and
`SampleDeltaDielectricInterfaceTransmission()` take a separate
`normalShdWldOut` / `normalShdReflectionWldOut` pair (`bsdf/dielectric.h:275,305`).
Fresnel, reflection, and refraction therefore describe different surfaces at
the same hit.

**3. Nothing rejects a sample on the wrong side of the geometry.** A direction
labelled reflection may travel below the geometric surface, or a transmission
direction above it, and be used regardless. Cycles rejects both at sampling.

The three relevant fixtures use `specular_roughness = 0`, so they primarily
expose the delta dielectric path. The policy below must cover finite roughness
too, or the defect returns as soon as fixture or material roughness changes.

Run 0239 (`typhoon-fix-bump`) has clean colors but reversed mapped directions,
because it selected an incident-facing frame *before* evaluating the normal
map. A tangent-space map must be evaluated in the stable exterior tangent
frame; only the complete world-space result is oriented, and only by geometric
side.

## What Cycles actually does

Verified against `blender/cycles` `main`.

- `ensure_valid_specular_reflection(Ng, I, N)`
  (`src/kernel/closure/bsdf_util.h:322`) exists and is the algorithm our
  `EnsureValidSpecularReflection()` ports, including
  `threshold = min(0.9f * Iz, 0.01f)` and the `Ix < 0` root selection. It has
  exactly one fallback — `X = safe_normalize_fallback(N - dot(N, Ng) * Ng, N)`,
  which falls back to **`N`** — and returns `Nx * X + Nz * Ng` unconditionally,
  with no validation of the result.
- `maybe_ensure_valid_specular_reflection(sd, N)` (`bsdf_util.h:399`) gates it
  on `SD_USE_BUMP_MAP_CORRECTION` and skips curve primitives and the `N == Ng`
  case.
- It is applied at **closure setup**, per closure, in `src/kernel/svm/closure.h`
  and `src/kernel/osl/closures_setup.h`: principled specular and coat normals,
  conductor, glass, bssrdf, and translucent. Principled sheen keeps its
  uncorrected normal. The translucent call carries a `FIXME(weizhen)` saying it
  should apply only to glossy closures. Blender 4.0 moved correction out of the
  bump/normal-map node into this per-closure setup — the same place
  `PrepareShadingNormals()` occupies.
- The geometric sample-side test is separate and additional. In
  `bsdf_microfacet_sample`:

  ```c
  const float cos_NgO = dot(Ng, *wo);
  if ((cos_NgO < 0) != do_refract || (cos_NO < 0) != do_refract) {
      return LABEL_NONE;
  }
  ```

  with a comment stating the `Ng` check is deliberately sampling-only, to avoid
  shadow-terminator artifacts on smooth geometry. `Ng` is threaded into every
  `bsdf_*_sample`; eval and PDF never see it.
- This geometric test does not replace Cycles' separate
  `bump_shadowing_term()` (`src/kernel/closure/bsdf.h:86`). That function
  compares the exact closure normal with the smooth, unbumped normal `Ns`:
  evaluation applies its hemisphere-agreement rejection to every closure;
  sampling applies it to diffuse closures. With bump-map correction enabled it
  also multiplies diffuse response by a continuous GGX-based softening factor.
  PDF is unaffected.
- Cycles never faces the shading normal to the view. The only flip is the
  geometric backface test in `src/kernel/geom/shader_data.h:115-126`, which
  negates `Ng`, `N`, `dPdu`, and `dPdv` together — the whole hit frame, by
  geometric classification alone. `faceforward()` exists in
  `src/util/math_float3.h:500` but is not used for internal hit-normal
  orientation. The lone
  `bsdf->N = -N` (`bsdf_microfacet.h:1336`) models thin-glass double refraction
  as one reflection about the mirrored normal and passes `-Ng` to the sampler
  to match.
- The normal-map node compensates for that hit-setup flip while building the
  tangent frame, then flips only the completed mapped normal
  (`src/kernel/svm/tex_coord.h:220-302`). In the flat-shaded branch it
  explicitly undoes the orientation before the tangent-frame math:

  ```c
  normal = sd->Ng;
  /* the normal is already inverted, which is too soon for the math here */
  if (is_backfacing) { normal = -normal; }
  object_inverse_normal_transform(kg, sd, &normal);
  ...
  const float3 B = sign * cross(normal, tangent);
  N = safe_normalize(to_global(color, tangent, B, normal));
  object_normal_transform(kg, sd, &N);
  /* invert normal for backfacing polygons */
  if (is_backfacing) { N = -N; }
  ```

  The smooth-normal branch needs no un-flip because it reads the interpolated
  object-space normal attribute, which was never flipped. Either way the tangent
  frame is built in the authored exterior orientation and only the finished
  world-space normal is oriented, by `SD_BACKFACING`. This is the rule run 0239
  violated, and the upstream comment names the failure directly.
- When a microfacet lobe normal ends up pointing away from the view, Cycles
  drops that lobe rather than reorienting it: `cos_NI <= 0` returns
  `LABEL_NONE` in `bsdf_microfacet_sample` and `zero_spectrum()` in
  `bsdf_microfacet_eval`. Diffuse-family sampling does not use the outgoing
  direction and must not inherit this microfacet precondition.

## Decision

1. Fix `EnsureValidSpecularReflection()` to match Cycles' solve for
   well-conditioned inputs: fall back to the input shading normal, never to the
   geometric normal, and never validate the corrected result. Retain the two
   explicit robustness guards documented below.
2. Keep correction unconditional. No render setting; behave as Cycles does with
   `SD_USE_BUMP_MAP_CORRECTION` set. Keep Cycles' `N == Ng` early-out as a cheap
   guard.
3. One normal per lobe, used for every quantity describing that interface:
   Fresnel, TIR, delta and rough reflection, delta and rough refraction,
   evaluation, and PDF.
4. Add Cycles' geometric sample-side rejection, per closure family, at sampling
   only.
5. Move the current smooth-base/material-normal agreement test from the
   integrator into each leaf, where the exact lobe normal is known, and port
   Cycles' diffuse softening. Apply this term during evaluation and diffuse
   sampling, never to PDF.
6. Do not face-forward a shading normal to `omegaOutWld`. Keep
   `FaceNormalShdWldOut(normalShdWldExt, frontFacing)` unchanged: geometric side
   is the only orientation applied.
7. When a resolved microfacet normal points away from `omegaOutWld`, that lobe
   contributes nothing rather than being reoriented, matching `cos_NI <= 0`.
   Do not impose this on diffuse-family lobes.

Evaluation order stays: normal-map evaluation in the view-independent exterior
frame, then validation of the graph normal against `normalSrfWldExt`, then each
authored lobe normal against the resolved exterior graph normal, then geometric
orientation by `frontFacing`, then correction.

The tangent frame that feeds `SurfaceClosure::ResolveNormal()` must stay in the
authored exterior orientation, matching Cycles' un-flip. It already is:
`surfaceShading.cpp:751-761` orthogonalizes the sampled tangent and bitangent
against `normalSrfWldExt` and fixes handedness against the same exterior normal,
and the underlying primvars are object-space values transformed to world without
any `frontFacing` flip. Treat this as an invariant to preserve — reorienting the
frame before evaluating a tangent-space map is exactly the run 0239 defect. Add
a regression test.

`frontFacing` remains immutable geometric state. It alone selects incident and
transmitted IOR order, inside/outside medium state, thin-film and absorption
side, and geometric ray offsets and boundary crossings. Never reconstruct those
from a shading-normal dot product.

## Deliberate deviations from Cycles

Record these in `ARCHITECTURE.md`; everything else should match.

- **Translucent correction: not adopted.** Cycles applies correction to the
  translucent closure but its own `FIXME(weizhen)` says that is a mistake and
  should apply only to glossy closures. Copying an acknowledged upstream bug is
  not parity worth having.
- **Subsurface correction: adopted.** Cycles corrects `bssrdf->N`
  (`svm/closure.h:495,1122`) with no such caveat, so add `SubsurfaceData` to the
  corrected set. This is a change from today's behavior.
- **Thin-walled transmission** is validated against the real
  `normalGeomWldOut`. Cycles' mirrored `-Ng` convention exists only because it
  reuses a reflection closure to model the event; we do not.
- **Numerical guards retained.** Cycles continues the quartic solve after its
  tangent fallback and assumes a non-zero denominator. Typhoon retains guards
  for a degenerate projected tangent and `quadraticA <= kEpsilon`, but they
  return the input shading normal rather than flattening to the geometric
  normal. These are robustness deviations, not an exact line-for-line port.

## Cycles-compatible sample validity

Correction guarantees only that the *mirror* direction clears the geometric
surface. It says nothing about a rough sample, a refracted direction, or any
lobe that is not corrected. Match Cycles per closure family, including its
tangent-boundary behavior, which is not uniform upstream:

| Closure family | Test on the sampled direction |
| --- | --- |
| Diffuse (Oren-Nayar, Burley), sheen | `Dot(normalGeomWldOut, omegaInWld) > 0`, matching `bsdf_diffuse_sample` / `bsdf_sheen_sample`. Sampling constructs a direction above the lobe normal |
| Translucent | `Dot(normalGeomWldOut, omegaInWld) < 0`, matching `bsdf_translucent_sample` |
| Microfacet — dielectric, conductor, coat, generalized Schlick, coupled interface | `(cos_NgO < 0) != isTransmission || (cos_NO < 0) != isTransmission` rejects, matching `bsdf_microfacet_sample`. Tangent counts as reflection here, unlike the diffuse family |

The outgoing direction must satisfy `Dot(normalShdLobeWldOut, omegaOutWld) > 0`
before sampling; otherwise the lobe yields no sample. If the sampled direction
fails its test, return an invalid sample (`pdfSolidAngle == 0`) without
resampling or renormalizing the remaining distribution. A TIR result is
reflection, even when reached through a transmission branch. Thin-walled
straight-through is transmission and normally passes; presence and medium-only
boundaries are not BSDF samples.

Apply the `normalGeomWldOut` test only to generated samples. Do **not** add it to
`EvalSurface`, `PdfSurface`, direct-light BSDF evaluation, or their Adobe
equivalents. Cycles makes that geometric sampling/evaluation split deliberately,
to avoid geometric-normal shadow-terminator artifacts. This does not exempt
evaluation from the separate smooth-base/material-normal agreement test.
Visibility, geometric ray offsets, and the existing smooth shadow-terminator
offset remain responsible for direct-light occlusion.

Two consequences to accept explicitly, both shared with Cycles:

- A rejected sample is lost, not redirected or resampled. At grazing angles this
  is a systematic energy loss, not just added variance.
- `PdfSurface` keeps returning a non-zero BSDF PDF for directions BSDF sampling
  can never generate, so the power heuristic down-weights the NEE strategy
  without the BSDF strategy paying it back.

Both darken grazing normal-mapped facets relative to true displacement.

Because correction is what makes a corrected delta reflection pass the
reflection-side test, that test is close to a no-op for corrected lobes. It
still matters for rough samples off a corrected normal, for every transmission
direction, and for uncorrected lobes such as diffuse and sheen. It is not dead
code.

## Event representation

Add `bool isTransmission = false` to `Bsdf::BsdfSample`. `eta != 1` and a
geometric boundary crossing cannot replace it: thin-walled transmission can have
`eta == 1`, and the geometric crossing is the condition being validated.

Set the flag where the leaf event becomes known and preserve it through
selection and composite nodes. Relabel TIR as reflection before validation.
`isSubsurface` marker samples carry no direction; their `isTransmission` value
is meaningless and must not be validated. No debug assert that the label agrees
with the geometric crossing — the rejection test makes that a tautology for
accepted samples.

## Implementation inventory

### 1. Correct the correction

`renderer/materials/MaterialXCpp/materials/bsdf/microfacet.cpp:15-79`

Remove every fallback to `normalGeomWldOut` and delete the final validation.
Match Cycles' solve where its preconditions are well-conditioned, while keeping
the two robustness guards recorded under
[Deliberate deviations from Cycles](#deliberate-deviations-from-cycles):

| Site | Now | Change to |
| --- | --- | --- |
| `:33` degenerate tangent | `return normalGeomWldOut` | `return normalShdLobeWldOut` |
| `:42` `quadraticA <= kEpsilon` | `return normalGeomWldOut` | `return normalShdLobeWldOut` |
| `:63` root outside `(1e-5, 1+1e-5]` | `return normalGeomWldOut` | remove the bail-out; compute `sqrt(max(1 - Nz2, 0))` and `sqrt(max(Nz2, 0))` exactly as Cycles' two `safe_sqrtf` calls do; do not clamp `Nz2` to one |
| `:76` final reflection re-check | ternary to `normalGeomWldOut` | return `corrected` unconditionally |

The `:76` re-check is the important one: the quartic is constructed to land the
reflection exactly at `threshold`, so a float re-test fails marginally often and
each marginal failure snaps one facet flat.

This alone resolves the reported bug. It is small, independently landable, and
should go in first.

### 2. Prepare one corrected normal per lobe

`renderer/materials/MaterialXCpp/materials/bsdf/closureTraversal.{h,cpp}`

- Keep `PrepareShadingNormals()`'s signature and the exterior-frame validation
  order. No setting to thread.
- Add `SubsurfaceData` to the corrected leaf set; leave `TranslucentData`
  uncorrected. See [Deliberate deviations](#deliberate-deviations-from-cycles).
- Add Cycles' `N == Ng` early-out before calling the correction.
- Update the contract to state the validation order and that interface-side
  classification stays with `frontFacing`.

`renderer/materials/MaterialXCpp/materials/closureTree.h`

- Keep `defaultDiffuseNormal`, `defaultSpecularNormal`, and
  `hasDefaultSpecularNormalNodes`. Cycles corrects per closure type, so the
  split is load-bearing.
- `ResolveTreeShadingNormal` (`closureTraversal.cpp:63-77`) keeps its
  diffuse/specular classification; extend it for the subsurface change.
- `CausticClassPruner` (`closureTraversal.cpp:85-86`) keeps copying both
  defaults and preserving prepared authored normals.

`FaceNormalShdWldOut()` and its call sites (`pathIntegrator.cpp:377`,
`surfaceShading.cpp:918`, `unlitIntegrator.cpp:143`), `TryResolveNormalShdWldExt()`,
the smooth shadow-offset helpers, and `path.lastLightSamplingNormal`
(`pathIntegrator.cpp:722`) are all unchanged.

### 3. Hemisphere rejection at the leaf-sampling boundary

`renderer/materials/MaterialXCpp/materials/bsdf.h`

- Add `BsdfSample::isTransmission`.
- Add `BsdfSample::normalShdLobeWldOut`, valid only on a native
  `isSubsurface && !hasSubsurfaceEntryDirection` marker. Do not overload
  `omegaInWld` with a normal.
- Extend `SampleSurface()` with finite unit `normalSrfWldOut` and
  `normalGeomWldOut`.
- Extend `EvalSurface()` with finite unit `normalSrfWldOut`. PDF remains
  independent of both smooth and geometric normals.
- Update contracts to distinguish the lobe shading normal from immutable
  geometric side state and to document invalid wrong-hemisphere samples.

`renderer/materials/MaterialXCpp/materials/bsdf.cpp` and
`renderer/materials/MaterialXCpp/materials/bsdf/closureTraversal.{h,cpp}`

- Thread `normalSrfWldOut` and `normalGeomWldOut` through `SampleSurface()` and
  every recursive `SampleNode()` call. Thread only `normalSrfWldOut` through
  `EvalSurface()` and every recursive `EvalNode()` call. Do not thread either
  through PDF.
- Add one small leaf-finalization operation which receives the exact
  `normalShdLobeWldOut`, the event label, and the closure family, applies that
  family's test, and only then calls `_FinalizeSubtreeSample()`.
- Do the test before full-subtree eval/PDF recomputation. An invalid selected
  child stays invalid through `Mix`, `Layer`, `Add`, and `Multiply`; do not
  rescue it with another lobe's PDF.
- Do not route a wrong-hemisphere rough result through an existing delta
  fallback. Those fallbacks remain only for a rough sampler's own numerical or
  degenerate failure; geometric rejection terminates the selected sample.
- Keep low-level mathematical samplers free of geometric-scene state. The
  traversal, legacy, and backend ownership boundary validates their result.
- Port Cycles' `bump_shadowing_term` at the leaf boundary, using
  `normalSrfWldOut` as `Ns` and the exact `normalShdLobeWldOut` as `N`:
  reject smooth-base/material-normal disagreement for every evaluated lobe and
  for sampled members of Cycles' diffuse closure family; apply the continuous
  GGX softening only to that family's BSDF values when correction is enabled.
  This family includes diffuse, translucent, and sheen here and is not the
  caustic-ancestry meaning of `BsdfSample::isDiffuseLike`; keep those concepts
  separate. Never modify PDF. Put the mxcpp helper with BSDF traversal/math
  rather than retaining a Gf-typed integrator policy helper.
- Avoid double application: non-discrete samples are re-evaluated by
  `_FinalizeSubtreeSample()`, so their softening comes from leaf evaluation;
  sample finalization performs the pre-evaluation validity decision but does
  not multiply the same factor again.

Audit every leaf, including fallback and TIR paths:

| Leaf/path | Event labels to set and validate |
| --- | --- |
| Oren-Nayar and Burley diffuse | reflection |
| Translucent | transmission |
| Subsurface marker and SSS entry | marker has no direction but carries the selected corrected lobe normal; generated entry direction is transmission |
| Dielectric | reflection, transmission, and TIR-as-reflection; delta, isotropic GGX, anisotropic GGX |
| Coupled dielectric interface | selected reflection/transmission, TIR-as-reflection, thin-walled transmission; delta and rough |
| Conductor | reflection; delta, isotropic GGX, anisotropic GGX |
| Generalized Schlick | reflection, transmission, and TIR-as-reflection; delta and rough |
| Sheen | reflection |
| Adobe OpenPBR | derive event from `OpenPBR_BsdfLobeTypeTransmission`; validate diffuse, glossy, singular, coat/fuzz, and transmission results |
| Legacy summary | diffuse/specular/coat reflection and delta transmission |
| Composite nodes | preserve the selected child's label and invalid state |

Audit that every microfacet leaf's eval and sample return zero or an invalid
sample when `Dot(normalShdLobeWldOut, omegaOutWld) <= 0`, matching
`cos_NI <= 0`; `microfacet.cpp:93` already does this in its local frame.
Legacy specular and coat components need the same rule, while legacy diffuse
does not. Confirm diffuse, sheen, and translucent remain governed by their own
incoming-direction hemisphere rules and are not accidentally given this
microfacet precondition.

Copy `BsdfSample::normalShdLobeWldOut` from the native marker into a
correspondingly named `_SubsurfaceInput` field rather than reusing its current
graph-normal field. Pass that normal plus `normalSrfWldOut` and
`normalGeomWldOut` through `renderer/integrator/sss.cpp:902` /
`Bsdf::SampleSubsurfaceEntry()`, use it to construct the actual entry direction,
and reject an entry that is not below both the lobe and geometric normals.
Adobe markers already carry a sampled entry direction and do not consume this
native marker payload. Add a test on the generated entry direction, not merely
on prepared `SubsurfaceData::normal`.

### 4. One normal per interface

`renderer/materials/MaterialXCpp/materials/bsdf/dielectric.{h,cpp}` and
`closureTraversal.cpp`

- Pass the same `normalShdLobeWldOut` to `WouldTotalInternalReflect()`,
  Fresnel-angle calculation, reflection, and refraction. That normal is the
  corrected one, including for refraction; Cycles does the same for its glass
  closure.
- Remove the paired `normalShdWldOut` / `normalShdReflectionWldOut` parameters
  from `SampleDeltaDielectricTransmission()` and
  `SampleDeltaDielectricInterfaceTransmission()` (`dielectric.h:275,305`).
- Ensure a delta fallback from failed rough transmission is labelled and
  validated exactly like a direct delta sample.
- Keep `backside = !frontFacing`; do not derive it from a shading normal.
- Audit eval and PDF as well as sampling. Replace every graph-normal use in
  dielectric, coupled-interface, and generalized-Schlick leaf logic where the
  quantity belongs to the lobe, including all `IsSameSide()` classifications,
  `TransmissionFresnelCosTheta()`, `EvalGGXTransmission()`,
  `PdfGGXTransmission()`, and their coupled/anisotropic equivalents. Recursive
  traversal still passes the graph normal only as the fallback from which the
  leaf resolves its exact normal.

`renderer/materials/MaterialXCpp/materials/bsdf/legacySurface.{h,cpp}`

- Thread `frontFacing` into sampling and use it for IOR order. The current
  `Dot(normalShdWldOut, omegaOutWld) < 0` test becomes dead.
- Validate the selected legacy leaf before returning it.

`renderer/materials/MaterialXCpp/materials/adobeOpenPbr.{h,cpp}`

- `PrepareAdobeOpenPbrSurface()` (`adobeOpenPbr.h:42`, `.cpp:673`) gains
  `normalSrfWldOut`, `normalGeomWldOut`, and `frontFacing`, stored in
  `AdobeOpenPbrPreparedSurfaceState`. Update the call site at
  `pathIntegrator.cpp:471`.
- `SampleAdobeOpenPbr()` (`.cpp:816`) builds its own prepared state at `.cpp:790`
  and needs the same three values. Update the call site at
  `closureTraversal.cpp:1653`.
- Keep reading `defaultSpecularNormal` (`.cpp:696`); the split survives.
- Derive `isTransmission` from the backend lobe type, use `frontFacing` for eta
  order, and select the validity family from the backend type: diffuse
  reflection/transmission use the diffuse/translucent `Ng` rule; specular,
  singular, and coat events use the microfacet `N` plus `Ng` rule. Apply it to
  both prepared and unprepared samples.
- Store `normalSrfWldOut` in prepared state. Apply the smooth-base agreement
  term to Adobe evaluation using its single corrected backend normal, multiply
  diffuse results by the continuous softening factor, and leave Adobe PDF and
  geometric eval rejection unchanged. Thread the smooth normal through
  `TryEvalPdfAdobeOpenPbrSurface()` and the prepared eval path used by
  `lighting.cpp:265-267`. Cover builds both with and without
  `PXR_HDEMBREE_ENABLE_ADOBE_OPENPBR`.

### 5. Move late, frame-inaccurate rejection into leaves

`renderer/integrator/pathIntegrator.cpp`

- Pass `normalSrfWldOut` and `normalGeomWldOut` to BSDF and SSS sampling, and
  pass `normalSrfWldOut` to BSDF evaluation.
- Delete the diffuse-only `BumpDirectionIsValid()` continuation check (`:631`).
  Its policy now runs during leaf sampling with the exact lobe normal; keeping
  both would apply it twice.
- Retain `isDiffuseLike` for caustic ancestry; it is unrelated and not dead.
- Continue computing `crossesBoundary` from `normalGeomWldExt` and use it for
  medium transitions, eta propagation, ray offsets, and caustic topology.

`renderer/integrator/lighting.cpp`

- Delete the `BumpDirectionIsValid()` direct-light rejection (`:227`) only
  after the equivalent smooth-base agreement test runs inside every leaf
  evaluation. The current call is too coarse because it uses the graph normal;
  deleting it without the leaf replacement would diverge from Cycles and allow
  rear-side bump light leaks.
- Retain lobe evaluation's shading-hemisphere checks, the smooth
  shadow-terminator lift, the smooth-base transmission classification used for
  the visibility offset, and shadow tracing.

## Dead code removed

- The Gf-typed `BumpDirectionIsValid()` in
  `renderer/integrator/shadingNormal.h:95-106`, its two integrator call sites,
  and `_TestBumpDirectionValidity()` in
  `testenv/testHdEmbreeMaterialNormals.cpp`. Its behavior is not deleted: an
  mxcpp leaf-level agreement/softening helper and focused BSDF tests replace it.
- The second reflection normal parameters on the delta dielectric transmission
  helpers.
- Backside inference from faced shading-normal dot products in legacy and Adobe
  sampling.
- The final result validation inside `EnsureValidSpecularReflection()`.

Explicitly retained: `EnsureValidSpecularReflection()` itself and its
`TestEnsureValidSpecularReflection` coverage; `FaceNormalShdWldOut()` and
`_TestExteriorNormalFacing()`; `defaultDiffuseNormal`, `defaultSpecularNormal`,
and `hasDefaultSpecularNormalNodes`; `normalSrfWldOut`, which lighting uses for
smooth shadow-terminator offsets and smooth-base transmission-side visibility
and which path differentials use for displaced-frame propagation.

The `normal` AOV is unaffected: `renderer/aov/aovOutput.cpp::_ComputeNormal`
uses geometric normals only. No `overview.dox` contract change is expected, and
no render setting is added.

## Tests

### Focused unit tests

Extend `TestEnsureValidSpecularReflection` in
`renderer/materials/MaterialXCpp/tests/testMaterialXCppBsdf.cpp:5155`:

- each concrete input that reached an old `normalGeomWldOut` bailout now
  returns the input normal for a retained robustness guard or the Cycles solve;
  do not assert that a valid limiting solution can never equal `Ng`;
- sweeping normal tilt against view angle, the output is continuous in the
  input — a snap to the geometric normal is a discontinuity, and this is the
  assertion that would have caught the shipped bug;
- the corrected mirror reflection clears `threshold` for inputs where the solve
  is well conditioned, and degenerate inputs return the input normal rather than
  the geometric normal.

New coverage:

- prepared normals correct the reflective set plus subsurface, and leave
  translucent and diffuse uncorrected;
- the `N == Ng` early-out returns the input untouched;
- interface side and eta order stay fixed by `frontFacing`;
- diffuse and sheen accept only `Dot(Ng, omegaIn) > 0`; translucent only `< 0`;
  coat and the other microfacet families use the two-normal test with tangent
  counted as reflection;
- smooth-base/material-normal disagreement zeros every evaluated lobe and
  sampled diffuse-family lobe using the exact authored lobe normal; diffuse,
  translucent, and sheen BSDF values receive continuous softening while PDFs
  do not change, independently of caustic `isDiffuseLike` classification;
- TIR is validated as reflection;
- coupled dielectric, generalized Schlick, legacy, Adobe, and all four composite
  node kinds preserve the event flag and the rejected state;
- Adobe diffuse and specular sampled types select different validity families;
- rejection returns zero PDF and does not resample;
- eval and PDF do not acquire a geometric-normal rejection;
- a microfacet lobe whose normal points away from `omegaOutWld` contributes
  nothing and is not reoriented, while a diffuse-family lobe is not incorrectly
  discarded for that reason;
- one dielectric case asserts the same normal reaches Fresnel, TIR, reflection,
  refraction, evaluation, and PDF, so the two-frame defect cannot return;
- a native subsurface marker carries its selected corrected lobe normal and the
  generated SSS entry direction demonstrably uses it rather than the graph
  normal;
- a tangent-space normal map resolves to the same exterior world-space normal on
  a front- and a back-face hit of the same surface, differing only in final
  sign. This pins the run 0239 defect.

Add a table-driven test over closure family crossed with event type crossed with
shading/geometric hemisphere agreement, so delta and finite-roughness paths
cannot drift apart.

Update existing preparation, pruning, delta fallback, and Adobe prepared-state
tests in `testMaterialXCppBsdf.cpp` and `testMaterialXCppMaterials.cpp` for the
new parameters, and update every direct `EvalSurface`, `EvalNode`,
`SampleSurface`, `SampleNode`, legacy, SSS, and prepared/unprepared Adobe call
in the test target. Update the production calls in `lighting.cpp` and
`pathIntegrator.cpp` in the same API-change step.

`testenv/testHdEmbreeMaterialNormals.cpp`: delete `_TestBumpDirectionValidity()`
and its registration only after equivalent table-driven mxcpp coverage exists.
`_TestExteriorNormalFacing()` and the
`TryResolveNormalShdWldExt` cases are unchanged and must still pass — they encode
the geometric-orientation rule this plan keeps.

Per `AGENTS.md`, launch an adversarial review agent after implementing the tests.
It must check that the tests exercise both geometric windings, that they
distinguish geometric orientation from correction, that the continuity assertion
would actually fail against the pre-fix `microfacet.cpp`, and that they do not
merely encode the new implementation.

### Image acceptance: glass planes

Use these fixtures in `/home/anders/code/typhoon-test-suite/test-suite`:

- `openPbr_displacement_glass_planes_20x20.usda` — geometric ground truth;
- `openPbr_normal_map_glass_planes_20x20.usda`;
- `openPbr_height_to_normal_glass_planes_20x20.usda`.

Render all three with the same local build, fixed `ty:randomNumberSeed = 1`,
camera, sample count, and axis-color environment. Compare the plane facing the
camera and the reversed-winding plane.

Acceptance criteria:

- normal-map and bump-map facet directions broadly match displacement on both
  planes;
- each interior angled facet has the same dominant axis color as the
  corresponding displaced facet. This is a test of the specular reflection path:
  disabling specular reflection collapses both the normal-mapped and the
  displaced render to flat cyan, so all per-facet axis color arrives through
  mirror reflection and none through transmission;
- the reversed `N` on the right-hand plane has no broad magenta mixing from the
  environment direction behind the camera;
- grazing facets may be darker than true displacement. That is the expected cost
  of unrenormalized sample rejection plus the MIS asymmetry, and is not a
  regression. Broad hue shifts and magenta contamination are regressions;
- silhouette, tessellation, texture-filtering edges, and ordinary Monte Carlo
  noise need not be pixel-identical to true displacement;
- run 0239's clean-but-reversed direction and run 0241's correct-but-mixed color
  are both regressions, not acceptable alternatives.

Suggested focused run:

```sh
cd /home/anders/code/typhoon-test-suite
pixi run pytest --renderer typhoon-local \
    test-suite/openPbr_displacement_glass_planes_20x20.usda \
    test-suite/openPbr_normal_map_glass_planes_20x20.usda \
    test-suite/openPbr_height_to_normal_glass_planes_20x20.usda
```

Do not update reference images until the human has inspected the renders side by
side and accepted the changed behavior.

## Measured diagnostics

Recorded because they scope the defect and because two of them were initially
misread.

**Raising stubbed to return `normalGeomWldOut`: every facet renders completely
magenta.** This experiment was invalid. Returning the geometric normal does not
disable correction — it flattens every specular normal onto the plane, so all
facets reflect identically. Conclusions drawn from it, including a prediction
that geometric rejection would erase the specular contribution, are withdrawn.

**Same normal map on a base-color-only openPBR material: relief broadly matches
the displaced reference.** The resolved shading normals are correct, and the
whole preparation chain — `ctx.normal = normalSrfWldExt`
(`surfaceShading.cpp:794`), the exterior tangent frame (`:751-788`), the
`2c - 1` decode and `T*x + B*y + N*z` composition (`geometricNodes.cpp:261-292`),
and `normalSpace = World` avoiding a second tangent transform
(`openPbr.cpp:404`) — is sound. There is no upstream normal bug beneath this
plan.

**Disabling specular reflection collapses both the normal-mapped and the
displaced render to flat cyan.** All per-facet axis color, including in the
ground truth, arrives through mirror reflection.

**Correction stubbed correctly, returning `normalShdLobeWldOut` unchanged:
matches the displaced reference closely, with no magenta contamination.** So
reflection off the mapped normals produces the right per-facet colors, and those
reflections do not fall below the flat plane on most facets.

**Correction re-enabled with the two retained guards changed to return the
shading normal, the invalid-root bailout removed in favor of Cycles-style safe
square roots, and the final re-check removed: matches the displaced reference as
well as the no-correction stub.** This confirms the root cause. The defect was
our port falling back to `Ng` instead of preserving or solving from `N`, not the
correction algorithm and not a design disagreement with Cycles. Section 1 of the
inventory is the fix.

## Residual concern: `Ng` rejection targets the flat plane

On the displaced reference there is no frame disagreement: the geometric normal
*is* the facet normal, so a mirror reflection never falls below the local
geometric surface. On the normal-mapped plane `normalGeomWldOut` is the flat
plane while the lobe normal is the relief, so the geometric hemisphere test
compares against the plane rather than against the relief the map represents.
Wherever a facet is steep enough that its reflection clears the relief but not
the plane, the test rejects a sample displacement would have kept.

The corrected stub shows this is not the common case on this fixture, so the cost
is bounded and probably small. Measure it rather than assume: once rejection
lands, compare rejected-sample counts and facet brightness against the displaced
reference. Cycles accepts the same limitation.

## Implementation sequence

1. Fix `EnsureValidSpecularReflection()` (inventory section 1) with the extended
   unit coverage. Small, independently landable, and resolves the reported bug.
   Render the three fixtures and confirm before moving on.
2. Land the signature changes together with red unit coverage:
   `BsdfSample::isTransmission`, the native subsurface-normal payload,
   `normalSrfWldOut` on evaluation and sampling, and `normalGeomWldOut` on
   `SampleSurface`/`SampleNode`/`SampleSubsurfaceEntry` and the two Adobe entry
   points. Tests cannot compile before this, so it cannot be a separate earlier
   step.
3. Apply the preparation changes in inventory section 2, including the
   subsurface correction change and the `N == Ng` early-out.
4. Collapse each interface onto one lobe normal (inventory section 4).
5. Add event labels, geometric rejection, smooth-base agreement, and diffuse
   softening to native traversal, composites, legacy, and SSS.
6. Apply the corresponding family-specific sampling and evaluation policy to
   the optional Adobe backend.
7. Remove the superseded integrator and NEE calls and the remaining dead code.
8. Build, run unit and focused renderer tests, render the three glass-plane
   fixtures, and have the tests adversarially reviewed.
9. Update `ARCHITECTURE.md` with the final normal/interface invariants and the
   recorded Cycles deviations, and `README.md` with the user-visible
   mapped-normal behavior. Leave this plan and plan 26 as historical design
   records.
10. Run the complete external gate and report elapsed time:

    ```sh
    cd /home/anders/code/typhoon-test-suite
    powerprofilesctl launch --profile performance -- \
        pixi run pytest --renderer typhoon-local
    ```

## Required local validation

From the OpenUSD checkout:

```sh
pixi run build
pixi run build/pxr/imaging/plugin/hdEmbree/testMaterialXCpp
pixi run ctest --test-dir build \
    -R 'testHdEmbree.*(Material|Sampling|Render)' --output-on-failure
```

That regex covers `testHdEmbreeMaterialNormals`; run it explicitly if the regex
is narrowed.

All failures and non-zero image differences must be investigated. Steps 2 onward
thread two extra normals through sampling, one through evaluation, add a
subsurface marker payload, and add per-leaf arithmetic, so they are hit-time
sensitive and follow a
performance-regression investigation in
[`27-plan-perf-regression.md`](27-plan-perf-regression.md). Take the "before"
measurement on the current branch before step 2 lands, on a named fixed-seed
material-heavy workload; it is not recoverable afterwards. Record durable
measured results in `OPTIMIZATION.md` if the difference is material. Step 1
alone should be performance-neutral or slightly faster, since it removes a
reflection re-computation.
