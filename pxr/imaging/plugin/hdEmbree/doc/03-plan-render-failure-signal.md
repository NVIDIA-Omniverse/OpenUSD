# Plan: Distinguish render failure from convergence (suppress stale product output)

Status: correctness follow-up to `02-plan-render-setup-failure.md`. Numbered
`03` because it runs immediately after `02` and depends on it, but is a
genuinely separate, larger change — it adds renderer→render-pass failure state,
whereas `02` only makes the failure safe and terminal. It also removes
`usdrender`'s color-AOV fallback writer: render delegates invoked through
`usdrender` must write their authored RenderProducts themselves.

## Issue

`02` stops a failed render by marking every AOV buffer **converged** (so Hydra's
render thread parks instead of spinning). But convergence is also what triggers
**offline product output**:

```cpp
// delegate/renderPass.cpp:420-432
bool HdEmbreeRenderPass::IsConverged() const {
    const bool converged = _HasConverged();          // true when all buffers IsConverged()
    if (converged) {
        HdEmbreeRenderPass *self = const_cast<HdEmbreeRenderPass *>(this);
        if (!self->_renderProductsWritten) {
            self->_WriteActiveRenderProducts();       // writes files to productName
            self->_renderProductsWritten = true;
        }
    }
    return converged;
}
```

`_HasConverged()` (`renderPass.cpp:401-418`) is satisfied the moment every buffer
reports `IsConverged()`, which is exactly what `02` does on failure. So under
`usdrender` (offline, `enableInteractive = false`), a **failed setup writes a
stale or blank product to `productName` and the command exits as if it
succeeded** — a silent failure that is arguably worse than the original crash.

The root cause is that "converged" conflates two different states: *the render
finished producing valid pixels* and *the render was stopped because it could
not run*. Offline output must only happen for the former.

There is a second path that currently hides the failure. After convergence,
`usdrender` checks whether each expected product exists. For every missing file
it maps or reads back the engine's color AOV and writes that same AOV to the
product path through its private `_Writer` (`renderDriver.cpp:28-35,54`). That
fallback turns hdEmbree's deliberately suppressed product into a stale or
blank file. It also discards the authored RenderProduct/AOV distinction.

## Goals

- Offline product output (`_WriteActiveRenderProducts`) never runs for a frame
  whose renderer setup failed.
- `usdrender` reports a non-success outcome when the render could not run,
  rather than writing a bogus product and exiting 0.
- `usdrender` never manufactures a missing RenderProduct from its color AOV;
  a missing expected product is an error.
- Interactive viewers (usdview) are unaffected — they never write products and
  must keep parking on a stopped render.
- Successful renders write products exactly as today.

## Non-goals

- Do not change the convergence/adaptive-sampling algorithm or what
  "converged" means for a *successful* render.
- Do not add per-AOV partial-failure reporting; one frame-level failure state is
  enough.
- Do not re-open the null/type-safety or setup-ordering work owned by `02`.
- Do not add a new render-pass/engine failure-query API. Once `usdrender` stops
  writing fallback files, its existing missing-product check is the
  client-visible failure signal.

## Proposed design

Add one synchronized frame-level validity signal from the renderer and consult
it at the single product-write gate. Remove `usdrender`'s fallback writer so
the absence of the suppressed product becomes its ordinary non-zero error.

1. **Renderer failure state.** When `_PreRenderSetup()` returns `false` (`02`),
   record it on the renderer. This state crosses threads: `_PreRenderSetup()`
   runs in `HdRenderThread` through `_RenderCallback`, while
   `HdEmbreeRenderPass::IsConverged()` reads it from the client thread. A plain
   `bool` is a data race.

   Use an atomic three-state value:

   ```
   enum class _FrameStatus { Pending, Valid, Failed };
   std::atomic<_FrameStatus> _frameStatus{_FrameStatus::Pending};
   ```

   Set `Pending` synchronously in the render pass immediately before
   `_renderThread->StartRender()`, so a restart cannot observe the previous
   frame's `Valid`. In `Render()`, store `Failed` when setup fails and `Valid`
   after setup succeeds. The render-thread stores use release ordering and the
   accessor uses acquire ordering. Expose
   `DidLastFrameProduceValidPixels()`, which returns true only for `Valid`.
2. **Gate product output on it.** In `HdEmbreeRenderPass::IsConverged()`
   (`renderPass.cpp:421`), write products only when the frame both converged
   **and** is valid:

   ```cpp
   if (converged && _renderer->DidLastFrameProduceValidPixels()) {
       if (!_renderProductsWritten) { _WriteActiveRenderProducts(); ... }
   }
   ```

   Still return `converged` so the render thread parks either way — the only
   change is whether a file is written. Interactive clients are unaffected
   because they never reach `_WriteActiveRenderProducts` (`enableInteractive`
   defaults to true; the write is already gated at `renderPass.cpp:553`).
3. **Remove `usdrender`'s fallback writer.** Delete `_Writer` from
   `usdImaging/bin/usdrender/renderDriver.cpp` and its now-unused Hio, hdSt
   conversion/readback, render-buffer, AOV-texture, and Hgi includes. After the
   render loop, keep only the existing pass over `expected`: if any product
   path does not exist, print `"Missing expected RenderProduct ..."` and return
   false. `usdrender.cpp` already turns that false result into exit code 1.

   This is deliberately renderer-neutral. A renderer that converges without
   writing the authored RenderProducts now fails under `usdrender`; it no
   longer receives a color-only substitute from the client.

## Interaction with existing behavior

- `AGENTS.md` states product output is convergence-driven; this plan adds a
  *validity* gate on top of that, and both `AGENTS.md` and `ARCHITECTURE.md`
  should be updated to say "converged **and** valid."
- The empty-AOV-bindings branch of `_HasConverged()` (`renderPass.cpp:406`,
  which returns `_converged` for the `_colorBuffer`/`_depthBuffer` path) must
  also respect the validity signal, or document why that path cannot fail setup.
- Removing the fallback is a user-visible `usdrender` behavior change for every
  render delegate, not only hdEmbree. The command now means "render the authored
  products", not "write the color AOV when the delegate did not write them."

## Implementation sequence

1. Land `02` first (the `bool` setup return this consumes).
2. Add atomic `_FrameStatus` + accessor on the renderer. Set `Pending`
   immediately before every `StartRender()`, and set `Failed`/`Valid` in
   `Render()` around the `_PreRenderSetup()` branch.
3. Gate `IsConverged()`'s product write on validity.
4. Delete `usdrender`'s `_Writer` fallback and its unused dependencies; retain
   the missing-product error check.
5. Update `AGENTS.md` / `ARCHITECTURE.md` product-output description.
6. Update `usdrender` documentation to state that the selected render delegate
   must write every authored RenderProduct.

## Tests

- `usdrender` with an invalid AOV binding / failed setup: **no product file is
  written**, and the command reports failure (non-zero).
- `usdrender` with a valid scene: product written exactly as before
  (byte-compare against a pre-change baseline).
- A renderer that converges without writing its authored product:
  `usdrender` exits non-zero and does not create a color-AOV substitute.
- usdview-style interactive pass with a failed setup: no crash, no product
  write, render thread parks (unchanged from `02`).
- A converged, valid offline render still writes exactly once
  (`_renderProductsWritten` latch intact).
- Restart a previously valid render and hold it in `Pending`; convergence checks
  must not write the previous frame's product. This exercises the state reset,
  not only the terminal `Valid`/`Failed` values.

## Completion criteria

- No product is written for a frame whose setup failed.
- `usdrender` exits non-zero on setup failure instead of writing a stale product.
- `usdrender` contains no color-AOV fallback writer; every missing expected
  RenderProduct is an error regardless of renderer.
- Frame validity is synchronized across the render and client threads; no plain
  cross-thread validity bool remains.
- Successful offline output is byte-identical to before.
- Interactive parking behavior is unchanged.
- Product-output documentation says "converged **and** valid."

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
