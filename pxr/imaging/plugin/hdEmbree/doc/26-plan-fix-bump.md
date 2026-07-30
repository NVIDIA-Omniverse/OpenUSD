# Plan: Land the bump/shadow-terminator normal work on `typhoon`

Status: correctness follow-up. Source material is the three-commit branch
`typhoon-fix-bump`, forked from `typhoon` at `f66ecfb1f` and never rebased.

This plan is **not behavior-preserving**. It deliberately changes shading of
bump- and normal-mapped surfaces, tangent-driven materials, and shadow
terminators on coarsely tessellated smooth meshes.

## Issue

`typhoon-fix-bump` fixes four real defects, but it was written against the
pre-naming-audit tree and carries a set of loose ends that must not land as-is.
The three commits are:

| Commit | Subject |
| --- | --- |
| `78f5b9426` | Fix hdEmbree material normal faceting |
| `21352666c` | Fix MaterialX tangent interpolation |
| `eea05c844` | Fix view-dependent normal map shading in hdEmbree |

The defects they fix are genuine:

- **Facet leak through smooth bump.** Material normals are validated against
  the coarse facet `normalGeomWldExt`, so a bump-perturbed normal that is
  legitimately above the interpolated vertex normal but below the individual
  triangle is rejected and falls back to the base normal. Triangulation shows
  through as faceted patches.
- **View-dependent shading.** Material normals are additionally rejected when
  `dot(N, omegaOutWld) <= 0`, and every closure leaf face-forwards its normal to
  `omegaOutWld`. Shading therefore changes with camera direction. This is not a
  diffuse-only defect: `ResolveReflectionNormal` face-forwards the same way, so
  a `coat_normal` highlight jumps sides at grazing angles. `eea05c844` fixes it
  for diffuse only; this plan fixes it everywhere (fix 4 below).
- **Faceted tangents.** `ND_tangent`/`ND_bitangent` return `dPdu`-derived
  vectors, which are triangle-constant, so any tangent-driven material facets.
- **Shadow terminator.** Shadow rays start on the coarse facet, so a coarsely
  tessellated smooth mesh self-shadows in patches near the terminator.

`typhoon` tip still contains the original logic unchanged; the intervening
commits (`19`–`23`) renamed the identifiers but touched none of the behavior.

## Goals

- Land all four fixes on `typhoon`.
- Resolve the fourteen loose ends listed under [Fixes required before
  landing](#fixes-required-before-landing).
- **End with fewer special cases than the tree has today**, not more. The branch
  fixes the view dependence by adding a second normal resolver and a lobe-type
  branch to select between them. Landing that shape would trade one bug for one
  fork in the code. Fixes 1, 2, and 4 instead collapse normal resolution and
  direction validation to one function each, applied to every lobe. If the
  landed change adds a branch on lobe type, side, or view anywhere in
  `bsdf/shadingFrame.h` or `bsdf/closureTraversal.cpp`, it is not finished.
- Leave the tree consistent with the naming, namespace, include, and API
  contract rules established by plans `05`, `19`, `20`, and `21`.

## Non-goals

- Do not preserve the `typhoon-fix-bump` commit boundaries. Only one of the
  three commits is portable (see below).
- Do not extend the smooth-terminator offset to refined or displaced
  prototypes. That is a separate question; record it in `TODO.md`.
- Do not change interface-side semantics. Which side of a dielectric is
  "outside" for IOR, thin film, and medium tracking is a separate question from
  which normal to shade with, and only the second one is in scope.

## Cherry-pick feasibility

Measured with `git merge-tree --write-tree --merge-base=<commit>^ typhoon
<commit>`:

| Commit | Result |
| --- | --- |
| `78f5b9426` (faceting) | **Conflicts.** 6 text files, plus `renderer/rendererImpl.h` modify/delete. |
| `21352666c` (tangents) | **Clean.** Applies with no conflict. |
| `eea05c844` (view-dependence) | **Conflicts.** 6 text files, plus two add/delete resolutions. |

Squashing all three into one merge against `typhoon` gives **19 conflict
hunks** across `ARCHITECTURE.md`, `lighting.cpp` (2), `pathIntegrator.cpp` (5),
`surfaceShading.cpp` (3), `unlitIntegrator.cpp` (1), `bsdf.cpp` (4), and
`renderer.h` (3), plus three files git cannot place at all.

The conflicts are not incidental. `typhoon` moved every affected surface:

- `renderer/rendererImpl.h` **no longer exists** (plan `14`/`16`). The
  `#include` that `eea05c844` added there has no home; each translation unit
  now includes what it needs directly.
- `bsdf.cpp`'s `_EvalNode`/`_PdfNode`/`_SampleNode` were extracted to
  `materials/bsdf/closureTraversal.{h,cpp}` as
  `Bsdf::detail::EvalNode`/`PdfNode`/`SampleNode` with published contracts
  (plan `17`, `21`). `_ResolveReflectionNormal`, `_FaceForwardNormal`, and
  `_NormalizeOrFallback` moved to `materials/bsdf/shadingFrame.h` as
  `ResolveReflectionNormal`/`FaceForwardNormal`/`NormalizeOrFallback`. The
  156-line `incidentN` threading in `eea05c844` has to be reapplied by hand
  against that structure, and the three `detail::` declarations need their
  doc contracts extended for the new parameter.
- `_SurfaceInteraction::baseNormalOut`/`GetIncidentBaseNormal()` are now
  `normalSrfWldExt`/`GetNormalSrfWldOut()`; `Ng` is `normalGeomWldExt`; `wo` is
  `omegaOutWld`; `wi` is `omegaInWld`; `_ToGf`/`_ToMx`/`_TryNormalizeDirection`
  are `ty::`-qualified.

**Recommended sequence.**

1. Apply `21352666c` onto `typhoon` unchanged (`git cherry-pick -n`; do not
   commit before review). It is clean, isolated to
   `nodes/geometricNodes.cpp` and its two node tests, and independently
   valuable.
2. Re-implement `78f5b9426` and `eea05c844` directly on `typhoon`, using the
   fix-bump diff as the reference rather than as the patch. Fold the fixes
   below in as you go.

Porting first and fixing second is the wrong order here: eight of the fourteen
fixes (7–14) are naming, include-style, and documentation-contract work that the
port has to do anyway. Doing them as a separate follow-up commit means touching
every conflicted line twice.

## Fixes required before landing

### Behavioral

**1. Apply one direction-validity test to every sampled direction.**
`eea05c844` gates the continuation-path test on `bs.isDiffuseLike`:

```cpp
if (bs.isDiffuseLike &&
    !HdEmbreeBumpDirectionIsValid(bsdfNormal, differentialNormal, wi)) {
    break;
}
```

Glossy, conductor, and dielectric-interface samples are then not checked at all
on the BSDF-sampling path (`pathIntegrator.cpp:616`) and rely entirely on each
lobe's internal `IsSameSide`. NEE still checks them, so the two paths disagree.

The lobe-type branch is unnecessary. `BumpDirectionIsValid` is

```
dot(Nsrf, omegaIn) * dot(Nsrf, Nshd) * dot(Nshd, omegaIn) >= 0
```

which is an *agreement* test, not a sidedness test, and it already handles
transmission: a transmitted direction has `dot(Nsrf, omegaIn) < 0` and
`dot(Nshd, omegaIn) < 0`, so the product stays non-negative and the sample
survives. It rejects exactly the case worth rejecting — the material normal and
the smooth base normal disagreeing about which side `omegaIn` is on. Apply it
unconditionally in both `lighting.cpp` and `pathIntegrator.cpp` and delete the
`isDiffuseLike` branch along with the geometric-agreement test it replaced.

The discarded geometric-agreement test also rejected samples passing below the
true facet, so removing it needs a replacement — see fix 2.

**2. Keep specular directions above the geometric normal by construction.**
Dropping the geometric-agreement test (fix 1) lets a bump-mapped glossy
reflection be sampled into the geometry. There are two obvious responses and
both are bad: reinstating the test restores exactly the facet-dependent
rejection this plan exists to remove, and doing nothing leaks light through open
and thin-walled meshes. An earlier draft of this plan proposed choosing between
them by experiment, which is not a design.

Take the third option, which is what Cycles does and what the terminator work in
`eea05c844` is already borrowing from. Cycles does not reject below-facet
specular directions and does not test agreement against the facet. It perturbs
the *microfacet* normal so the reflected direction stays above
`normalGeomWldExt` (`ensure_valid_specular_reflection`), and reserves
`bump_shadowing_term` — our `BumpDirectionIsValid` — for the diffuse-style
agreement case. The correction lives inside microfacet sampling, where the
half-vector is already in hand, instead of as a rejection branch in the
integrator.

That resolves the dilemma without a lobe-type branch anywhere in the integrator:
`BumpDirectionIsValid` applies unconditionally to every sampled and light
direction, and microfacet sampling independently guarantees it never emits a
below-facet direction in the first place. Port
`ensure_valid_specular_reflection` into `bsdf/microfacet.cpp` in the same change
that deletes the geometric-agreement test; landing fix 1 without it is the
light-leak case.

Validate on open, thin-walled, and closed geometry separately — the leak modes
differ and a closed mesh hides them.

**3. Keep `_Visibility` offsetting along the geometric normal.**
`eea05c844` changes the `_Visibility` call in `lighting.cpp:232` from
`normalGeomWldExt` to the base shading normal. That parameter is now
`dirOffsetReferenceWld` and `renderer.h:812` documents it as *"Surface events
supply their geometric normal"*; `AGENTS.md` states *"Geometric normals alone
own topology, medium transitions, and ray offsets."* Two separate mechanisms
are being conflated: the terminator lift (correct, new, carried by
`smoothShadowOffset`) and the self-intersection epsilon (`kRayBias` along the
offset reference). Pass the lifted origin and keep `normalGeomWldExt` as the
offset reference. If the lift alone is insufficient, that is a finding to
document, not a silent contract change.

**4. Resolve and validate every lobe's normal with one function.**
`eea05c844` adds `ResolveDiffuseNormal` beside the existing
`ResolveReflectionNormal` and routes only the two diffuse leaves to it. That
leaves the tree with two normal resolvers, a lobe-type branch selecting between
them, and the view dependence still live on every reflective lobe. Consolidate
instead.

The premise that makes consolidation safe: **the graph's own frame is the
incident frame.** `_BuildShadingContext` sets `ctx.normal` to
`interaction.GetNormalSrfWldOut()` and orthogonalizes the tangent frame against
that same vector (`surfaceShading.cpp:527`, `:673`–`:685`), so a tangent-space
normal map decoded inside the graph, and `ND_normal` itself, both emit
incident-facing world normals. For those sources no side transform is needed at
the leaf.

That premise covers the frame the renderer supplies; it does not cover every
value a graph can produce. `_ReadOptionalNormal` copies whatever `Vec3f` reached
the node's `normal` input, which may be an authored constant, an object- or
world-space normal map, or arbitrary graph math. So the resolver must *validate*
as well as normalize — which the face-forward it replaces was implicitly doing,
by clamping an inverted per-lobe normal into a usable hemisphere. Deleting the
face-forward without adding validation would turn an inverted `coat_normal` into
a silently black coat with no fallback, and nothing else would catch it: the
integrator's `BumpDirectionIsValid` only ever sees the top-level `bsdfNormal`,
never a leaf normal.

Which means `FaceForwardNormal(·, omegaOutWld)` is not transforming
`data.normal` at all — it is converting the leaf's *first* argument,
`interfaceN`, from exterior to incident orientation, using
`dot(N, omegaOutWld) > 0` as a proxy for `frontFacing`. The proxy is the bug:
it disagrees with `frontFacing = dot(normalGeomWldExt, omegaOutWld) > 0`
exactly when the shading and geometric normals disagree, which is what a bump
map is for. Threading the already-correct incident normal to the leaves — which
`eea05c844` does — removes the need for the proxy entirely.

Land one resolver that applies the *same* validation policy the top-level
material normal already gets — finite, non-degenerate, and in the base
hemisphere, otherwise fall back rather than negate:

```cpp
/// Returns the incident-facing shading normal for a closure leaf.
/// `normalShdWldOut` must be a finite unit vector on the incident side.
/// A per-lobe `data.normal` is accepted only when it is finite, non-degenerate,
/// and in the same hemisphere as `normalShdWldOut`; any other value falls back
/// to `normalShdWldOut` and is never negated to fit. Returns a finite unit
/// vector. Does not throw.
template<typename DataT>
inline Vec3f
ResolveShadingNormal(const DataT& data, const Vec3f& normalShdWldOut)
{
    Vec3f resolved;
    return data.hasShadingNormal &&
           TryResolveNormalShdWldOut(data.normal, normalShdWldOut, &resolved)
        ? resolved
        : normalShdWldOut;
}
```

This is the same `TryResolveNormalShdWldOut` the integrator uses on the
top-level normal, so the renderer ends up with **one** validation policy for
every shading normal it handles, at any depth. That is a reduction in special
cases, not an addition: today the top level rejects-and-falls-back while the
leaves silently negate.

The fallback should feed `_invalidMaterialNormalCount` the way the top-level
path does, so a broken per-lobe normal is observable instead of merely dark.

With the resolver in place, delete the surrounding machinery:

- `ResolveReflectionNormal` and `ResolveDiffuseNormal` both go; every leaf in
  `closureTraversal.cpp` calls `ResolveShadingNormal`. No lobe-type branch, no
  `omegaOutWld`, no flip.
- `FaceForwardNormal` goes. Its only remaining users are three sites in
  `bsdf/legacySurface.cpp:39,115,159`, which take `interfaceN` for the same
  proxy reason and take the incident normal directly instead. Move them in the
  same change or the special case survives in the one file nobody reads.
- `interfaceN` stops being a normal source. It is still needed for interface-
  side decisions — IOR entering/exiting, thin film — but that is a different
  question and should be spelled as what it means. Replace the parameter with
  an explicit `frontFacing` on the leaves that genuinely need it.
  `EvalLayerBaseThroughput` currently takes `interfaceN` and moves with them.

The remaining question is what a *valid* normal does at grazing incidence, where
`dot(normalShdWldOut, omegaOutWld) < 0` is reachable even for the unperturbed
base normal. Stating the policy explicitly, because the earlier draft did not:
**a validated normal is used as-is and the existing zero-cosine guards apply.**
`microfacet.cpp` already bails on `woLocal[2] <= 0.0f` / `cosThetaO <= 0.0f`,
and the diffuse leaves already bail on `Dot(shadingN, omegaInWld) <= 0.0f`.
Removing the flip makes those existing guards fire where the flip previously hid
the condition. The cost is grazing darkening of the same family as a shadow
terminator; the remedy, if it proves objectionable in the suite, is fix 2's
`ensure_valid_specular_reflection`, not a reinstated flip. Flipping the normal
does not recover that energy — it relocates the highlight to the wrong side of
the surface, which is the defect being fixed.

Two things this does not cover:

- An **object-space** normal map bypasses the tangent frame and arrives in the
  exterior frame. The resolver's hemisphere test rejects it and falls back to
  the base normal rather than shading with it — the same outcome the top-level
  `SurfaceClosure::ResolveNormal` path already produces today, so this is not a
  regression, but it is now a *deliberate* documented behavior rather than an
  accident. Record it in `ARCHITECTURE.md` alongside the contract that
  renderer-supplied graph frames are incident-facing.
- The reflective lobes change too, so this is a wider image diff than
  `eea05c844` alone. A `coat_normal` highlight that currently jumps sides at
  grazing angles will stop jumping. That is the fix, but it moves baselines.

**5. Decide and document the refined/displaced exclusion.**
`SetSmoothShadowOffset` is computed only when
`!prototypeContext->refined && !prototypeContext->displaced`
(`surfaceShading.cpp`), so subdiv and displaced prototypes get a zero lift and
no terminator fix. That is defensible — they are finely tessellated — but it is
currently undocumented and looks like an oversight. State the reasoning in
`ARCHITECTURE.md` and add a `TODO.md` entry.

**6. Measure the per-hit cost of the corner-normal fetch.**
`_SampleTriangleNormalCorners` runs two `dynamic_cast`s plus a
`primvarMap.find(HdTokens->normals)` on **every** non-refined surface hit,
before it is known whether any light will be sampled. Either hoist the sampler
resolution and the cast result into `PrototypeContext` at build time (the
sampler type is fixed per prototype), or profile and record the cost in
`OPTIMIZATION.md`. The generic fallback path is also dead work: it samples at
`(0, 0)` and replicates to all three corners, which yields exactly zero lift.

### Documentation

**7. `ARCHITECTURE.md` claims a `wo` test that no longer exists.**
`78f5b9426` wrote *"Material normals are validated against this smooth base
normal **and `omegaOutWld`**"*; `eea05c844` then removed the `omegaOutWld` term
and pinned the removal with `_TestViewBackfacingNormalIsAccepted`. The prose
must say the validation is against the smooth base normal alone, and that
`normalGeomWldExt` remains the authority for boundary classification and
continuation-ray offsets.

**8. `renderer.h` counter comment is stale for the same reason.**
`// Material normals rejected by finite/length/base/view-hemisphere checks.`
There is no view-hemisphere check. Say `finite/length/base-hemisphere`.

**9. The naming table's `Out` definition contradicts the plan — and already
contradicts the code.**
`ARCHITECTURE.md:133` says orientation uses *"`Out`/`In` only when faced toward
the corresponding transport direction."* This plan deliberately accepts a
`normalShdWldOut` with `dot(normal, omegaOutWld) < 0` and pins that with a test,
so the two cannot both stand.

The cheap resolution is the correct one, because the table is already wrong
about the existing code: `GetNormalSrfWldOut()` returns
`frontFacing ? normalSrfWldExt : -normalSrfWldExt`, and `frontFacing` is
`dot(normalGeomWldExt, omegaOutWld) > 0` — a *geometric side* test. A smooth
vertex normal at grazing incidence therefore already carries the `Out` suffix
while facing away from `omegaOutWld`. Amend the sentence so `Out`/`In` select a
transport **side**, determined by the geometric frontFacing test, rather than
claiming a per-vector facing guarantee.

No renames follow. Every existing `...Out` name is already side-based in
practice, so `renderer.h` contracts, the BSDF API, ray differentials, and cosine
factors are unaffected. This is one sentence of prose, but it must land in the
same change or the plan is self-contradictory on paper.

**10. `README.md` states two things this plan makes false.**
`README.md:285` says *"Materials evaluate in an exitant-facing frame on either
mesh side. Invalid or **boundary-crossing** normal-map results fall back to the
smooth/displaced normal."* Accepting normals below the coarse facet is precisely
a boundary-crossing result that no longer falls back. `AGENTS.md` requires a
user-visible change to update `README.md`, and this is user-visible rendering
behavior. Rewrite both sentences: materials evaluate in an incident-facing
frame; results are validated against the smooth base normal, and invalid or
inverted results — not boundary-crossing ones — fall back.

### Conventions

**11. Namespace and prefix.**
The new helpers are free functions at `PXR_NAMESPACE` scope with an `HdEmbree`
prefix. Plan `19` requires shared renderer declarations in `PXR_NAMESPACE::ty`
with redundant prefixes dropped:

| fix-bump | landed name |
| --- | --- |
| `HdEmbreeTryResolveIncidentMaterialNormal` | `ty::TryResolveNormalShdWldOut` |
| `HdEmbreeBumpDirectionIsValid` | `ty::BumpDirectionIsValid` |
| `HdEmbreeComputeSmoothTriangleShadowOffset` | `ty::ComputeSmoothTriangleShadowOffset` |
| `HdEmbreeComputeShadowTerminatorOffsetWeight` | `ty::ComputeShadowTerminatorOffsetWeight` |

Pick the final quantity roots against
[Common-quantity naming](../ARCHITECTURE.md#common-quantity-naming); the table
above is the shape, not the last word.

**12. Include style.**
`eea05c844` adds `#include "integrator/shadingNormal.h"` — quotes with a
directory prefix, in a file that no longer exists. Cross-directory first-party
includes use angle brackets and start with `renderer/`:
`#include <renderer/integrator/shadingNormal.h>`, placed in the cross-directory
group of each translation unit that needs it.

**13. Parameter naming inside the new header.**
`shadingNormal.h` uses `wi`, `wo`, `N`, `normal`, `value`, `p0..p2`, `n0..n2`,
`Ng`. Ported, these become `omegaInWld`, `omegaOutWld`, `normalShdWldOut`,
`normalSrfWldOut`, `normalGeomWldExt` and so on. `p0..p2`/`n0..n2` are
positional triangle-corner indices and may stay.

**14. API contracts on the new and widened declarations.**
Plan `21` requires input invariants, failure modes, and returned errors on
declarations. The four `shadingNormal.h` inlines, `ResolveShadingNormal`, and
the ported `ensure_valid_specular_reflection` all need them, and the three
widened `Bsdf::detail::` declarations in `closureTraversal.h` plus
`_ComputeDirectLightingMIS` in `renderer.h` need their existing contracts
extended for the added normal/offset parameters — including the exact frame
each one is in.

## Implementation

Order matters; each step leaves the tree buildable and each is a logical commit
boundary. Prepare them as such, but do not commit any of them until your human
has reviewed and approved — including step 1, which is why it uses `-n`.

1. `git cherry-pick -n 21352666c`. Build, then run the combined `testMaterialXCpp`
   executable; there is no separate `testMaterialXCppNodes` binary.
2. Amend the `ARCHITECTURE.md` naming-table sentence so `Out`/`In` select a
   transport side rather than promising a per-vector facing (fix 9). This lands
   first so every later step is describable in the tree's own vocabulary.
3. Add `renderer/integrator/shadingNormal.h` with the four helpers, named and
   documented per fixes 11, 13, and 14.
4. Add `smoothShadowOffsetExt` + `GetSmoothShadowOffsetOut()` to
   `_SurfaceInteraction` and populate it in `_TryBuildSurfaceInteraction`.
   Nothing reads it yet. Document the refined/displaced exclusion (fix 5).
5. Replace the three copies of the material-normal predicate
   (`pathIntegrator.cpp:360`, `surfaceShading.cpp:804`,
   `unlitIntegrator.cpp:127`) with the new resolver, and land fixes 7, 8, and
   10's prose in the same commit.
6. Capture `normalSrfWldOut` in `pathIntegrator.cpp` before the material normal
   is resolved, and thread it plus the step-4 offset into
   `_ComputeDirectLightingMIS`. Apply the offset at the shadow-ray origin only —
   not to `dirOffsetReferenceWld` (fix 3).
7. Port `ensure_valid_specular_reflection` into `bsdf/microfacet.cpp` (fix 2).
   This must precede step 8, or the tree spends a commit leaking light.
8. Replace the reflection-agreement tests in `lighting.cpp:220` and
   `pathIntegrator.cpp:616` with one unconditional bump-direction test (fix 1).
9. Thread the incident normal to every leaf in `bsdf/closureTraversal.cpp` and
   extend the `detail::` contracts (fix 14). Largest single step, no
   cherry-pick help.
10. Consolidate normal resolution (fix 4), in this order so the tree builds at
    each point: add `ResolveShadingNormal` to `bsdf/shadingFrame.h`; repoint the
    two diffuse leaves; repoint the reflective leaves; move
    `bsdf/legacySurface.cpp` and `EvalLayerBaseThroughput` off `interfaceN`;
    delete `ResolveReflectionNormal`, `ResolveDiffuseNormal`, and
    `FaceForwardNormal`; replace the leaf `interfaceN` parameter with an explicit
    `frontFacing` on the interface lobes that still need a side.
11. Record the graph-frame and per-lobe-fallback contracts in `ARCHITECTURE.md`,
    and rewrite the two `README.md:285` sentences (fixes 4 and 10).
12. Resolve fix 6 by construction or by measurement.

Steps 7–10 are the wide-image-diff group. Keep them as separate commits so a
suite regression is attributable to one of them rather than to the whole port.

## Testing

**Test the resolved normal, not the evaluated radiance.** An earlier draft of
this plan proposed driving conductor, GGX, and sheen leaves through the same
two-`omegaOutWld` equality comparison that
`TestTreeDiffuseNormalIsIndependentOfViewHemisphere` uses. That oracle is
invalid for every lobe except Lambertian: it only holds there because
`SampleLambertian` ignores `omegaOutWld` and `f = color/pi`. GGX VNDF sampling,
Fresnel, and sheen all depend on the view direction *by design*, so asserting
equal eval or PDF across two views would fail on correct code. The invariant
this plan actually introduces is that the **resolved shading normal** is
view-invariant, and that eval is continuous across the plane where the old
face-forward used to flip.

- Port `testenv/testHdEmbreeMaterialNormals.cpp` and its `pxr_build_test` /
  `pxr_register_test` pair. Re-derive the assertions against the final names;
  `_TestViewBackfacingNormalIsAccepted` is the one that pins fix 7's prose.
- Port `TestTreeDiffuseNormalIsIndependentOfViewHemisphere` unchanged. It is
  valid for the diffuse leaves and stays as the direct regression for
  `eea05c844`'s original fix.
- Add a resolver test covering every lobe type — Oren-Nayar, Burley, conductor,
  GGX dielectric, sheen: for a fixed `data.normal` and `normalShdWldOut`,
  `ResolveShadingNormal` returns the same vector regardless of `omegaOutWld`,
  including values on both sides of the normal's tangent plane. This is the
  lobe-agnostic claim, tested at the layer where it is actually true.
- Add a **continuity** test for a reflective leaf: sweep `omegaOutWld` across the
  plane where the old face-forward flipped and assert eval changes smoothly
  rather than discontinuously. Under the old code this test fails; that is the
  point.
- Add per-lobe validation tests for fix 4's resolver, covering the cases the
  face-forward used to mask: an inverted `data.normal` falls back to
  `normalShdWldOut` and is *not* negated; a degenerate and a non-finite value do
  the same; `_invalidMaterialNormalCount` increments. Include a back-face case —
  not to assert a flip, which the renderer should not do, but to pin the chosen
  fallback behavior.
- Add a leaf test that `hasShadingNormal` and non-`hasShadingNormal` paths agree
  when `data.normal == normalShdWldOut`. That is the invariant that makes one
  resolver correct for both, and it is cheap to assert.
- Add cases covering fixes 1 and 2, the pair most likely to be wrong: a
  normal-mapped transmissive surface, and a bump-mapped glossy surface at
  grazing incidence. Assert the transmitted sample survives `BumpDirectionIsValid`,
  that a direction the material and base normals disagree about does not, and
  that `ensure_valid_specular_reflection` never emits a direction below
  `normalGeomWldExt`.
- Build first, then run:
  - `pixi run build`
  - `pixi run build/pxr/imaging/plugin/hdEmbree/testMaterialXCpp` (one
    executable; the node, BSDF, material, graph, and adapter suites are all
    registered into it)
  - `pixi run ctest --test-dir build -R testHdEmbreeMaterialNormals --output-on-failure`
  - `pixi run ctest --test-dir build -R testHdEmbreeSampling --output-on-failure`
  - `pixi run ctest --test-dir build -R testHdEmbreeSubdivision --output-on-failure`
- Launch an adversarial test-review agent once the tests are written.

## Validation

The external suite is the primary evidence here, because every fix is visible
in a rendered image and none of them is bit-preserving. Expect and explain
image differences in:

- `materials/` normal-map and bump cases — faceting should disappear.
- Any tangent-driven material (anisotropy, tangent-space normal maps) — the
  `21352666c` change alters the tangent basis, so anisotropic highlights move.
- Coarse smooth geometry under a small light — terminator patches should
  disappear.
- **Any material with a `coat_normal`, and any rough conductor or dielectric
  under a normal map.** Fix 4 removes the view-dependent flip from the
  reflective lobes too, so these move even though `eea05c844` never touched
  them. This is the largest baseline movement in the plan and the easiest to
  mistake for a regression; that is why implementation steps 7–10 stay separate.
- **Open and thin-walled geometry with glossy normal-mapped materials.** This is
  where fix 2 either works or leaks. A closed mesh hides the failure.

Camera-dependence is the wrong rendered observable, and an earlier draft of this
plan asked for the wrong thing: it required a `coat_normal` case to produce
identical radiance from two cameras, which correct specular shading must not do.
The rendered check is **stability**, not equality — orbit the camera through the
grazing band on a bump-mapped and a `coat_normal` case and confirm the highlight
moves continuously instead of jumping sides. Under the current code it jumps;
that discontinuity is the defect, and no still-frame baseline captures it. The
equality claim belongs to the unit tests on the resolved normal, not here.

Performance: fix 6 makes this a performance-sensitive change. Compare the same
material-heavy workload before and after on the same machine with
`ty:randomNumberSeed` fixed, and record the result in `OPTIMIZATION.md`.

## Mandatory final suite gate

```sh
cd /path/to/typhoon-test-suite
powerprofilesctl launch --profile performance -- pixi run pytest --renderer typhoon-local
```

Run all tests to completion, report elapsed time, compare it with a relevant
baseline, and investigate regressions. Because this plan is not
behavior-preserving, every image difference must be individually explained and
the affected baselines updated deliberately. Do not commit until your human has
reviewed and explicitly approved the change.
