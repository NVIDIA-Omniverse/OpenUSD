# Plan: Publish convergence for anonymous fallback AOVs

Status: implemented 2026-07-30. Correctness follow-up discovered by the
plan-21 contract review.

## Issue

`HdEmbreeRenderPass::_Execute()` preserves the caller's empty AOV binding
vector in `_aovBindings`, then supplies anonymous color and depth buffers only
to the renderer-local binding copy. `_HasConverged()` therefore selects
`_converged`, but that flag is initialized false, reset to false on every
render start, and never set true. It is write-only dead state.

Clients that provide no AOV bindings never observe convergence even after both
anonymous buffers finish. Two consequences follow:

- Interactive clients poll a render pass that never reports completion.
- `IsConverged()` gates `_WriteActiveRenderProducts()`, so an empty-binding
  client never writes a render product at all. The headless output path is
  dead for that client shape.

The data needed to fix this already exists and is already correct. The
renderer marks every one of its own bindings converged when a render finishes,
and also when setup fails, so the anonymous color and depth buffers do carry
accurate convergence. Only the render pass fails to read it.

The comments at the empty-binding branch are plan-21 placeholders describing a
blit path that no longer exists. They are removed by this change.

## Goals

- Make empty caller bindings converge exactly when the anonymous fallback
  buffers converge.
- Preserve explicit-AOV convergence and valid-frame product gating.
- Give convergence one authoritative source instead of a second flag, and
  avoid encoding which fallback buffers exist in a second place.

## Implementation

1. Delete `_converged`. `_HasConverged()` first requires the binding generation
   installed by this pass to remain current in the shared renderer, then reads
   this pass's explicit caller buffers or its anonymous color/depth fallbacks.
   Keep the existing null-`renderBuffer` skip for explicit bindings.

   Fresh anonymous buffers start unconverged, so the local fallback check also
   reports false before first execution without a separate convergence flag.

2. Advance a renderer-owned binding generation on every `SetAovBindings()`,
   including equal-vector replacement. Each pass records the generation it
   installed. This distinguishes simultaneously live passes even when they
   use identical external binding vectors and prevents one pass from combining
   another pass's convergence/frame validity with its own product buffer.

3. Treat a generation mismatch as pass activation. Stop rendering, clear the
   previous binding set, republish the returning pass's settings, camera,
   wireframe, data window, and frozen adaptive-subdivision snapshot, then
   install its caller or anonymous bindings after fallback allocation and
   restart. `ResetAccumulation()` treats the temporary empty binding set as a
   silent no-op; render setup remains the authority that warns about a
   genuinely missing binding.

   The destructor stops and clears bindings only when its generation remains
   current. A non-current pass owns no renderer buffers and must not interrupt
   another live pass's in-flight render.

4. Confirm both empty-binding shapes park correctly.

   - No framing: `_Execute()` allocates `_colorBuffer` and `_depthBuffer` in
     the invalid-framing branch, the render completes, both buffers converge,
     and the pass reports convergence.
   - Valid framing: the fallback buffers are never allocated and stay
     zero-sized, so `_ValidateAovBindings()` fails, the setup-failure path
     parks the usable buffers converged, and
     `DidLastFrameProduceValidPixels()` suppresses the product write.

   Both shapes must reach a settled state. Only the first may write a product.

5. Record the thread-safety invariant this fix relies on. Convergence is read
   from the app thread while the render thread writes it;
   `HdEmbreeRenderBuffer` holds it in an atomic. Binding generations and
   vectors are read and replaced only on the app thread, with rendering
   stopped before replacement.

6. Remove the temporary plan-21 bug comments at the empty-binding branch and
   comment the settled convergence source.

7. Update `AGENTS.md`, `ARCHITECTURE.md`, and user documentation only if the
   fix changes a durable or user-visible invariant.

## Testing

Extend `testenv/testHdEmbreeRenderSetup.cpp`. It already carries the required
harness in the existing `_TestRenderPass*` helpers: render index, delegate,
`HdRenderThread`, a constructed `HdEmbreeRenderPass`, and a 1x1 data window.
It is already registered with CTest. Do not add a new test target.

Add a helper that drives an empty caller AOV binding vector through a real
render callback to completion and asserts `renderPass.IsConverged()`. Set a
viewport rather than a framing so the fallback buffers actually allocate, and
keep the render 1x1 with a low sample count so the test converges in bounded
time.

Assert the pre-execute state as well: convergence must be false before the
first `Execute()`.

Cover the valid-framing empty-binding shape from implementation step 3 in the
same helper or an adjacent one.

Cover two simultaneously live render passes against one shared renderer. Give
them different viewport sizes, alternate execution, and verify each activation
reinstalls the correct anonymous buffers while invalidating the other pass's
convergence. Destroy the non-current pass while the owner is in flight and
verify rendering continues; destroying the current owner must clear its
borrowed bindings. In the subdivision test, alternate two attached-camera
passes and verify each restores its own frozen tessellation snapshot.

## Validation

- Run `pixi run build`.
- Run
  `pixi run ctest --test-dir build -R testHdEmbreeRenderSetup --output-on-failure`.
- Launch an adversarial test-review agent after the test is written.
- Run the complete Typhoon suite as a no-regression check.

The full suite cannot verify this fix. It renders through `usdrender`, which
always supplies AOV bindings, so it never exercises the empty-binding path.
The focused test above is the only real coverage; the suite gate confirms the
`_HasConverged()` rewrite does not regress the explicit-AOV path.

## Mandatory final suite gate

```sh
cd /path/to/typhoon-test-suite
powerprofilesctl launch --profile performance -- pixi run pytest --renderer typhoon-local
```

Run all tests to completion, report elapsed time, compare it with a relevant
baseline, and investigate regressions. Do not commit until your human has
reviewed and explicitly approved the change.
