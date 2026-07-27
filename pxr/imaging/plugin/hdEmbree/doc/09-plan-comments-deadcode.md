# Plan: Fix comments and remove dead code

Status: readability cleanup from the full-codebase review against
`pxr/imaging/plugin/hdEmbree/AGENTS.md`. Targets **Goal 4** (comment WHAT and
WHY, not HOW; comments must be correct) and **Goal 1** (a reference renderer
should carry no dead code).

These are behavior-preserving edits that are independent of each other and land
together as one commit, after `05-plan-naming-core.md` (see *Sequencing*). Three
issues edit code tokens — a function-local rename (Issue 1), an uncalled
declaration deleted (Issue 2), an unused SSS field and its sole write deleted
(Issue 2c), and a redundant `||` term removed (Issue 6) — but all four are
behavior-preserving. The rest are comments and whitespace.

## Issue 1: the ambient-occlusion accumulator is documented backwards

`renderer/integrator/unlitIntegrator.cpp`. `_ComputeAmbientOcclusion` returns
**visibility**, not occlusion: the caller at `:137-144` multiplies it in as
`aoLightIntensity`, and the declaration doc at `renderer/renderer.h:585-590`
already states "ambient visibility" / "Unoccluded fraction in [0,1]". The
implementation says the opposite in its accumulator name and three comments, so
the header and the body actively contradict each other. A fourth comment is
wrong for an unrelated reason:

- `:167`, `:248`, `:251`, `:253` — the accumulator is named `occlusionFactor`
  but is incremented on the *visible* branch.
- `:220-222` — "The occlusion factor is the fraction of the hemisphere that's
  occluded". Inverted.
- `:241` — "Record this AO ray's contribution to the occlusion factor."
  Inverted.
- `:250` — "Compute the average of the occlusion samples." Inverted.
- `:246` — "shadow is occluded when `shadow.ray.tfar < 0.0f`". The *sign* here
  is right (`rtcOccluded1` does drive `tfar` negative on occlusion). The defects
  are that `shadow` is an `RTCRay` and has no `.ray` member, and that the
  comment states the occlusion condition directly above a branch that tests
  visibility (`:247` checks `shadow.tfar > 0.0f`), so it reads as contradicting
  the code.

**Fix:**

- Rename the local `occlusionFactor` → `visibility` (4 sites: `:167`, `:248`,
  `:251`, `:253`). The function name `_ComputeAmbientOcclusion` stays — AO is
  the conventional name for the effect, and `05-plan-naming-core.md` owns any
  identifier rename beyond this local.
- Restate `:220-222`, `:241`, and `:250` in terms of ambient visibility (the
  fraction of the hemisphere that is *unoccluded*), matching the header.
- Correct `:246` rather than deleting it. That `rtcOccluded1` signals occlusion
  by setting `tfar` to `-inf` is a non-obvious Embree API contract, and without
  it `tfar > 0` does not read as "visible". Fix the member name and phrase it
  against the branch actually taken: "`rtcOccluded1` sets `shadow.tfar` to
  `-inf` on occlusion, so `tfar > 0` means the light is visible." This is a WHY
  (why *this* is the visibility test), not HOW narration.

Leave the estimator explanation at `:242-245` alone; it already states WHY the
cosine-weighted average is the right estimator.

## Issue 2: dead code — `_RayShouldContinue`

`renderer/renderer.h:938-944`. Declared and documented as "reserved... currently
has no definition or call sites and must not be called until implemented." A
declared-but-unimplemented member with a paragraph of documentation is pure
noise in a reference renderer.

**Fix:** delete the declaration and its doc block. Reintroduce it when it is
actually implemented and called.

## Issue 2b: dead file — `renderer/pxrPbrt/pbrtUtils.h`

Nothing in the tree includes it. The only references are its `CMakeLists.txt`
header-list entry and a mention in `ARCHITECTURE.md`; no `.cpp` or `.h` includes
it, and its four symbols — `pxr_pbrt::pi<T>`, `SphericalDirection`,
`SampleUniformCone`, `InvUniformConePDF` — have no call sites.

Verify before acting, since a plan landing between this one and the check could
add a consumer:

```sh
rg -l 'pbrtUtils\.h' . -g '!build/**' -g '!.pixi/**'
rg -n 'SphericalDirection|SampleUniformCone|InvUniformConePDF' \
    renderer delegate testenv
```

**Fix:** if both come back empty, delete `renderer/pxrPbrt/` entirely, drop its
`CMakeLists.txt` entry, and remove the `ARCHITECTURE.md` mention. If something
does consume it by then, leave it and let `19-plan-ty-namespace.md` move it to
`ty::pbrt` as planned.

Coordinate with `19`, which otherwise renames this namespace, and with `23`,
whose `_pi<T>` item counts `pxr_pbrt::pi<T>` as one of the definitions to
collapse — deleting the file removes that item rather than completing it.

## Issue 2c: dead field — `HdEmbreeSssInput::iorInterior`

`renderer/integrator/sss.h:34` declares `HdEmbreeSssInput::iorInterior`.
`renderer/integrator/sss.cpp` assigns it while preparing `walkInput`, but no
code reads it. The SSS walk obtains every IOR value it uses through other
state, so the field and assignment add a false input contract without affecting
transport.

Verify before acting, since an intervening change could add a consumer:

```sh
rg -n '\biorInterior\b' renderer -g '*.{cpp,h}'
```

**Fix:** if the only matches remain the declaration and assignment, delete
both. Do not redirect the value into another IOR field; this issue removes
dead state and does not change the SSS IOR policy.

## Issue 3: procedural step numbers in the derivative helper

Goal 4 asks for WHAT and WHY, not step-by-step narration. Several of these
comments *already* carry good WHY text and must not be rewritten — the churn
risk here is real, so the edit is specified per location:

- `renderer/rendererImpl.h:891` `// 1. Position derivatives. Displaced
  subdivision needs the derivatives of P + D*N; rtcInterpolate itself
  intentionally returns only the undisplaced limit surface.` — **remove the
  `1. ` prefix only.** The rest is exactly the WHY we want; do not touch it.
- `renderer/rendererImpl.h:957` `// 3. Normal derivatives` — **remove the
  `3. ` prefix.** The block comment immediately below it already explains why
  displaced geometry zeroes them; leave that alone. The `//    Support both
  vertex and face-varying interpolation modes.` continuation restates the code
  below — delete it.
- `renderer/rendererImpl.h:929` `// 2. If st available, transform from
  parametric to st space. / Handle both GfVec2f and GfVec3f st buffers. /
  Support both vertex and face-varying interpolation modes.` — **remove the
  `2. ` prefix and the two continuation lines** (both narrate HOW and restate
  the code); keep the first sentence.
- `renderer/rendererImpl.h:800` `// 3. Normal derivatives (dndu, dndv)` — this
  one is a bare label with no intent. **Replace with a WHY**: "Normal
  derivatives let specular continuations propagate ray-direction
  differentials." That is their only consumer — `_PropagateRayDifferential`
  (`renderer/integrator/surfaceShading.cpp:249`) reads `dndu`/`dndv` at `:282`
  and `:285`, past the `if (!specular)` guard at `:262` that clears
  `hasDifferentials` and returns. Do not write "propagate the surface/ray
  differential footprint": it is vague and conflates the surface derivatives
  with the ray differentials they feed.
- `renderer/renderer.cpp:827` `// (Above is equivalent to: tileX = tile %
  numTilesX)` — restates the code immediately above it. **Delete.**

## Issue 4: `_ComputeAmbientOcclusion` has an undocumented normal parameter

`renderer/renderer.h:585-594`. The doxygen documents `position`, `normal`,
`domain`, and the return value, but the signature takes **two** normals — the
second (`:593`) is entirely undocumented, and two normal parameters with no
stated distinction is a Goal-4 (and Goal-3) gap.

**This plan lands after `05-plan-naming-core.md`**, which renames that parameter
`Ng` → `normalGeomWldExt` (`05-plan-naming-core.md:195`) and renames `normal` in the
surrounding surface code. Write the doc against the **post-05 names**; the
line numbers above will also have shifted by then, so re-locate by symbol.

**Fix:** add a `\param` line for the geometric normal stating its role (ray-origin
offset along the true surface, so self-intersection is avoided independently of
the shading normal used to orient the sampling hemisphere) and why it is
distinct from the shading normal. If 05 has not landed when this commit is
prepared, rebase rather than writing `\param Ng`.

## Issue 5: `_UpdateTangentFrameCache` — contract and algorithm

`delegate/mesh.h:222-225` and `delegate/mesh.cpp:1386`.

The declaration comment states *what* the function produces ("smooth tangent
frame as face-varying data... orthonormalized against the effective shading
normal") but not the two things a caller actually needs: the **smoothing
semantics** and the **failure state**. The 170-line body then has no leading
explanation at all, so a reader must reverse-engineer the algorithm.

Note the smoothing key is not a loose "vertex+normal hash". `_SmoothingKey`
(`mesh.cpp:283-330`) compares the **authored vertex index** and the **exact
float bit patterns of the normalized normal**; the hash is only the map
plumbing. The consequence is the point: corners that share a vertex but carry
split normals do *not* smooth together, so tangent discontinuities are
preserved across normal splits.

**Fix — two comments, no code change:**

1. **`mesh.h` declaration:** extend the existing comment with the contract —
   corners are averaged only within a (vertex index, exact normal) group, which
   preserves tangent discontinuities across normal splits; and the exact
   invalidation condition: refined geometry, missing prototype context, invalid
   or mis-sized derivative cache, empty topology, or an inconsistent
   triangulated-corner mapping clear the outputs and leave
   `_tangentFrameValid == false` (`mesh.cpp:1389-1406`).

   State that condition precisely and stop there. Do **not** write a broader
   "never produces a partial frame" claim: it is false. Triangles with
   out-of-range point indices (`:1427`) and corners with out-of-range authored
   indices (`:1447`) are individually skipped, leaving their default-initialized
   output entries in place, and `_tangentFrameValid` is still set to `true` at
   `:1555`.
2. **`mesh.cpp` leading block:** the algorithm — per-corner tangent from `dPdu`
   projected off the normal, accumulated per `_SmoothingKey` group, then
   re-orthonormalized. Do not restate the declaration contract here.

Deliberately do **not** extract a shared orthonormalize helper: the per-corner
construction (from `dPdu`/`dPdv`, preserving handedness) and the post-smoothing
construction (projecting accumulated vectors, different fallbacks) use genuinely
different policies that a shared helper would obscure.

This item stays in 09 rather than moving to `13-plan-mesh-ownership.md`: the
overlap table in `README.md` makes 09 the authoritative owner, and the
`mesh.cpp` hot-file order is **09 → 13**.

## Issue 6: duplicate `distantLight` token in the light-type chains

`delegate/renderDelegate.cpp:530 & 532` (in `CreateSprim`) and `:557 & 559` (in
`CreateFallbackSprim`): `HdPrimTypeTokens->distantLight` appears twice in the
same `||` disjunction. Harmless but misleads a reader into hunting for a
distinction, and signals copy-paste (Goal 1, redundant code).

**Fix:** delete the duplicate term in both chains. This is a pure no-op: the
disjunction accepts the same token set before and after.

**Out of scope — separate investigation, not part of this plan.** These chains
also accept `HdPrimTypeTokens->light` (`:529`, `:556`), which is absent from
`SUPPORTED_SPRIM_TYPES` (`:60-71`). Whether `->light` should be removed or
documented is *not* behavior-preserving — removing it may be a compatibility
change for clients that create a generic `light` Sprim — so it is not an edit
this plan makes. Resolve it separately by first determining why the
generic-light path is accepted, then testing that path directly.

## Issue 7: typo and trailing whitespace

- `delegate/renderDelegate.h:101` — "error ahndling" typo in the constructor
  doc.
- `delegate/renderParam.h:27` — trailing whitespace on a `///` line.

**Fix:** correct the typo and strip the whitespace as part of this commit. There
is no global whitespace pass to defer to, and `delegate/renderDelegate.cpp` has
no trailing whitespace at all, so this is two edits.

## Sequencing and validation

Land as **one commit**, after `05-plan-naming-core.md` merges (Issue 4 depends on its
parameter names). The items are independent of each other; there is no reason to
split Issue 2 out.

Hot-file coordination:

- `renderer.h` / `rendererImpl.h`: these in-place edits must precede
  `14-plan-renderer-impl-header.md`, which relocates the code.
- `mesh.cpp`: precede `13-plan-mesh-ownership.md`, which rewrites the file.

**Validation: build only** —
`pixi run cmake --build build --target hdEmbree`. The compiler proves everything
that can be proved here: that `_RayShouldContinue` is unreferenced, and that the
Issue 1 rename hit every site. Image diffs and `testHdEmbreeLightSamplers`
cannot distinguish these edits — in particular that test exercises runtime light
sampling, not `CreateSprim`/`CreateFallbackSprim`, so do not cite it as
validating Issue 6. Real render-delegate factory coverage would be its own
testing change, not a justification attached to a no-op edit.

No README / ARCHITECTURE / AGENTS update is needed: no behavior, architecture,
extension point, or workflow changes.

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
