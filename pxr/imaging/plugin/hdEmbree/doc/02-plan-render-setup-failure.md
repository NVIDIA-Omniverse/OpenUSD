# Plan: Make renderer setup failure explicit and terminal

Status: correctness + readability work from the full-codebase review. This is a
real crash fix, so it is a high implementation priority. It follows the
build-surface plan only; it has no naming dependency.

## Issue

The root problem is that `_aovBindings[i].renderBuffer` (an `HdRenderBuffer*`) is
**dereferenced through an unchecked `dynamic_cast` in several places**, and
`HdEmbreeRenderer::_PreRenderSetup()` is `void`, so `Render()` cannot tell that
setup failed and runs the full pipeline anyway.

The buffer must be cast to `HdEmbreeRenderBufferInterface` to reach
`SetConverged()`/`Map()`/`Resolve()` (there is no `SetConverged` on the base
`HdRenderBuffer` — it declares only `IsConverged()`), so the cast is mandatory;
the bug is that its result is never null-checked.

Unchecked deref sites (a null binding, or a non-null buffer that is *not* an
`HdEmbreeRenderBufferInterface`, makes each `dynamic_cast` return `nullptr`):

```cpp
// renderer/aov/aovOutput.cpp:307-313  — runs BEFORE _PreRenderSetup, from
// renderPass.cpp:1115 whenever a new render starts.
void HdEmbreeRenderer::MarkAovBuffersUnconverged() {
    for (size_t i = 0; i < _aovBindings.size(); ++i) {
        HdEmbreeRenderBufferInterface *rb =
            dynamic_cast<HdEmbreeRenderBufferInterface*>(_aovBindings[i].renderBuffer);
        rb->SetConverged(false);   // rb may be null -> crash
    }
}
```

```cpp
// renderer/renderer.cpp:425-429  — the "validation failed" path.
for (size_t i = 0; i < _aovBindings.size(); ++i) {
    HdEmbreeRenderBufferInterface *rb =
        dynamic_cast<HdEmbreeRenderBufferInterface*>(_aovBindings[i].renderBuffer);
    rb->SetConverged(true);        // rb may be null -> crash
}
```

The same unchecked cast+deref recurs at the map loop (`renderer.cpp:447`) and
the preview/resolve loop (`renderer.cpp:541-544`).

On top of the null deref, the control flow is wrong in four ways:

1. **`Render()` never branches on setup.** `renderer.cpp:490` calls
   `_PreRenderSetup()` and marches into preview/full/resolve/unmap regardless.
2. **Failure leaves stale state.** `_width`/`_height` reset (`renderer.cpp:435`)
   and `_BuildAovDispatchTable()` (`renderer.cpp:478`) sit *after* the failure
   `return` (`:432`), so a failed setup keeps last frame's dimensions and AOV
   writer list.
3. **Size/data-window problems are non-terminating diagnostics.** Inconsistent
   buffer sizes (`renderer.cpp:456`) and an out-of-bounds data window (`:464`)
   are `TF_CODING_ERROR` — they log and fall through. The containment check is
   also gated on `if (_width > 0 || _height > 0)` (`:462`), so a 0×0 buffer
   bypasses it entirely.
4. **Validation checks the raw pointer, not the type.** `_ValidateAovBindings`
   rejects `renderBuffer == nullptr` (`aovOutput.cpp:79`) but never confirms the
   `dynamic_cast` succeeds, so a wrong-type buffer passes validation and then
   null-derefs at the cast sites above.

## Goals

- A failed setup terminates the current `Render()` before any tile work or AOV
  finalization, and before `MarkAovBuffersUnconverged()` can crash.
- No null or wrong-type buffer is ever dereferenced.
- `Map()`/unmap stay balanced with no cleanup bookkeeping.
- Setup success/failure and postconditions are documented at the declaration.
- Successful-render behavior and output are preserved exactly.

## Non-goals

- Do not redesign Hydra AOV formats or add new AOVs.
- Do not merge with `06-plan-aov-dispatch.md`; land the correctness fix first,
  then 06 rebases the AOV-state build.
- Do not add exception-based control flow.
- **Do not** add a renderer→render-pass failure signal to distinguish "setup
  failed" from "genuinely converged." That is a separate, larger change owned by
  `03-plan-render-failure-signal.md`.

## Core change: validate before you commit or map

The key simplification is ordering: **check every observable failure before
`rtcCommitScene()` and before the first `Map()`.** Then no partial-mapped state
can exist, so there is no cleanup to unwind. `_PreRenderSetup()` becomes:

```cpp
void HdEmbreeRenderer::Render(HdRenderThread *renderThread) {
    _renderStartTime = std::chrono::steady_clock::now();  // moved to entry (see below)
    if (!_PreRenderSetup()) {
        return;                                            // buffers left converged; thread parks
    }
    ...
}
```

`_PreRenderSetup()` (now returning `bool`) runs in this fixed order:

1. Reset invocation-derived state (`_width=0`, `_height=0`, clear the AOV
   dispatch/classification list, reset the counters and other per-frame members
   built here — see `renderer.h:1316` "Pre-resolved per-frame state").
2. Validate everything observable *before touching the scene or buffers*:
   non-null `_scene`; at least one AOV binding; each `renderBuffer` non-null
   **and** castable to `HdEmbreeRenderBufferInterface`; supported format for the
   AOV; consistent, non-zero dimensions; a valid, in-bounds data window.
3. On any failure: apply the convergence policy below and `return false`.
4. `rtcCommitScene(_scene)`.
5. Allocate adaptive arrays and build the AOV dispatch state.
6. `Map()` every buffer.
7. `return true`.

Successful `Render()` remains solely responsible for unmapping every binding, as
today. Because every failure returns before step 6, `Map()` is only ever called
on a validated buffer — document it as non-failing in that case (allocation
`bad_alloc` aside, which is out of scope).

This removes the "track mapped buffers and unwind on later failure" vector that
an earlier draft proposed — it is unnecessary once validation precedes mapping.

## Null- and type-safety

Every `dynamic_cast<HdEmbreeRenderBufferInterface*>(renderBuffer)` that marks
convergence must null-check its result and skip null/failed-cast buffers —
including `MarkAovBuffersUnconverged()` (`aovOutput.cpp:307`), which runs before
setup. Do **not** try to route around the cast via a base-class `SetConverged`;
`HdRenderBuffer` has none (`renderBuffer.h:107` declares only `IsConverged()`).
Fold the type check into `_ValidateAovBindings` so a wrong-type buffer fails
validation up front rather than crashing later.

## Delete the validation cache

`_ValidateAovBindings` memoizes its result via `_aovBindingsNeedValidation` /
`_aovBindingsValid` (`aovOutput.cpp:69-74`), but a buffer's dimensions or format
can change while the pointer and binding vector do not, so the cached verdict
can go stale. AOV counts are tiny; remove both members and validate on every
setup. (Confirm `SetAovBindings` is the only writer of the flag before deleting
it.)

## Convergence policy (decided, not open)

On validation failure, apply exactly this and nothing more:

- mark every **non-null** binding converged (skip null / wrong-type);
- emit the specific per-binding diagnostic that validation already produces;
- return without tracing.

Remove the generic `TF_WARN("Could not validate Aovs …")` (`renderer.cpp:431`);
`_ValidateAovBindings` already warns with the precise reason, so the generic
line is a duplicate.

## Failure-time metrics

`_renderStartTime` is currently set *after* `_PreRenderSetup()`
(`renderer.cpp:492`), so a failed setup would report freshly-zeroed counters
against a previous render's start time. Move the timer to `Render()` entry (as
in the skeleton above) so a failed invocation reports a coherent, near-zero
elapsed time.

## Follow-up: the convergence-vs-failure ambiguity (separate plan)

Marking buffers "converged" is how a failed render stops Hydra from spinning —
but convergence is also what drives offline output, so a failed setup under
`usdrender` could write a **stale or blank product and exit as if it
succeeded**. Making the render pass suppress product output on a failed frame
requires a renderer→render-pass failure state and is a separate, larger change,
owned by **[03-plan-render-failure-signal.md](03-plan-render-failure-signal.md)**.
This plan only makes the failure safe and terminal; `03` makes it
distinguishable from genuine convergence.

## Implementation sequence

1. Add the focused tests below (they should fail on current code).
2. Make `MarkAovBuffersUnconverged()` and the convergence loops null/type-safe.
3. Fold the interface-type check into `_ValidateAovBindings`; delete the
   validation cache.
4. Reorder `_PreRenderSetup()` to validate-before-commit/map, change it to
   `bool`, and add the early `return` in `Render()`.
5. Promote size / data-window / empty-binding / null-scene problems to `false`
   returns; fix the 0×0 containment gap.
6. Move `_renderStartTime` to `Render()` entry.
7. Confirm a successful setup builds exactly the same adaptive and AOV state.
8. Document the declaration contract on `renderer.h` (inputs, success
   postconditions, failure effects, convergence behavior). This declaration
   contract lives here; `21-plan-api-contracts.md` should not duplicate it.

## Tests to add

Keep the suite focused:

- null binding — including the pre-render `MarkAovBuffersUnconverged()` path;
- non-null buffer of the wrong implementation type;
- known AOV with an unsupported format;
- mismatched buffer dimensions;
- out-of-bounds and zero-sized data-window cases;
- one successful render proving every `Map()` has exactly one unmap;
- assert setup failures perform zero maps and zero sample passes.

After the tests are written, run the repository's required adversarial
test-review agent over them.

## Validation

- New failure tests complete without crash, render work, or unbalanced
  map/unmap. Instrument a test buffer with map/unmap counters and assert they
  match on success and are zero on each failure path.
- Existing AOV, render-buffer, and render-settings tests pass.
- A fixed-seed render is bit-identical before/after on the success path.

## Completion criteria

- `Render()` has an explicit setup-success branch; no failure path reaches tile
  rendering or normal finalization.
- No null or wrong-type buffer is dereferenced anywhere, including
  `MarkAovBuffersUnconverged()`.
- All validation precedes commit/map; no partial-map cleanup exists.
- The validation cache is gone.
- Convergence policy and the deferred stale-product risk are documented.
- The declaration states inputs, success postconditions, failure effects, and
  convergence behavior.

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
