# Plan: Extract the instance-update block out of `_PopulateRtMesh()`

Status: source-layout cleanup, scoped to one function extraction in
`delegate/mesh.cpp`. This plan owns the `_PopulateRtMesh()` function
extraction that `13-plan-mesh-ownership.md` explicitly declines (see its
Non-goals, `13-plan-mesh-ownership.md:51-55`) so that plan 13 stays a
bisectable lifetime-only commit.

Implementation measurement: after plans 13 and 15, the guarded instance
update body was 69 physical lines, 61 nonblank, and required only the planned
`sceneDelegate`, `scene`, and `device` parameters. Both counts exceed the
40-line abort threshold, so the extraction proceeded.

The file-splitting work for `bsdf.cpp` and `lightSamplers.cpp` that earlier
drafts of this plan listed as "candidates" is now designed and measured in
`17-plan-bsdf-split.md` and `18-plan-light-samplers-split.md`. This plan
does not decide anything about those files, does not create or delete any
file, and does not change `CMakeLists.txt`.

## Issue

`HdEmbreeMesh::_PopulateRtMesh()` is `delegate/mesh.cpp:1638-2258` — 621
lines in four numbered sections:

| Section | Lines | Responsibility |
| --- | --- | --- |
| 1. Pull scene data | 1651-1720 | dirty-bit-gated `HdSceneDelegate` pulls |
| 2. Resolve drawstyles | 1721-1757 | `doRefine`, `_smoothNormals`, `authoredNormals` |
| 3. Populate Embree prototype | 1758-2137 | geometry create/recreate, subdiv topologies, normals, primvar samplers, material bind, points, visibility, prototype commit |
| 4. Populate Embree instance objects | 2139-2237 | instancer sync, per-instance geometry create/destroy, transforms, categories |
| (tail) | 2239-2257 | repr state publish, dirty-bit clear |

Sections 1-3 are the ordered prototype build. A reader tracing "what does
Embree see for this mesh" has to read them in order, and splitting them would
be exactly the tiny-helper fan-out Goal 2 forbids.

Section 4 is different. It is a self-contained top-level instance
materialization: it reads the instancer, sizes a vector of Embree instance
geometries up or down, and writes transforms and light-linking categories. It
does not participate in the prototype build ordering, and nothing after it
reads any local it defines. It sits at the end of an already-long function
purely because instances must be built after the prototype scene exists.

## Goals

- Give the Embree instance lifetime operations (attach, detach, release, user
  data) one named home next to the `_Instance` record that
  `13-plan-mesh-ownership.md` introduces.
- Remove section 4's locals from `_PopulateRtMesh()`'s live state.
- Leave the prototype build (sections 1-3) as one visible ordered sequence.

## Non-goals

- Do not split `mesh.cpp` into more than one translation unit. The extracted
  function stays a private `HdEmbreeMesh` method defined in `mesh.cpp`, with
  one private declaration added to `mesh.h`; no new file or `CMakeLists.txt`
  edit.
- Do not extract sections 1-3, in whole or in part. See "Rejected candidate".
- Do not extract scene-data pulls one dirty bit at a time.
- Do not change instancing, flattening, or threading semantics. This plan is
  a pure code move; `13-plan-mesh-ownership.md` already owns the lifetime
  correctness of the lines being moved.

## Proposed design

Extract `mesh.cpp:2166-2231` — the body of `if (instancesDirty) { ... }` —
into one private method:

```
// Rebuild this rprim's top-level Embree instances from the current instancer
// state. Sizes _instances to the instancer's instance count (one identity
// instance when the rprim is un-instanced), creating and releasing Embree
// instance geometries and their HdEmbreeInstanceContexts as the count changes,
// then writes each instance's object-to-world/world-to-object transform and
// its resolved light-linking category set.
//
// Requires: _rtcMeshScene exists; _transform and _categories are current; the
// caller has already run _UpdateInstancer() and
// HdInstancer::_SyncInstancerAndParents(). _rtcMeshScene must be committed
// before the caller commits these instances.
// Commits neither the instance geometries nor the root scene; the caller must
// run _CommitPrototypeInstances() and then commit the root scene.
// An empty instancer id yields exactly one identity instance; an existing
// instancer returning no instance data yields none. Embree errors use the
// configured device callback; this method has no local recovery or failure
// result.
void _UpdateInstances(HdSceneDelegate* sceneDelegate,
                      RTCScene scene,
                      RTCDevice device);
```

The call site becomes:

```
if (instancesDirty) {
    _UpdateInstances(sceneDelegate, scene, device);
}
```

What stays in `_PopulateRtMesh()`: `_UpdateInstancer()`, the
`_SyncInstancerAndParents()` call, the `DirtyCategories` pull into
`_categories`, and the `instancesDirty` computation. Those are dirty-bit
reads, and `instancesDirty` is also consumed by the
`prototypeDirty || instancesDirty` commit decision at `mesh.cpp:2235`. Moving
the dirty test inside the callee would force the function to return a `bool`
whose only purpose is to answer a question the call site can already see.

### Effect on `_PopulateRtMesh()` live state

Removed from the driver's live set: the `instances` vector, `oldSize`,
`newSize`, and three loop induction variables. `dirtyBits` is not needed by
the callee. Nothing the callee produces is read later in the driver.

### Ordering against plan 13

Plan 13 replaces `_rtcInstanceIds` and `_rtcInstanceGeometries` with one
`_Instance` record vector and moves the detach/release into that record's
teardown. Land plan 13 first: the block being extracted shrinks from ~66
lines to roughly 50, and the extracted method then contains no raw
`rtcReleaseGeometry`/`delete` pair at all. Extracting first would mean
rewriting the same lines twice and would put a lifetime change inside a move
commit, which is what plan 13's non-goal is protecting against.

### Abort condition

Re-measure after plan 13 lands. Abandon the extraction and close this plan as
"declined, measured" if either is true:

- the block is under 40 lines after plan 13's record change;
- it needs more than the three parameters above (i.e. plan 13 leaves state
  that must be threaded through, rather than reachable as a member).

Record the measurement in the plan either way. A declined outcome is a
complete outcome for this plan and blocks nothing downstream.

## Rejected candidate: the shading-primvar / smooth-normal / tangent block

`mesh.cpp:1912-2038` — smooth-normal adjacency and computation, the
`_primvarSourceMap` sampler loop, the surface-derivative and tangent-frame
cache refreshes, and the uniform primvar map build — was the other candidate.
Reject it:

- **It is not one responsibility.** It is five dirty-bit-gated refreshes that
  happen to be adjacent. `_UpdateSurfaceDerivativeCache()` and
  `_UpdateTangentFrameCache()` are already separate methods, so what remains
  to extract is mostly the gating around them. Extracting gating is the
  "reader has to bounce back to understand order" failure.
- **It removes almost no live state.** Only `authoredNormals`
  (`mesh.cpp:1751`) dies at the call site; `newMesh`, `_refined`,
  `_smoothNormals`, `dirtyBits`, and `id` are all still needed by section 3
  afterwards, so the signature would be four parameters for ~110 lines.
- **`15-plan-primvar-binding-cache.md` deletes a third of it.** The
  `primvarMapByString.erase()` / `delete` pairs at `:1942-1945`, `:1969-1972`,
  and `:2011-2016` collapse when `primvarMap` becomes owning, and the entire
  uniform primvar map build at `:2021-2038` disappears with
  `uniformPrimvarMap`. The block is ~90 lines after 15, and the part that
  made it look tangled is the part that goes away.

If the block still reads badly after 15, the right fix is comments and
ordering inside `_PopulateRtMesh()`, not a fourth private method.

## Dependencies

Authoritative set: **13, then 15**. Both edit the exact lines this plan
moves; nothing else does.

- `13-plan-mesh-ownership.md` — must land first. It rewrites the instance
  vectors this plan extracts, and owns every lifetime and failure
  postcondition in the moved lines.
- `15-plan-primvar-binding-cache.md` — must land first, for the rejected
  candidate's re-measurement only. If 15 slips, this plan can still land its
  extraction; only the recorded justification for the rejection is provisional
  until 15 lands.

Plans 02, 12, and 14 were listed as dependencies in earlier drafts. They edit
`renderer.h`/`renderer.cpp`/`rendererImpl.h`, which this plan does not touch.
They are not dependencies.

## Validation

- **Bit-identical fixed-seed renders.** This is a same-translation-unit
  private-method extraction, so `-O3` inlining decisions and floating-point
  contraction are unchanged and bit-identical output is a real requirement,
  not an aspiration. Render with
  `-s "{settings}.ty:randomNumberSeed = 1"` and compare with
  `oiiotool --diff`, expecting zero differing pixels. Any difference means
  the move was not a move.
- Cover both instancing paths: a point-instanced stage and an un-instanced
  stage, so the identity-instance branch at `:2180-2184` is exercised.
- Cover an instance-count decrease, not just an increase, so the size-down
  path runs.
- Cover a light-linked instanced stage so category propagation is checked.
- `pixi run ctest --test-dir build --output-on-failure` for the focused
  targets that build geometry: `testHdEmbreeSubdivision`,
  `testHdEmbreeWireframe`.
- No new tests. The extraction is behavior-preserving and the existing
  instancing coverage plus the image diff is the check. (If that judgement
  changes and tests are added, run the adversarial test-review agent per the
  project's test policy.)

## Documentation

- `AGENTS.md` "Directory Map" — the `delegate/mesh.*` entry describes
  `_PopulateRtMesh()`'s responsibilities; add the extracted method.
- `AGENTS.md` "Geometry And Primvars" — the paragraph describing what
  `_PopulateRtMesh()` updates.
- `ARCHITECTURE.md` — wherever mesh population is described.
- `README.md` — no change expected; this plan exposes no user-visible
  behavior. Confirm rather than assume.
- No `CMakeLists.txt` change.

## Risks and decisions

- The extraction is small. Its value is ownership clarity next to plan 13's
  `_Instance` record, not line count. The abort condition above is the guard
  against doing it for its own sake.
- A private method that takes `RTCScene` and `RTCDevice` as parameters when
  both are derivable from the render param is slightly redundant. Keep the
  parameters: `_PopulateRtMesh()` already has them in hand, and re-deriving
  them inside would hide the fact that the top-level scene is the one being
  mutated.

## Completion criteria

- `_PopulateRtMesh()` contains sections 1-3 and the tail as one ordered
  sequence, plus a single guarded `_UpdateInstances()` call — or a recorded
  measurement declining the extraction.
- No new file and no `CMakeLists.txt` change; `mesh.h` gains only the private
  `_UpdateInstances()` declaration and its contract.
- The declaration contract above is present on `_UpdateInstances()`.
- Fixed-seed renders are bit-identical.
- `AGENTS.md` and `ARCHITECTURE.md` mesh sections match the final shape.

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
