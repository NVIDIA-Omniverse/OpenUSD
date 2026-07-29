# Plan: Make MaterialX compilation and evaluation failures explicit

Status: correctness, diagnostics, and API-contract cleanup from the full-codebase
review.

## Issue

`EvalGraph::Compile()` normally reports invalid graphs by returning a non-null
object whose `IsValid()` is false, but malformed graph structure can also throw.
The DFS records a missing upstream path as `Visited` and appends it to the
sorted list (`graph.cpp:449-450`); the build phase then calls
`normalized.nodes.at(path)` (`graph.cpp:475`), which throws. The behavior is
therefore inconsistent: some malformed graphs are ordinary invalid results,
while missing nodes escape as exceptions.

`Compile()` also can't express the difference between three distinct outcomes:
a **valid** graph, an **invalid** (malformed) graph, and an **absent** terminal
(the requested terminal simply isn't present). Today an absent terminal and a
malformed terminal both yield the same empty invalid graph (`graph.cpp:391-401`),
and the only signal is an `fprintf` that fires solely when `terminalName` is
empty. Because the delegate compiles a displacement graph for *every* material,
a naïve "warn whenever the graph is invalid" rule in the delegate would warn on
almost every non-displaced material. Distinguishing absent from invalid is a
hard requirement, not a nicety.

The delegate catches all exceptions around conversion/compilation and emits a
generic material-level warning. Every hit-time surface evaluation path also
wraps `EvalGraph::Evaluate()` in `catch (...)` and silently selects fallback
shading. There are four such sites, not one:

- `integrator/pathIntegrator.cpp:279` (path evaluation);
- `integrator/unlitIntegrator.cpp:96` (unlit evaluation);
- `integrator/surfaceShading.cpp:707` (visibility / `visibilityOnly`
  evaluation);
- `geometry/displacementEvaluation.cpp` (five `catch (...)` wrappers around the
  displacement entry points).

This obscures the failing node, makes the public contract unclear, and puts
exception machinery in per-hit paths.

`graph.h` does not state whether `Compile`, `Evaluate`, or
`EvaluateDisplacement` can throw, which invalid inputs are accepted, or how a
caller obtains diagnostics.

## Goals

- Graph compilation is non-throwing for malformed authored networks.
- `Compile()` reports **valid**, **invalid**, and **absent-terminal** as three
  distinct, explicit outcomes.
- A caller can obtain a concise diagnostic describing the failure in downstream
  context (the node that references the problem, plus what is wrong).
- Hit-time evaluation is non-throwing by contract and does not use blanket
  exception handling.
- Surface and displacement terminals can still fail independently.
- Unsupported nodes remain a normal, diagnosable invalid-graph result.

## Non-goals

- Do not redesign the graph representation or MaterialX adapter.
- Do not introduce a general error framework, exceptions, or logging callbacks.
- Do not change fallback-material appearance.
- Do not do the tiny-helper inlining / DRY cleanup of
  `displacementEvaluation.cpp` — that is `10-plan-displacement-cleanup.md`, which
  is behavior-preserving. This plan only removes the displacement `catch (...)`
  wrappers (a behavior change) and establishes the `EvaluateDisplacement()`
  contract that plan 10 relies on.

## Terminal semantics

Define these explicitly and implement the delegate accordingly:

- **Absent optional terminal** (e.g. no displacement authored): expected. No
  warning. The delegate simply skips that terminal.
- **Present but malformed terminal** (unsupported node, missing upstream node,
  cycle, missing evaluator): invalid. Emit one warning.
- **Missing surface with no volume or displacement terminal**: invalid — the
  material falls back to display-color shading and warns once.
- **Volume-only material**: valid. Compilation synthesizes the internal
  transparent surface-volume terminal so the renderer can cross the medium
  boundary.
- **Displacement-only material**: valid. The displacement graph is compiled,
  the absent surface remains the renderer's display-color fallback, and no
  missing-surface warning is emitted.
- The current "use first available terminal when the requested surface terminal
  is absent" behavior at `graph.cpp:391-393` is intentionally **removed**: an
  absent surface terminal now reports `Absent` rather than silently substituting
  an arbitrary terminal.

## Proposed design

### Compile result: explicit tri-state

Give `Compile()` an explicit status rather than an implicit convention. Return a
small result carrying status + graph + one diagnostic string:

```
enum class CompileStatus { Valid, Invalid, AbsentTerminal };
struct CompileResult {
    CompileStatus status;
    std::unique_ptr<EvalGraph> graph;   // null unless Valid
    std::string diagnostic;             // populated only when Invalid
};
```

The tri-state is chosen over the alternative "keep `unique_ptr` + empty
diagnostic means absent" convention because the delegate has a hard requirement
to distinguish absent-displacement (silent) from malformed-displacement (warn),
and the implicit empty-string encoding is exactly the subtlety that makes that
call site error-prone. `AbsentTerminal` carries no diagnostic; `Invalid` always
carries one; `Valid` carries a usable graph.

Do not accumulate an elaborate diagnostic tree; one actionable first error is
enough.

### Structural validation in `Compile()`

- Validate `nodePath` at a single point: on DFS entry, look it up immediately;
  if absent, record a diagnostic with the downstream connection context and
  return false. This removes the "missing node marked `Visited` and pushed to
  `sorted`" path, so the later `normalized.nodes.at(path)` (`graph.cpp:475`) is
  enforcing an invariant rather than handling authored failure and need not
  change.
- Continue to detect cycles and missing evaluators explicitly, each producing a
  diagnostic.
- Remove **all** `fprintf` diagnostics from `Compile()` (`graph.cpp:396, 406,
  426, 480`); diagnostics are returned via `CompileResult`, never printed from
  MaterialXCpp. This avoids double-reporting once the delegate logs.

### Diagnostic context rules

Diagnostics describe the *downstream* context, because an upstream problem (e.g.
a missing node) has no type of its own. Required forms:

- Missing upstream node:
  `/Material/Add (ND_add_float) input in1 references missing node /Material/Missing`
- Cycle: name a node on the cycle:
  `cycle detected through node /Material/Foo`
- Missing terminal node (terminal present, its upstream node absent):
  `terminal "displacement" references missing node /Material/Disp`
- Unsupported evaluator:
  `no evaluator registered for node type ND_totally_unknown_float at /Material/X`

`AbsentTerminal` produces no diagnostic (it is not a failure).

### Evaluation contract (narrower, honest)

Document and enforce this contract in `graph.h`:

> Evaluation does not throw due to malformed authored values, missing inputs, or
> type mismatches. Renderer/backend callbacks (texture, geomprop, transform,
> color-transform) must translate recoverable failures into their documented
> fallback results.

This is deliberately narrower than an absolute `noexcept`, which would require
catching every callback and allocation exception or accepting termination. Node
evaluator functions validate variant/type access through the existing
`Get<T>`/`ValueGetter` paths. If a genuinely external texture backend can throw,
catch at that narrow boundary, report once with the filename and whatever
context the boundary has, and return a documented fallback value. (Propagating
owning-material identity down to the texture request is out of scope; the
boundary has the filename.)

### Delegate reporting

`HdEmbreeMaterial::Sync()` reports terminal-specific results with the material
path:

- `AbsentTerminal` → skip silently (this is what makes non-displaced materials
  quiet).
- `Invalid` → one warning: material path + terminal name + the compile
  diagnostic.
- `Valid` → use the graph.

Narrow or remove the compile `catch (...)` in the delegate once `Compile()` is
non-throwing.

## Implementation sequence

This is three separable chunks. Chunk 1 is clean and independently valuable and
should land first. Chunk 2 needs the callback audit before any catch is removed.
Chunk 3 depends on chunk 2.

**Chunk 1 — compile validation, diagnostics, delegate reporting**

1. Extend the existing graph tests to assert diagnostics and status, and add the
   gaps:
   - missing upstream node (new) — asserts `Invalid` + downstream-context
     diagnostic;
   - missing terminal node (new);
   - cycle — extend `TestCompileRejectsCycle` to assert the diagnostic;
   - unknown evaluator — extend `TestCompileUnknownNodeTypeFails` to assert the
     diagnostic;
   - absent vs present-but-malformed surface/displacement — extend
     `TestCompileMissingDisplacementTerminal` to assert `AbsentTerminal` (no
     diagnostic) distinctly from `Invalid`.
2. Make DFS/build validation return invalid graphs without throwing (single
   entry-point validation), and remove all `fprintf` from `Compile()`.
3. Introduce `CompileResult` / `CompileStatus`, document the contract and the
   three outcomes in `graph.h`, and update call sites.
4. Update `HdEmbreeMaterial::Sync()` to report terminal-specific diagnostics per
   the delegate-reporting rules; narrow or remove its compile `catch (...)`. Add
   a delegate-level test that verifies material path, terminal name, suppression
   for an absent displacement terminal, and the warning count (exactly one for a
   single malformed material).

**Chunk 2 — runtime evaluation contract**

5. Audit every node evaluator and every renderer callback boundary for throwing
   access; make the evaluation contract hold as documented.
6. Remove the per-hit blanket `catch (...)` from all four surface paths:
   `pathIntegrator.cpp:279`, `unlitIntegrator.cpp:96`, `surfaceShading.cpp:707`.
   Preserve each site's existing fallback semantics (they differ).

**Chunk 3 — displacement**

7. Apply the same explicit contract to displacement evaluation and remove the
   `catch (...)` wrappers in `displacementEvaluation.cpp`, preserving fallback
   behavior. Coordinate the `EvaluateDisplacement()` contract with
   `10-plan-displacement-cleanup.md` (which does the behavior-preserving helper
   cleanup); this chunk owns the catch removal, plan 10 does not. Leave the
   `HdEmbreeXxx()` → `_XxxImpl()` delegations in place even though they become
   trivial — plan 10 collapses them.

## Validation

- Every malformed-graph test returns `Invalid` with a stable, useful diagnostic;
  every absent-terminal test returns `AbsentTerminal` with no diagnostic; no test
  depends on an exception.
- Existing `testMaterialXCppGraph`, adapter, node, material, and BSDF tests pass.
- Render a stage with one invalid material and confirm a single actionable
  warning identifies material and failing node while fallback shading remains;
  confirm a non-displaced material produces no displacement warning.
- Fixed-seed valid-material renders are bit-identical.

## Risks and decisions

- Logging inside pxr-independent MaterialXCpp would introduce an unwanted USD
  dependency; diagnostics are returned via `CompileResult` and logged by the
  delegate only.
- Texture libraries may throw outside MaterialXCpp's direct control. Catch only
  at the smallest boundary that can add filename/context.
- Avoid printing from both `Compile()` and the delegate, which would duplicate
  diagnostics — hence the explicit removal of `Compile()`'s `fprintf` calls.
- The absent/invalid distinction is load-bearing for delegate warning volume; it
  is encoded explicitly (tri-state), not by convention.

## Completion criteria

- Malformed networks never require exceptions for control flow.
- The graph API documents all failure behavior and the three compile outcomes.
- No `catch (...)` remains in the per-hit material or displacement evaluation
  paths.
- Surface/displacement failures retain independent diagnostics and fallback, and
  absent terminals are silent.

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
