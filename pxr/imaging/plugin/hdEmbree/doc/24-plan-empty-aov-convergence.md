# Plan: Publish convergence for anonymous fallback AOVs

Status: correctness follow-up discovered by the plan-21 contract review.

## Issue

`HdEmbreeRenderPass::_Execute()` preserves the caller's empty AOV binding vector
in `_aovBindings`, then supplies anonymous color and depth buffers only to the
renderer-local binding copy. `_HasConverged()` therefore selects `_converged`,
but that flag is reset to false on every render start and is never set true.
Clients that provide no AOV bindings never observe convergence even after both
anonymous buffers finish.

The stale comments referred to a blit path that no longer exists. Plan 21 must
document the current failure rather than inventing publication behavior.

## Goals

- Make empty caller bindings converge exactly when both anonymous fallback
  buffers converge.
- Preserve explicit-AOV convergence and valid-frame product gating.
- Give the empty-binding state one authoritative source instead of a second
  flag that can drift from the buffers.

## Implementation

1. Add a focused render-pass test with empty caller AOV bindings that renders
   through completion and asserts convergence.
2. Prefer deleting `_converged` and querying `_colorBuffer` and `_depthBuffer`
   directly when `_aovBindings` is empty.
3. Confirm failed renderer setup still parks usable buffers without publishing
   a valid frame or writing products.
4. Remove the temporary plan-21 bug comments and document the settled
   convergence source.
5. Update `AGENTS.md`, `ARCHITECTURE.md`, and user documentation only if the
   fix changes a durable or user-visible invariant.

## Validation

- Run the new focused empty-binding test.
- Run `pixi run build`.
- Run the complete Typhoon suite.
- Launch an adversarial test-review agent after the test is written.

## Mandatory final suite gate

```sh
cd ~/code/typhoon-test-suite
powerprofilesctl launch --profile performance -- pixi run pytest --renderer typhoon-local
```

Run all tests to completion and report elapsed time. Warn above 250 seconds,
but do not fail on timing alone. Do not commit until Anders has reviewed and
explicitly approved the change.
