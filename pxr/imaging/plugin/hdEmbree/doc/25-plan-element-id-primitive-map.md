# Plan: Correct the elementId primitive-to-face map

Status: correctness follow-up discovered by the plan-21 contract review.

## Issue

`PrototypeContext::primitiveParams` is assigned `_trianglePrimitiveParams` only
when `_refined` is true. `_trianglePrimitiveParams` is produced while creating
the non-refined triangulated mesh, not the refined subdivision mesh. The current
ternary is inverted:

- non-refined triangle hits receive an empty table and fall back to raw Embree
  triangle IDs instead of authored coarse-face IDs;
- refined hits can receive empty or stale triangle-indexed data even though
  Embree reports subdivision coarse-face primitive IDs.

Consequently no truthful general “indexed by Embree primitive ID” contract can
be stated, and refined elementId lookup may index incompatible storage.

## Goals

- Return authored coarse-face element IDs for both triangle and subdivision
  meshes.
- Never index triangle-derived data with subdivision primitive IDs.
- State the exact table/index relationship at `PrototypeContext`.

## Implementation

1. Add focused elementId AOV tests for a triangulated n-gon and a refined
   subdivision mesh with multiple coarse faces.
2. For triangle meshes, retain the triangle-to-coarse-face primitive params and
   index them by Embree triangle `primID`.
3. For refined meshes, use Embree's coarse-face `primID` directly or provide a
   separately named table only if topology requires remapping.
4. Make bounds/failure behavior explicit so malformed hit data cannot index
   outside the selected table.
5. Replace the temporary plan-21 field comment with the settled invariant.
6. Update `AGENTS.md` and `ARCHITECTURE.md` if this establishes a durable AOV
   mapping rule not already documented.

## Validation

- Run the focused elementId tests for triangle and refined meshes.
- Run `pixi run build`.
- Run the complete Typhoon suite.
- Launch an adversarial test-review agent after the tests are written.

## Mandatory final suite gate

```sh
cd /path/to/typhoon-test-suite
powerprofilesctl launch --profile performance -- pixi run pytest --renderer typhoon-local
```

Run all tests to completion, report elapsed time, compare it with a relevant
baseline, and investigate regressions. Do not commit until your human has reviewed
and explicitly approved the change.
