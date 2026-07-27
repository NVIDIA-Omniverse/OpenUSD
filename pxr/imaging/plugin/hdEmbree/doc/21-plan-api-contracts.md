# Plan: Complete function and data contracts at renderer boundaries

Status: documentation-only cleanup over the settled post-plan-20 tree.
Individual comment defects remain in `09-plan-comments-deadcode.md`; contracts
for declarations created or changed by earlier plans stay owned by those plans.

Depends on 02, 03, 04, 05's core naming work, 12, 13, 14, 15, 17, 18, 19, and
20. Run after 20 has established the final identifiers.

## Issue

Contract quality is uneven. Several cross-file interfaces still require reading
their definitions to discover ownership, lifetime, coordinate space, units,
failure results, or thread restrictions. Earlier plans fix the behavior and
write contracts for the declarations they own; this plan fills only the
remaining documentation gaps against the final tree.

This plan does not fix behavior under cover of documentation. In particular,
adding aggregate defaults, clamping invalid light dimensions, or changing a
failure result is a semantic change that requires its own plan and focused
tests.

## Goals

- Every remaining cross-file function states relevant input invariants,
  outputs, side effects, ownership/lifetime, and failure behavior.
- Result and data structs state their validity invariant and the meaning of
  every field.
- Direction, coordinate-space, units, PDF measure, normalization, and thread
  mutation conventions are stated once at the relevant type boundary.
- Comments describe WHAT and WHY, not line-by-line mechanics.
- No declaration contract duplicates an authoritative contract written by its
  owning earlier plan.

## Non-goals

- No code behavior changes.
- No new member initializers, field reordering, clamping, fallback, or
  validation.
- No tests whose only purpose is to encode existing implementation behavior.
- Do not add documentation to obvious private one-line helpers.
- Do not duplicate algorithms from definitions into headers.

If this sweep discovers that the implementation cannot satisfy a useful
contract, stop and open a separate behavior plan with focused tests. Do not
weaken the contract into vague prose and do not fix the behavior in this
commit.

## Ownership and de-duplication

The following contracts are authoritative in their owning plans and are
excluded except for consistency review:

| Contract | Owner |
| --- | --- |
| `_PreRenderSetup()` validation, map/unmap, and failure effects | 02 |
| Atomic frame validity, valid-convergence product gate, and missing-product `usdrender` failure | 03 |
| `EvalGraph` compile tri-state, diagnostics, and evaluation failure behavior | 04 |
| Render-settings resolution/application boundary | 12 |
| `_PopulateRtMesh()` ownership and failure postconditions | 13 |
| Geomprop handle-space, resolved-table lifetime, and refresh invariants | 15 |
| Extracted renderer and BSDF module declarations | 14 and 17 |
| Per-light-type `Sample*` / `Evaluate*` declarations | 18 |
| Namespace ownership and TU-containment rules | 19 |

When an owning plan already wrote a complete declaration contract, this plan
removes stale duplicate prose rather than adding a second version.

## Workstreams

### Light sampling data

`LightSample` remains in scope because 18 owns the per-type functions, not
their shared result type. Document the post-05 field names and:

- `omegaInWld` orientation, world space, and normalization;
- the incident-radiance quantity and color space of `radianceIn`;
- finite versus infinite-light meaning of `distanceWld`;
- the reciprocal solid-angle convention of `pdfSolidAngleInverse`;
- delta-light interpretation;
- exact validity invariant and which fields callers may read when invalid.

For light shape/data structs, document units, spaces, transforms, and authored
input invariants. Match 18's decided behavior: dimensions and radii are
expected positive and finite, and behavior outside that invariant is
unspecified. Do not claim invalid samples, clamping, or deterministic defaults
that the implementation does not guarantee.

### Geometry and renderer contexts

Document the final post-15 `geometry/context.h` shape:

- raw observing pointers and their owners;
- Embree stable-address requirements;
- transform directions and coordinate spaces;
- parallel-array/vector size relationships;
- geomprop tables' material handle space and refresh lifetime;
- which thread may mutate each field and which scene-edit lock is required.

Cover other cross-file geometry results only where their owning plans did not
already provide the declaration contract.

### Render pass and renderer

Document remaining pass operations against post-03 behavior:

- `_HasConverged()` reports parking/completion state, not frame validity;
- product output requires both convergence and an atomically published valid
  frame;
- `_WriteActiveRenderProducts()` writes authored `productName` paths only for
  offline active RenderSettings/Products and may return without output when
  prerequisites are absent;
- `usdrender` does not write a color-AOV fallback and treats every missing
  expected product as failure;
- render-setting bridge ownership and stale-setting reconciliation;
- stopped-render requirements for mutations.

`_UpdateRenderSettingsFromActiveRenderSettingsPrim()` must state the
bridge-ownership invariant: a delegate value equal to the last bridged value
remains bridge-owned and may be reset when the authored setting disappears.
Add concise WHY comments to the remove-stale then add-new reconciliation if
they remain necessary after plan 12.

### Material synchronization

Review `HdEmbreeMaterial::Sync()` only against the post-04/post-15
implementation. Document:

- dirty-bit and early-return effects;
- independent surface/displacement compile outcomes;
- absent optional displacement versus invalid graph behavior;
- diagnostic ownership established by plan 04;
- material-version publication and why it stops rendering;
- stable material-handle mutation and the later prototype binding refresh.

Do not copy pre-plan-04 blanket-catch behavior into the declaration. Graph
compilation is non-throwing for authored malformed networks after plan 04;
document any remaining narrow external exception boundary exactly where it
still exists.

## Implementation sequence

1. Regenerate the cross-file declaration inventory after plan 20.
2. Mark every declaration already owned by an earlier plan and exclude it.
3. Add the remaining contracts module by module: light data, geometry/context,
   render pass/renderer, then material synchronization.
4. Remove stale HOW comments and duplicate contracts while the authoritative
   declaration is in view.
5. If a truthful contract requires behavior to change, record a follow-up and
   leave code unchanged.
6. Update `AGENTS.md` or `ARCHITECTURE.md` only when this sweep discovers a
   missing durable architectural invariant; do not duplicate declaration prose
   into those documents.

## Validation

- Source diff contains comments only.
- Build `hdEmbree` to catch stale parameter names and malformed doxygen.
- Generate doxygen if the configured build supports it.
- Review declarations without opening definitions: ownership, units/spaces,
  thread restrictions, and failure behavior must be recoverable.
- Search the ownership table's distinctive phrases and confirm there is one
  authoritative declaration, not two competing copies.
- No new test is required for a documentation-only change. If code changes,
  this plan has exceeded scope and must be split before review.

## Completion criteria

- Every important remaining cross-file declaration has a concise truthful
  contract.
- Contracts use the final post-20 names and post-03/04/15/18 behavior.
- `LightSample` and light-shape contracts state conventions and invariants
  without inventing validation or defaults.
- Render-product documentation says "converged and valid", and records that
  `usdrender` treats missing products as errors without a fallback writer.
- No pre-plan-04 blanket graph-compilation catch is documented.
- No source behavior, aggregate layout, initializer, or test changed.

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
