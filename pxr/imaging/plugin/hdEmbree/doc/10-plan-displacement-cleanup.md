# Plan: Simplify `displacementEvaluation.cpp`

Status: readability cleanup from the full-codebase review against
`pxr/imaging/plugin/hdEmbree/AGENTS.md`. Targets **Goal 2** (don't add lots of
tiny helper functions that force the reader to jump around) and **Goal 1**
(don't repeat yourself; don't keep an abstraction that doesn't pay for itself).

Scope is deliberately narrow: a numerical-conversion refactor plus removal of
indirection that carries no policy. It runs after
`04-plan-materialx-error-handling.md` and before
`23-plan-auto-types.md`, which owns the `auto`
sweep.

Ownership split with plan 04 (`04-plan:62-66` states removing the catches is a
behavior change and the helper cleanup is not):

- **Plan 04** establishes the non-throwing contract and removes the `catch (...)`
  wrappers.
- **Plan 10** collapses the resulting `HdEmbreeXxx()` → `_XxxImpl()` delegations,
  which are pure pass-throughs once the catches are gone (Issue 6).

10 stays a standalone commit. Folding it into 04 would mix a behavior-changing
exception refactor with behavior-preserving arithmetic cleanup in one diff,
which hurts both review and bisection.

## Issue 1: duplicated double-to-float narrowing

Five sites hand-write the same three-element `static_cast<float>` narrowing
followed by a finiteness check; three of them additionally re-declare
`constexpr double maxFloat` and check per-component range:

| Site | Range check? |
| --- | --- |
| `displacementEvaluation.cpp:114-118` (`_TryNormalize`, float overload) | no |
| `displacementEvaluation.cpp:154-158` (`_TryBuildNormalFromTangents`) | no |
| `displacementEvaluation.cpp:399-412` (`_ComputeObjectSpaceDisplacementOffsetImpl`) | yes |
| `displacementEvaluation.cpp:428-440` (`_TryAddOffset`) | yes |
| `displacementEvaluation.cpp:460-472` (`_TryFiniteDifference`) | yes |

The range checks are **required**, not defensive leftovers. Finite `float`
inputs produce out-of-`float`-range doubles at each of the three sites that have
them: matrix transformation of the world-space offset; summing two large finite
floats; and division by the finite-difference step
(`_GetFiniteDifferenceOffset` returns values on the order of `0.002`, so the
difference is scaled by ~500). Do not remove them.

**Fix:** add one helper that owns both the range check and the narrowing:

```c++
static bool _TryConvertToVec3f(GfVec3d const& value, GfVec3f* result);
```

Use it at all five sites. It subsumes the `maxFloat` block, the three-element
cast, *and* the trailing `_IsFinite`, so each site collapses to a single
`return _TryConvertToVec3f(...);` or one guarded call. It removes considerably
more than a `_FitsInFloat` predicate would, and names the operation actually
being performed.

Adding a range check to the two sites that lack one is safe and behavior-
preserving: both narrow a *normalized* vector, whose components are `<= 1`, so
the new check can never fire.

## Issue 2: drop the dead `length` out-param on `_TryNormalize`

`displacementEvaluation.cpp:73-100`, the `GfVec3d` overload, takes
`double* length = nullptr`. **No caller passes it** — both callers pass two
arguments — so the parameter and its `if (length)` branch (96-98) are dead.

**Fix:** delete the parameter and the branch. Nothing else.

**Keep both overloads.** The `GfVec3d` overload has two callers, not one: the
`GfVec3f` overload at line 110, *and* `_TryBuildNormalFromTangents` at line 151,
which normalizes a genuine double-precision cross product built at 147-149.
Collapsing into a single float function would either narrow that cross product
before normalizing — a precision change in the surface normal, so not behavior-
preserving — or duplicate the double normalization algorithm.

Do **not** template the overloads either, for the same reason: the `float` path
deliberately performs its arithmetic in `double`. Displacement also keeps its own
normalization helper on purpose — do not merge with `_TryNormalizeDirection` /
`_SafeNormalized` in renderer/medium code, which have different semantics.

**Leave the two `_IsFinite` overloads (`displacementEvaluation.cpp:57-71`) as
they are.** They are five lines total and overload resolution reads more clearly
here than a template would. Decided, not discretionary.

## Issue 3: inline the normal-derivative projection

`_TryProjectNormalDerivative` (`displacementEvaluation.cpp:569-585`) is called
twice, both from `_ComputeDisplacedSubdivNormalDerivativesImpl` (`750-751`).

**Fix:** replace the two calls with one linear block, not two inlined copies of
the helper body:

```c++
dNdu -= frame.normal * GfDot(frame.normal, dNdu);
dNdv -= frame.normal * GfDot(frame.normal, dNdv);
if (!_IsFinite(dNdu) || !_IsFinite(dNdv)) {
    return false;
}
```

Dropping the helper's *input* validation is safe here: `frame.normal` is already
checked finite at line 684, and `dNdu`/`dNdv` come straight from
`_TryFiniteDifference`, which guarantees a finite result. Duplicating those
guards would defeat the point of the change.

**Do not inline** `_AreTangentsIndependent` (3 sites: `144, 177, 200`),
`_TryAddOffset` (3 sites: `548, 549, 798`), or `_TryFiniteDifference` (4 sites:
`544, 546, 748, 749`). Each owns finite-value, degeneracy, and — after Issue 1 —
narrowing policy that would be duplicated 3-4× by inlining. They name real
numerical operations; they are not bare indirection.

Final helper set after this plan: `_GetFiniteDifferenceOffset`, `_IsFinite` (×2),
`_TryNormalize` (×2), `_TryConvertToVec3f`, `_AreTangentsIndependent`,
`_TryBuildNormalFromTangents`, `_TryBuildWorldFrame`, `_InterpolateBaseFrame`,
`_TryAddOffset`, `_TryFiniteDifference`, `_EvaluateDisplacedSubdivProbe`,
`_TryBuildDisplacedNormal`.

## Issue 4: use `orientationSign` directly

`context->orientationSign < 0.0f ? -1.0f : 1.0f` appears at
`displacementEvaluation.cpp:601, 692, 774` — in three *separate* functions
(`_ComputeDisplacedSubdivFrameImpl`, `_ComputeDisplacedSubdivNormalDerivativesImpl`,
`_ComputeDisplacedSubdivPositionImpl`), so it cannot be hoisted into a shared
local, and a `_OrientationSign(context)` helper would add exactly the
indirection this plan is removing.

`HdEmbreePrototypeContext::orientationSign` is initialized to `1.0f`
(`renderer/geometry/context.h:70`) and assigned exactly `-1.0f` or `+1.0f`
(`delegate/mesh.cpp:1825-1828`); nothing else writes it. Every other consumer —
`delegate/mesh.cpp:490,889`, `renderer/rendererImpl.h:692,738`,
`renderer/integrator/surfaceShading.cpp:365`,
`renderer/integrator/sss.cpp:317` — already multiplies by it raw.
`displacementEvaluation.cpp` is the only file that re-normalizes it.

**Fix:** document the ±1 invariant on the `context.h:70` declaration and pass
`context->orientationSign` directly at all three sites. This is behavior-
preserving given the invariant and makes the file consistent with the rest of
the codebase.

The `auto const* faceVertexCounts` at `displacementEvaluation.cpp:614` is owned
by `23-plan-auto-types.md:99`. Leave it alone here.

## Issue 5: unused frame outputs

`_TryBuildDisplacedNormal` (`displacementEvaluation.cpp:522-567`) always
requires `outDPdu`/`outDPdv`, so
`_ComputeDisplacedSubdivNormalDerivativesImpl` declares `unusedDPdu`/
`unusedDPdv` (`720-721`) purely to satisfy it at `726` and `735`.

**Fix:** make the derivative out-params optional (`nullptr`-permitting) and pass
`nullptr` at `726` and `735`. Removes four lines of noise from the main
algorithm without duplicating any policy.

**Keep the name `_TryBuildDisplacedNormal`.** Do not rename to
`_TryBuildDisplacedFrame`: `HdEmbreeDisplacedSubdivFrame`
(`renderer/geometry/displacementEvaluation.h:27`) is a distinct public struct
holding the full cached frame, and "frame" in this file already means that. If a
rename is wanted, `_TryBuildDisplacedNormalAndTangents` is accurate; the plain
existing name is fine.

## Issue 6: collapse the post-plan-04 delegation wrappers

Once plan 04 has removed the `catch (...)` blocks, the five public entry points
(`displacementEvaluation.cpp:808-918`) are pure one-line delegations to their
`_XxxImpl` counterparts, and the split has no remaining purpose.

**Fix:** fold each `_XxxImpl` body into its public function and delete the
`Impl` layer. Purely mechanical; the public signatures in
`displacementEvaluation.h` do not change.

Note the real call graph, since it rules out any "hoist to a single entry point"
variant: there is no single top-level caller. `delegate/mesh.cpp:501,519` is the
Embree displacement callback, `delegate/mesh.cpp:601` is adaptive tessellation,
and `renderer/rendererImpl.h:700,903,1061` is the hit-time path — three
independent entries.

## Sequencing and validation

Land as **one commit**, after plan 04 and before plan 23. The six issues are
small and touch overlapping lines; splitting them costs more in rebase churn
than it buys in reviewability. If a split is wanted, the only clean seam is
Issue 1 (the conversion helper) separately from the rest.

Validation:

- Build then run the unit tests — CTest does not rebuild:
  - `pixi run cmake --build build --target testHdEmbreeSubdivision`
  - `pixi run ctest --test-dir build -R testHdEmbreeSubdivision --output-on-failure`
- Existing coverage in `testenv/testHdEmbreeSubdivision.cpp` exercises the
  **successful** numerical paths this plan touches, and is the primary
  regression signal:
  - `TestRealCallbackAddsRemovesAndReplacesDisplacement` (1041) checks displaced
    frame values, curvature (`dNdu`/`dNdv` against analytic expectations,
    including `dot(N, dNdu) ≈ 0`), boundary finite-difference step direction
    (`du < 0`, `dv > 0`), evaluation call counts, and that outputs are left
    untouched when evaluation fails — see 1296-1315.
  - `TestLeftHandedSubdivisionDisplacesAlongAuthoredNormal` (1635) pins
    `orientationSign == -1.0f` (1694), which is the direct guard on Issue 4.
  - `TestRprimScalePreservesWorldUnitDisplacement` (1706) exercises
    `HdEmbreeComputeObjectSpaceDisplacementOffset` (1766), the Issue 1 site with
    the matrix transform.
- **Gap:** nothing currently covers the out-of-`float`-range *rejection*
  branches, which are exactly what `_TryConvertToVec3f` centralizes — a grep for
  `maxFloat` / `numeric_limits<float>::max` in `testenv/` returns nothing. This
  is a pre-existing gap, not one this plan introduces; do not expose the private
  helpers just to test a mechanical refactor. If coverage is wanted, the natural
  place is a `_TryFiniteDifference`-shaped case driven through
  `HdEmbreeComputeDisplacedSubdivNormalDerivatives` with a probe position near
  `FLT_MAX`, added as a follow-up rather than a prerequisite.
- AOUSD displacement fixture, bit-identical before/after. "Before" means the
  post-plan-04, pre-plan-10 commit — *not* the pre-plan-04 state, whose error
  handling differs by design.
  1. At the post-04 commit: `pixi run cmake --build build --target install`, then
     render the fixture with this checkout's installed `usdrender` at a fixed
     seed to `before.exr`. Locate the fixture per AGENTS.md "Focused Tests":
     `cd /home/anders/code/aousd-materials-test-suite && pixi run pytest
     test-suite/surfaces/open_pbr_surface/displacement.usda --collect-only -q`
     — that command only resolves the stage, it does not render.
  2. Apply plan 10, **rebuild and reinstall** (the fixture loads the installed
     plugin, so an un-reinstalled build silently re-renders the old code), and
     re-render identically to `after.exr`.
  3. `oiiotool --diff before.exr after.exr` must report zero differing pixels.

  Fill in the exact `usdrender` invocation and output paths on first run and
  record them here, so the comparison is reproducible.

There is no checked-in delta reflection/refraction displacement fixture, so the
hit-time normal-derivative path (`renderer/rendererImpl.h:1061`) is covered by
the `testHdEmbreeSubdivision` curvature assertions above rather than by a render
comparison. If one is authored later, add it here.

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
