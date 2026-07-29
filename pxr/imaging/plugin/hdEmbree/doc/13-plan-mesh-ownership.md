# Plan: Make mesh prototype and instance ownership explicit with RAII

Status: lifetime-safety cleanup. Ownership only — no source reorganization.

## Issue

`HdEmbreeMesh` manually owns several resources whose pointers are also attached
to Embree objects:

- prototype context allocated with `new` and recovered through geometry user
  data (`mesh.cpp:2261`);
- one instance context per top-level instance, likewise recovered through
  geometry user data (`mesh.cpp:2268`);
- Embree geometry/scene handles requiring ordered detach/release.

Instance state is already split across two index-coupled vectors,
`_rtcInstanceIds` (`mesh.h:256`) and `_rtcInstanceGeometries` (`mesh.h:343`),
with the context reachable only by round-tripping through
`rtcGetGeometryUserData()`. Destruction logic is repeated in `Finalize()`,
prototype rebuild, and instance-vector shrink paths. Comments such as “delete
context first” communicate ownership procedurally, but no type expresses it.
Early returns during rebuild make it difficult to prove which resources remain
live — and one of them currently leaks (see **Failure postconditions**).

Primvar samplers are also manually owned — raw pointers in the prototype
context's `primvarMap`, mirrored into `primvarMapByString` in lockstep at five
sites, and `delete`d by hand at four. That ownership is **deliberately
excluded** here; see the non-goals and `15-plan-primvar-binding-cache.md`.

Embree needs borrowed stable addresses; it does not require Embree itself to own
the pointed-to C++ objects.

## Goals

- Express C++ ownership of the prototype and instance contexts in
  `HdEmbreeMesh`.
- Reduce, not increase, the number of index-coupled instance containers.
- Keep every pointer passed to Embree stable for the geometry lifetime.
- Remove repeated manual context delete loops.
- Preserve the required stop-render, detach, commit, and release order.
- Give every early return in `_PopulateRtMesh()` a stated, verified
  postcondition.
- Centralize primvar-sampler teardown into one operation so that making the
  prototype context RAII does not silently leak the samplers it holds.

## Non-goals

- Do not wrap every Embree handle in a generic smart-handle framework.
- Do not change prototype/instance flattening or threading semantics.
- Do not introduce shared ownership where one mesh is the clear owner.
- **Do not extract functions out of `_PopulateRtMesh()`.** That work belongs
  entirely to `16-plan-source-organization.md`, which re-measures the file after
  the ownership boilerplate is gone and decides whether the extraction is worth
  it. Bundling it here would make this commit non-bisectable against a
  lifetime regression.
- **Do not change primvar sampler ownership, and do not touch
  `primvarMapByString`.** `15-plan-primvar-binding-cache.md` replaces the
  per-hit name lookup with compile-time-resolved handles. That deletes
  `primvarMapByString` outright, adds a handle-indexed observing vector in its
  place, and only then converts `primvarMap` to owning `unique_ptr`. Doing the
  `unique_ptr` conversion here would mean editing the same read sites twice.

## Proposed design

### One instance record, not three parallel vectors

Replace `_rtcInstanceIds` and `_rtcInstanceGeometries` with a single vector of a
plain record:

```
struct _Instance {
    unsigned rtcId = RTC_INVALID_GEOMETRY_ID;
    RTCGeometry geometry = nullptr;
    std::unique_ptr<HdEmbreeInstanceContext> context;
};
std::vector<_Instance> _instances;
```

Attach `context.get()` as the instance geometry's user data. Nothing requires
the ids or geometries to be contiguous — every use site is already a loop
(`mesh.cpp:580, 673, 732, 2186-2229`) — so a record costs nothing and makes one
container the complete truth about an instance. Adding a third
`std::unique_ptr` vector alongside the existing two would make the coupling
invariant *worse* than it is today, which is the opposite of this plan's goal.

This also deletes `_GetInstanceContext()` (`mesh.cpp:2268`) outright, including
its unused `RTCScene` parameter.

### Prototype context owned by the mesh

`std::unique_ptr<HdEmbreePrototypeContext> _prototypeContext`, with
`_prototypeContext.get()` attached as the prototype geometry's user data.
`_GetPrototypeContext()` becomes a read of the member.

`mesh.h:26-27` forward-declares both context structs and `mesh.h:68` defines
`virtual ~HdEmbreeMesh() = default;` inline. A `unique_ptr` to an incomplete
type will not compile there. **Declare the destructor in the header and define
it out-of-line in `mesh.cpp`**, where `context.h` is already included. Do not
include `context.h` from `mesh.h` to work around this: `mesh.h` is included far
more widely than the context definitions need to be, and the out-of-line
destructor is where the lifetime ordering should be readable anyway.

### Sampler teardown

Primvar samplers stay raw-owned by `primvarMap` for this plan, but their
teardown must stop being open-coded. Today the same “iterate `primvarMap`,
`delete it->second`” loop appears at `mesh.cpp:750` (finalize) and
`mesh.cpp:1776` (rebuild), with targeted single-name deletions at
`mesh.cpp:1567` and `mesh.cpp:1942`. Collapse the two whole-map loops into one
private `_ReleasePrimvarSamplers()` and call it immediately before the prototype
context is destroyed. This is required for correctness, not tidiness: once
`_prototypeContext` is a `std::unique_ptr`, any path that resets it without
first releasing the samplers leaks every sampler in the map. `15` deletes this
helper when it rehomes sampler storage.

### Embree handles

Embree RTC handles may remain explicit if their release order is clearer than a
custom deleter. If RAII wrappers are introduced, use one concrete local wrapper
per handle type and demonstrate a net code reduction.

## Failure postconditions

“Audit every early return” is not actionable on its own, and the line numbers
this plan used to cite have already drifted. State the contract instead. Every
early return from `_PopulateRtMesh()` — currently the two `TF_CODING_ERROR`
paths, one after prototype creation and one after vertex-buffer allocation —
must leave the mesh satisfying **all** of:

1. **No leaked geometry.** Any `RTCGeometry` created during this call is
   released before returning. *This is broken today.*
   `_CreateEmbreeTriangleMesh()`/`_CreateEmbreeSubdivMesh()` assign
   `_rtcMeshId` internally, and the caller checks it only after the geometry
   handle has already been stored in `_geometry`. On the failure path
   `_rtcMeshId` is `RTC_INVALID_GEOMETRY_ID`, so `Finalize()`'s release is
   guarded off (`mesh.cpp:747`) and the handle leaks. Converting contexts to
   RAII does not fix this; the create helpers must stop mutating `_rtcMeshId`
   as a side effect and return success explicitly.
2. **No dangling Embree user data.** No geometry that outlives the call
   references a context the call destroyed, and no context outlives the
   geometry it is attached to.
3. **Agreement.** `_rtcMeshId`, `_geometry`, `_rtcMeshScene`, and
   `_prototypeContext` are mutually consistent: either all describe one live
   committed prototype, or all describe the defined empty state.
4. **Instances remain valid.** Existing instances reference either a valid
   committed prototype scene or the defined empty state — never a
   half-rebuilt one.
5. **Retry is possible.** The consumed dirty bits are *not* cleared, so the
   next `Sync()` re-attempts the rebuild. (The clear at the end of
   `_PopulateRtMesh()` is already skipped by an early return; the plan must
   confirm this rather than assume it.)

## Implementation sequence

1. Document current lifetime ordering and every allocation/deletion site.
2. Collapse the two instance vectors into `std::vector<_Instance>` with a
   `unique_ptr` context member; update grow, shrink, and finalize paths; delete
   `_GetInstanceContext()`.
3. Extract `_ReleasePrimvarSamplers()` from the two duplicated whole-map delete
   loops, so sampler teardown has a single call site before the context dies.
4. Convert the prototype context to mesh ownership; move the destructor
   out-of-line; update rebuild/finalize; delete `_GetPrototypeContext()`.
5. Make `_CreateEmbreeTriangleMesh()`/`_CreateEmbreeSubdivMesh()` report failure
   through their return value instead of via `_rtcMeshId`, and fix the leak in
   postcondition 1.
6. Walk each early return against the five postconditions above; add the
   missing releases and reset the members that must agree.
7. Document the `_PopulateRtMesh()` declaration contract (`mesh.h:195`, which is
   currently a one-line comment) per **Goal 4**, limited to what this plan owns:
   preconditions (only pulls dirty buffers); ownership (which members the call
   creates, replaces, and destroys); the five failure postconditions above; and
   the caller contract (the caller commits the root scene afterward, and the
   prototype must be committed before instances). Broader dirty-bit and
   call-flow narration belongs to `16` or `21`.

## Validation

- Focused lifetime exercises: create/delete meshes, change topology between
  triangle and subdivision, add/remove instances, replace primvars repeatedly,
  and force the prototype-creation failure path.
- Exercise material/displacement changes while the render thread is active to
  verify the existing stop-before-mutation contract.
- Run subdivision, wireframe, light-linking/instancing, material, and render
  tests.
- Fixed-seed images remain bit-identical.
- **Sanitizers are opportunistic.** No ASan configuration exists in this
  repository today — not in `pixi.toml`, not in either `CMakeLists.txt`. Run
  under ASan only if one has been added by then; adding one is a separate
  build-workflow change and is explicitly not part of this plan. Do not gate
  completion on it.
- After the lifetime tests are written, run an adversarial review over them:
  confirm they would actually fail against the pre-change code rather than
  merely encoding current behavior, and that the failure paths are genuinely
  reached.

## Risks and decisions

- Member destruction order must not free contexts before Embree geometries are
  detached/released. Explicit `reset()` in `Finalize()` may still be needed.
- `std::vector<std::unique_ptr<...>>` moves pointer owners but not pointees, so
  attached addresses remain stable. The same holds for `_Instance`: growing
  `_instances` moves the record, not the pointed-to context.
- Ensure no render worker retains a sampler/context after scene-edit locking
  permits replacement.
- The prototype context transitively owns the primvar samplers while this plan
  is in flight. Destroying it through `unique_ptr` without first calling
  `_ReleasePrimvarSamplers()` is a silent leak that no test currently catches;
  this is the single highest-risk detail in the plan.

## Documentation

- `ARCHITECTURE.md`'s `mesh.h/.cpp` entry (`:38`) gains the ownership statement:
  the mesh owns the prototype and instance contexts; Embree borrows stable
  addresses to them.
- `README.md` needs no change — no user-visible behavior changes.

## Completion criteria

- One instance container, not two or three; `_GetInstanceContext()` and
  `_GetPrototypeContext()` are gone.
- No direct `delete` remains for prototype contexts or instance contexts.
- `~HdEmbreeMesh()` is defined out-of-line.
- Primvar-sampler deletion happens in exactly one whole-map operation, invoked
  before the prototype context is destroyed on every path.
- Every early return in `_PopulateRtMesh()` satisfies the five postconditions,
  and the prototype-creation leak is fixed.
- Rebuild and finalize paths are shorter and share one documented lifetime
  order.
- Focused lifetime tests pass; images bit-identical.

## Mandatory final suite gate

After every plan-specific validation above, run the complete Typhoon suite as
the final gate:

```sh
cd ~/code/typhoon-test-suite
powerprofilesctl launch --profile performance -- pixi run pytest --renderer typhoon-local
```

Run the complete suite to completion; never interrupt it because of elapsed time.
All tests must pass. Report the total elapsed time. Runtime is variable: warn
when it exceeds 250 seconds, but timing alone does not fail the gate. Do not
commit the plan implementation until Anders has reviewed the completed changes
and explicitly approved committing them.
