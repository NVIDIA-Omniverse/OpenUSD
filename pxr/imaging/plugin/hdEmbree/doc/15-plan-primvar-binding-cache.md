# Plan: Resolve geomprop primvar bindings once per prototype instead of per hit

Status: performance and simplification. Behavior-preserving **except** for the
connected-`geomprop` case (see **Risks**), which must be audited before the
claim is made unconditional. Executes after plans 04, 13, and 14: plan 14
first relocates the `rendererImpl.h` primvar consumers into their final
modules, then this plan rewrites them.

## Issue

A MaterialX `geompropvalue` node names its primvar with a string. Today that
name is re-resolved against a hash map **on every evaluation of the node**,
which means on every shading-graph evaluation, which means several times per
ray hit.

The chain, hit-side first:

- `MaterialXCpp/nodes/geometricNodes.cpp:322-342` — `_EvalGeomPropValue()`
  reads the `geomprop` input out of the node's `ParamMap` and calls
  `ctx.geomPropLookup(ctx.geomPropUserData, *name)`.
- `MaterialXCpp/shadingContext.h:91` — `GeomPropFn` is
  `Value(*)(const void* userData, const std::string& name)`. The name is the
  only binding information the callback receives.
- `geometry/primvarSampling.h:168-175` — `HdEmbreeSamplePrimvar()` does
  `lookup->primvars->find(name)`: a `std::string` hash and compare, per call.
- `geometry/context.h:91-92` — `primvarMapByString` is the map being hashed. It
  exists *only* for this callback.

`primvarMapByString` is an exact duplicate of `primvarMap`, differing only in
key type. It exists because the pxr-independent shading core cannot name a
`TfToken`, and interning one per evaluation would take the global token-table
lock on the shading inner loop. Both maps are written and erased together at
five sites in `mesh.cpp` — `:1567`/`:1570` and `:1632`/`:1633`
(`_CreatePrimvarSampler`), `:1942`/`:1945`, `:1971`, `:2015` — and any site that
updates one and forgets the other leaves a dangling sampler pointer.

The uniform variant has the same shape and is worse per call:
`_EvalGeomPropValueUniformString()` (`geometricNodes.cpp:344-359`) does
`Get<std::string>(inputs, _kGeomprop, ...)`, which **copies** the name, then
hashes it against `ctx.uniformProps` (`context.h:97-99`, `uniformPrimvarMap`).
The varying node already carries a comment explaining that it deliberately
avoids that copy, so the cost is understood to matter.

None of this lookup is dynamic. MaterialX declares the `geomprop` input on
`ND_geompropvalue_*` and `ND_geompropvalueuniform_*` as `uniform`, so the name
is a compile-time constant of the graph: in `mxcpp` terms it arrives in
`GraphNode::parameters`, not `GraphNode::inputConnections`
(`MaterialXCpp/graphTypes.h:21-25`). We are re-deriving a constant answer
millions of times per frame.

### Why this violates the project goals

- **Goal 1 (simple).** Two containers holding identical data, kept in sync by
  hand across five sites, are complexity that buys nothing a compile-time
  resolution would not give for free.
- **Goal 2 (linear flow).** The reader must currently understand *why* the same
  samplers appear under two key types before they can read either.

## Goals

- Resolve each `geompropvalue` node's primvar name to a small integer handle
  once, when the material's network is compiled — in **one** handle space shared
  by the surface and displacement terminals.
- Resolve each handle to a concrete `HdEmbreePrimvarSampler*` once per
  prototype, when the mesh binds its material and samplers under the scene-edit
  lock — and **re-resolve whenever the bound material recompiles**, which does
  not currently imply a mesh sync.
- Make hit-time geomprop lookup an array index: no string hash, no string copy,
  no `TfToken` construction.
- Delete `primvarMapByString` and the five-site synchronization it requires.
- Give the surviving sampler storage real ownership (`std::unique_ptr`), which
  `13-plan-mesh-ownership.md` explicitly defers to this plan.

## Non-goals

- Do not change what a `geompropvalue` node returns for any input whose
  `geomprop` is authored as a constant. This plan is bit-identical-image work
  for every supported authoring path.
- Do not add a general "primvar system" abstraction. The only new concepts are a
  per-material name table and a per-prototype resolved vector.
- Do not change how primvars are authored, triangulated, refined, or sampled.
  `HdEmbreePrimvarSampler` and its subclasses are untouched.
- Do not touch texture, colour-transform, or transform-space callbacks, which
  have the same shape but are out of scope.

## Proposed design

### 0. Two graphs, one handle space

A material compiles **two independent `EvalGraph`s** — `material.cpp:94`
compiles the surface terminal and `material.cpp:98` compiles `"displacement"`,
each producing its own `std::unique_ptr<EvalGraph>` (`material.h:48-49`). They
are reached through separate members, `HdEmbreeMaterialData::evalGraph` and
`::displacementGraph` (`materials/material.h:18-19`), and they have separate
consumers: the integrators evaluate `evalGraph`
(`surfaceShading.cpp:648`, `pathIntegrator.cpp:268`, `unlitIntegrator.cpp:85`)
while displacement evaluates `displacementGraph`
(`displacementEvaluation.cpp:277, 364`, `mesh.cpp:466, 687`).

But both are compiled from the **same `MaterialGraph`** — `material.cpp:92`
builds one `mxcppGraph` and `:94`/`:98` compile two terminals out of it. The set
of geomprop names is therefore a property of the **network**, not of either
terminal.

So the handle space must be **owned by the material, not by the graph**. Build
the name table once from the `MaterialGraph` and pass it into both `Compile()`
calls. One handle space per material, one resolved table pair per prototype.

The alternative — a table per `EvalGraph`, built from each terminal's reachable
subgraph — was considered and rejected. It doubles the state on the prototype
context, forces every callback site to choose the right pair, and creates a
silent-wrong-result failure mode (surface handles used against displacement
bindings) that then needs its own guard. The only thing it saves is the cost of
resolving a name one terminal does not reference: a few null pointers in a
vector sized by the number of distinct geomprops in a material, typically one to
five. That is not a trade worth making.

(Leaving the table inside `Compile()` but building it from the whole network
rather than the reachable set would also yield identical handle spaces, since
`MaterialGraph::nodes` is a `std::map` (`graphTypes.h:27`) and enumeration is
deterministic. Rejected as well: the invariant would be implicit, and a reader
would have to derive it from the container type.)

### 1. `mxcpp`: a per-material geomprop name table

Add a free function that collects the table from a network:

```
std::vector<std::string> CollectGeomPropNames(const MaterialGraph& network);
```

It walks `network.nodes` and, for each `geompropvalue`/`geompropvalueuniform`
node, interns the constant `geomprop` parameter into an ordered, de-duplicated
vector.

`EvalGraph::Compile()` (`MaterialXCpp/graph.h:32`) takes the table as an input
and uses it to rewrite geomprop input bindings (§2). It does not build one and
does not own one — a compiled graph is only valid against the table it was
compiled with.

There are ~25 `Compile()` call sites (18 in `testMaterialXCppGraph.cpp`, 5 in
`testHdEmbreeSubdivision.cpp`, 2 in `material.cpp`), almost none of which care
about the table. Keep the existing two-argument signature as a convenience
overload that calls `CollectGeomPropNames()` internally, and add the
three-argument form that takes an explicit table. Only `HdEmbreeMaterial::Sync()`
uses the explicit form, collecting once and passing the same table to both
terminal compiles — which is exactly where the shared handle space is
established, and now visibly so.

**`HdEmbreeMaterialData` owns the table by value:**

```
struct HdEmbreeMaterialData
{
    ::mxcpp::EvalGraph* evalGraph = nullptr;
    ::mxcpp::EvalGraph* displacementGraph = nullptr;
    /// Handle space shared by both graphs above. Prototypes size and index
    /// their resolved sampler vectors against this.
    std::vector<std::string> geomPropNames;
};
```

`HdEmbreeMaterialData` is already the stable renderer-facing handle that
prototype contexts hold, so putting the table directly in it gives graphs and
prototypes one authoritative copy and adds no new lifetime relationship. Do not
leave it owned by `HdEmbreeMaterial` and pointed at from here.

If a graph connects the `geomprop` input instead of authoring it as a constant,
the node gets no handle. That is outside the MaterialX `uniform` contract:
report it once through the plan-04 error path and evaluate the node's
`default` input. Do **not** keep a name-based fallback lookup alive for it —
that would preserve exactly the map this plan removes.

### 2. `mxcpp`: deliver the handle through the existing input binding

`NodeEvalFn` receives only `(const ParamMap&, const ShadingContext&,
NodeOutputMap*)` (`MaterialXCpp/nodeRegistry.h:17-20`). It never sees its
`CompiledNode`, so a handle stored on the node cannot reach the evaluator.

Rather than change every `NodeEvalFn`, **rewrite the node's `geomprop` input
binding at compile time**: replace the constant string `defaultValue` with the
integer handle. `_BuildParamMap()` (`graph.h:78`) then supplies it through the
normal path, and `_EvalGeomPropValue()` reads an int out of the `ParamMap`
instead of a string. No signature change, no per-node side table, and the
string disappears from the eval-time `ParamMap` entirely.

The `ShadingContext` callback becomes:

```
using GeomPropFn = Value(*)(const void* userData, int geomPropHandle);
```

and `uniformProps` (`shadingContext.h:97`) becomes a handle-indexed
`std::vector<Value> const*`. Document on both members that the handles are
meaningful only against the name table of the material that produced the graph.

**Bounds checks are mandatory, not optional hardening.** Today an unresolvable
name produces a graceful "not found" and the node falls back to its `default`
input. A handle-indexed design turns that same situation into undefined
behavior unless it is checked, so every step must degrade to the existing
fallback rather than index blindly:

- the evaluator rejects a missing or negative handle before calling the
  callback (a node whose `geomprop` could not be resolved at compile time);
- the callback independently verifies **both** bounds — `handle >= 0` and
  `handle < vector.size()` — before indexing. It is the defensive backstop for a
  prototype whose table refresh was missed, so it must not assume the evaluator
  already screened the handle;
- the temporary commit-2 adapter checks both bounds before indexing
  `geomPropNames[handle]`.

One predictable compare per lookup is not a measurable cost against the string
hash it replaces, and it converts the worst failure mode in this plan from a
memory error into a wrong-but-safe default value.

### 3. Renderer: one resolved table pair per prototype

In `geometry/context.h`, replace `primvarMapByString` and `uniformPrimvarMap`
with one pair, valid for both of the material's graphs:

```
/// Handle-indexed against material->geomPropNames. Rebuilt whenever the
/// bound material or this prototype's samplers change.
std::vector<HdEmbreePrimvarSampler*> geomPropSamplers;
std::vector<mxcpp::Value> geomPropUniformValues;
```

Both are sized to `material->geomPropNames.size()` and hold `nullptr`/empty for
names the mesh does not author — the same "not found" outcome the map lookup
produces today. Entries for names only the other terminal references simply stay
unused. With no bound material the vectors are empty, matching the existing
null-graph early exits.

`primvarMap` stays, keyed by `TfToken`, because the pxr-side consumers
(`rendererImpl.h:718, 780, 804, 933, 969, 1070`,
`integrator/surfaceShading.cpp:426, 439, 449, 500-503`,
`geometry/displacementEvaluation.cpp:287, 336`) genuinely hold tokens already
and are not on the geomprop path. Make it own
`std::unique_ptr<HdEmbreePrimvarSampler>`; the resolved vectors observe it.

### 4. Renderer: two invalidation edges, one refresh operation

The resolved vectors are a function of (bound material's name table, sampler
set). There are **two** ways that input changes, and the second is the one this
plan must get right.

**Edge A — the mesh re-syncs.** Samplers are rebuilt and the material is bound
inside `_PopulateRtMesh()`, under `AcquireSceneForEdit()`. Rebuild the vectors
in one private `_ResolveGeomPropBindings()` called after the material is bound
and all `_CreatePrimvarSampler()` calls have run — not incrementally per
primvar. A whole-vector rebuild of a handful of entries during Sync is far
cheaper than maintaining incremental correctness.

**The two vectors resolve from two different sources.** This is easy to get
wrong, so state it: for each name in `material->geomPropNames`,

- `geomPropSamplers[i]` comes from `primvarMap`, looked up by `TfToken(name)` —
  the one place a token construction is still acceptable, because it happens
  during Sync rather than per hit;
- `geomPropUniformValues[i]` comes from **`_primvarSourceMap`**, via the
  existing `_GetUniformStringPrimvarValue()` (`mesh.cpp:168`), reproducing what
  `mesh.cpp:2026-2032` does today when it builds `uniformPrimvarMap`. String
  and filename constants never become samplers, so deriving this vector from
  `primvarMap` would silently produce empty values for every
  `geompropvalueuniform` node.

**Edge B — the material re-syncs without the mesh.**
`HdEmbreeMaterial::Sync()` replaces both graphs — and, under this plan, the name
table — behind a *stable* `HdEmbreeMaterialData` handle. The code says so
explicitly at `material.cpp:110-112` ("keep the handle stable and replace only
the graphs it points at"). Bound meshes are not guaranteed to receive
`DirtyMaterialId`, so a prototype can be left holding resolved vectors sized and
indexed against the material's **previous** name table. That is not merely
stale: if the new table is longer than the old, hit-time indexing reads out of
bounds.

The codebase already knows about this hazard and only half-solves it.
`UpdateSubdivisionLevels()` (`mesh.cpp:614`) carries the comment "Material
graphs are held through a stable handle and can change without dirtying every
bound mesh", and calls `_RefreshDisplacementState()` before any early exit —
but that path cannot be reused here:

- it returns immediately for unrefined meshes (`mesh.cpp:620-622`), so triangle
  meshes are never visited;
- it is driven by `UpdateAdaptiveSubdivision()`
  (`renderDelegate.cpp:500-515`), which needs camera matrices and a data
  window, so it does not run before a subdivision camera exists;
- it refreshes only the `displaced` flag, not any binding table.

So **15 must add its own refresh path — and it must not run inside
`HdEmbreeMaterial::Sync()`.** Walking the mesh registry from a material's Sync
would work today only by accident: material sprim sync happens to be serial
(`HdRenderDelegate::IsParallelSyncEnabled()` returns false for everything but
`extComputation`, and hdEmbree does not override it) and happens to precede
rprim sync (`renderIndex.cpp:1626` vs. the parallel rprim walk below it). Both
are Hydra-internal behaviors this plugin does not control, `_meshRegistryMutex`
protects only the registry and not mesh contents, and the walk would be
O(materials × meshes) per frame. Do not build on that.

Instead, split publish from refresh — and **do not add a new version counter for
it.** `_displacementVersion` is already exactly this signal:

- it is bumped only by `NotifyDisplacementChange()` (`renderParam.h:55-58`),
  whose only two callers are `HdEmbreeMaterial::Sync()` (`material.cpp:47`) and
  `HdEmbreeMaterial::Finalize()` (`material.cpp:130`);
- so it already means "a compiled material graph changed", despite its narrow
  name;
- and the render pass already observes it after `SyncAll()`, in `_Execute()`
  (`renderPass.cpp:746-749`, re-checked at `:956-958`).

A second atomic threaded through render delegate, render param, and render pass
would duplicate a signal that already exists and already fires at exactly the
right moments. So:

1. `HdEmbreeMaterial::Sync()` stops the render and bumps the version — it
   already does both via `NotifyDisplacementChange()` (`material.cpp:42-47`) —
   and installs the new graphs and name table. It touches no mesh.
2. `HdEmbreeRenderPass::_Execute()` calls `RefreshMaterialBindings()` over
   **all** meshes whenever `displacementChanged` is set, before rendering
   resumes.

Place that call **before** the camera-gated subdivision block at
`renderPass.cpp:1055-1073`, not after: committing subdivision geometry there
runs Embree's displacement callback, which evaluates the displacement graph
through the geomprop lookup. Refreshing afterwards would displace against stale
bindings for one frame.

This reuses an existing, proven hook rather than inventing one:
`renderPass.cpp:1060-1073` already has exactly this shape — stop the render,
call a delegate-wide walk (`UpdateAdaptiveSubdivision()`), reset accumulation,
restart. The new call sits just above it but **without that block's
`hasAttachedCamera && _hasSubdivisionCamera` gate**, which is precisely what
makes the existing walk unusable here.

**Rename the signal.** Once bindings depend on it, `_displacementVersion` /
`NotifyDisplacementChange()` actively misleads — it has never been
displacement-specific, and after this plan a reader tracing binding correctness
will look for it under the wrong name. Rename to `_materialVersion` /
`NotifyMaterialChange()` in this plan, since this is the change that makes the
old name wrong. It is mechanical: `renderParam.h`, `renderPass.cpp`,
`renderDelegate.{h,cpp}`, `material.cpp`. Coordinate with
`05-plan-naming-core.md`.

`RefreshMaterialBindings()` is a sibling of `UpdateAdaptiveSubdivision()`
(`renderDelegate.h:182`, `renderDelegate.cpp:500-515`), walking the existing
`std::vector<HdEmbreeMesh*> _meshes` (`renderDelegate.h:309`) under
`_meshRegistryMutex`. Refresh every mesh unconditionally rather than tracking
which material changed: re-resolving a handful of pointers per prototype, once
per frame in which any material recompiled, is cheaper than plumbing changed-
material identities through, and it cannot miss a binding.

**Failed and empty recompiles count as changes.** `HdEmbreeMaterial::Sync()`
has early returns for a `GetMaterialResource()` exception and for an empty
resource, both after the graph pointers are already nulled
(`material.cpp:52-55`). Clear `geomPropNames` in the same place. The version is
already bumped at `material.cpp:47`, *before* those returns, so the refresh
still fires — a material that fails to recompile ends up with empty prototype
tables rather than tables indexed against a name table that no longer exists.

### 5. Renderer: update the callback set-up sites

`HdEmbreePrimvarLookup` (`geometry/primvarSampling.h:31-37`) swaps its map
pointer for the two vector pointers. Because there is one table per material,
every site passes the same pair and none of them has a choice to get wrong:

- `integrator/surfaceShading.cpp:670-675`;
- `integrator/pathIntegrator.cpp:238-241`;
- `integrator/unlitIntegrator.cpp:73-76`;
- `geometry/displacementEvaluation.cpp:341-343`.

This is the payoff of the design decision in §0: a per-graph table would have
made this list a per-site correctness question.

## Implementation sequence

Four commits, each independently buildable and testable.

1. **Audit the `uniform` assumption.** Confirm across every supported authoring
   path — including the UsdPrimvarReader `varname` → `geomprop` mapping in
   `mxcppAdapter.cpp:_CanonicalInputName()` (`:102-129`) — that no path
   produces a *connected* `geomprop` input. Record the result. If one does, the
   fallback decision in design §1 must be revisited before anything else lands,
   and the behavior-preserving claim in this plan's status line is withdrawn.
2. **`mxcpp` name table and handle binding.** `CollectGeomPropNames()`, the
   three-argument `Compile()` plus the collecting convenience overload, the
   `geomprop` input-binding rewrite, the `GeomPropFn` signature change, the
   `uniformProps` vector, and the bounds checks. `HdEmbreeMaterial::Sync()`
   collects the table once and passes it to both compiles; the ~23 test call
   sites keep working through the overload. Update
   `testMaterialXCppNodes.cpp:1421-1571`, which already exercises both node
   families.
   At this point hdEmbree's callback still ends in a map lookup, keyed by
   `geomPropNames[handle]` — slower than today, but a small reviewable diff that
   isolates the interface change. **The temporary adapter's user data must carry
   the material's name table**, not just the sampler map, since the handle means
   nothing without it.
3. **Renderer resolved table and invalidation.** Add `geomPropSamplers` /
   `geomPropUniformValues` and `_ResolveGeomPropBindings()`; add the
   `RefreshMaterialBindings()` and its ungated call in
   `HdEmbreeRenderPass::_Execute()`, driven by the existing
   `displacementChanged` signal and placed above the camera-gated subdivision
   block; point the four callback sites at the vectors; delete
   `primvarMapByString`, `uniformPrimvarMap`, and the five synchronization sites
   in `mesh.cpp`. This is the commit that makes the lookup an array index. The
   `_displacementVersion` → `_materialVersion` rename can be a separate trivial
   commit either side of this one.
4. **Sampler ownership.** Convert `primvarMap` to
   `TfHashMap<TfToken, std::unique_ptr<HdEmbreePrimvarSampler>, ...>` and delete
   `_ReleasePrimvarSamplers()` (introduced by plan 13). Update both the ~15
   `it->second` read sites **and the construction sites** — note
   `testHdEmbreeSubdivision.cpp:1107-1108` currently assigns the address of a
   *stack* sampler into `context.primvarMap`, which will not compile against an
   owning map and must be reworked, not merely adjusted. Mechanical, and last so
   the perf change bisects independently of the lifetime change.

## Validation

- **Required before/after measurement**, following the AGENTS.md profiling
  workflow rather than a reduced version of it. This plan's entire justification
  is the hit-time cost, so an unmeasured landing is not acceptable:
  - profile build, fixed
    `-s "{settings}.ty:randomNumberSeed = 1"`, identical resolution, sample
    counts, and scene-authored settings between measurements;
  - `perf stat -r 5` end-to-end, **and** the renderer-reported render time and
    samples per second, which AGENTS.md requires alongside wall-clock;
  - more than one workload: a `geompropvalue`-heavy material case is the
    headline, but include a texture-heavy and a deep-path scene before
    generalizing the result;
  - record the baselines and the outcome in `OPTIMIZATION.md`, not only in the
    commit message — `/tmp` profile artifacts are ephemeral.
- Fixed-seed images remain bit-identical across all four commits.
- `testMaterialXCpp` node tests cover: constant name resolving to a value; a
  name the mesh does not author falling back to `default`; the uniform-string
  variant; and a graph with two nodes naming the same primvar sharing one
  handle.
- A `CollectGeomPropNames()` test asserting that the table covers names
  reachable only from the displacement terminal as well as the surface one —
  this is what keeps the single handle space valid.
- New hdEmbree tests for the two invalidation edges, neither covered today:
  1. replace a primvar on a mesh and confirm the resolved vector is rebuilt;
  2. **edit a material so it recompiles with a different geomprop set, without
     dirtying the bound mesh, and confirm shading is correct afterwards** —
     including the case where the new table is *longer* than the old, which is
     the out-of-bounds scenario. This must be exercised on a **triangle** mesh,
     which `UpdateSubdivisionLevels()` never visits.
  3. a material that fails to recompile (empty or malformed resource) clears
     the prototype tables rather than leaving them stale.
- A test that a mesh whose surface and displacement terminals name *different*
  primvar sets shades and displaces correctly.
- Subdivision and displacement tests, because `displacementEvaluation.cpp`
  evaluates the second graph at a different time with a separately constructed
  lookup.
- After the tests above are written, run an adversarial review over them. Note
  the right bar: the pre-change map implementation is behaviorally **correct**,
  so "would this fail against the old code?" is the wrong question for the
  invalidation tests — they pass before and after. Require instead that each
  test fails against a plausible *broken* cached implementation:
  - a per-terminal name table instead of a per-material one;
  - a missing or gated post-`SyncAll()` material refresh;
  - a missing sampler re-resolve on mesh sync;
  - an unchecked handle index.

  A test that no broken variant fails is not testing this change.

## Risks and decisions

- **The `uniform` assumption is load-bearing** and is the subject of commit 1.
  Until it is confirmed, this plan's behavior-preserving claim is conditional.
- **Edge B is the main correctness risk.** A prototype holding vectors resolved
  against the material's previous name table would be an out-of-bounds read
  rather than a stale value, which is why the §2 bounds checks are mandatory
  even with the refresh in place. `RefreshMaterialBindings()` must be
  unconditional; any gating on subdivision, camera, repr, or displacement state
  reintroduces the bug.
- **Do not move the refresh back into `HdEmbreeMaterial::Sync()`.** It will
  appear to work — material sprim sync is currently serial and precedes rprim
  sync — but that is a Hydra-internal accident, `_meshRegistryMutex` does not
  protect mesh contents, and the cost is O(materials × meshes) per frame.
- The name table must be collected from the whole network, not from either
  terminal's reachable subgraph. Collecting per terminal silently reintroduces
  two handle spaces and the mix-up hazard §0 exists to remove.
- The **table** is per material; the **resolution** is per prototype. Prototypes
  are shared across instances and materials are shared across prototypes, so the
  resolved vectors must live on the prototype context. Putting them on the
  material would be wrong for any scene where two meshes with different primvars
  bind the same material.
- The invariant "these vectors are indexed against `material->geomPropNames` as
  of the last refresh" belongs in the `HdEmbreePrototypeContext` declaration
  comment, and its mirror ("this graph is only valid against the name table it
  was compiled with") in `EvalGraph`'s.
- Commit 2 makes the callback temporarily slower. Do not land it alone if a
  release could be cut between commits.

## Completion criteria

- `primvarMapByString` and `uniformPrimvarMap` no longer exist.
- Hit-time geomprop evaluation performs no string hash, no string copy, and no
  `TfToken` construction.
- One handle space per material; all four callback sites pass the same resolved
  pair, so there is no per-site choice to get wrong.
- A material recompile — including a *failed* one — refreshes every bound
  prototype's table after `SyncAll()` and before rendering resumes, on triangle
  and subdivision meshes alike, and no mesh is touched from a material's
  `Sync()`.
- No handle is used to index anything without a bounds check; an unresolvable
  handle degrades to the node's `default` input, as the map lookup does today.
- `primvarMap` owns its samplers; no `delete` of a sampler remains anywhere.
- Measured shading-path improvement recorded in `OPTIMIZATION.md` — `perf stat`
  wall-clock plus renderer-reported time and samples per second, across more
  than one workload — with images bit-identical.
- No second version counter was added; `_displacementVersion` is reused and
  renamed to reflect what it has always meant.
- `ARCHITECTURE.md`'s `geometry/context.h` entry (`:94`) and its materials
  section describe compile-time handle resolution and the refresh path.

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
