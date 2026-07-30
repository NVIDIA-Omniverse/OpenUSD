# Plan: Correct the elementId primitive-to-face map

Status: correctness follow-up discovered by the plan-21 contract review.

This plan is **not behavior-preserving**. It changes `elementId` AOV values for
unrefined meshes, which is the point of the fix.

## Issue

`PrototypeContext::primitiveParams` takes `_trianglePrimitiveParams` when
`_refined` is true. The condition is inverted, inherited from the upstream
commit that added these AOVs (`9857db2cb`).

Only `_CreateEmbreeTriangleMesh()` ever produces that table, on the unrefined
path, and `HdMeshUtil::ComputeTriangleIndices()` skips hole faces and splits
n-gons, so an unrefined `primID` needs the remap. `_CreateEmbreeSubdivMesh()`
binds the authored `faceVertexCounts` as the Embree FACE buffer and puts holes
in a separate HOLE buffer, neither of which renumbers faces, so a refined
`primID` already *is* the authored face index and never needs a table. The rest
of the renderer already relies on that invariant: `faceVertexOffsets` is
primID-indexed, `ComputeDisplacedPosition()` indexes `faceVertexCounts` by
`primID`, and adaptive subdivision feeds authored face indices in as primIDs.
`_CreatePrimvarSampler()` applies the correct `!refined` condition to uniform
primvars a few hundred lines away in the same file.

Three situations produce wrong output.

- **Unrefined meshes report a triangle index.** The triangle path is taken when
  `subdivisionScheme` is `none`, when the display style refine level is zero
  (usdview complexity `low`), or for an invalid geom style. Hull and points geom
  styles do *not* land here: `_GeomStyleShouldHonorRefineLevel()` returns true
  for them. The reported ID is wrong whenever triangulation renumbers, which
  needs a face with more than three vertices, a hole face, or a face with fewer
  than three vertices. A quad mesh is enough: authored face *N* reports 2*N* and
  2*N*+1. A pure-triangle, hole-free mesh is unaffected because the
  triangulation writes an identity table; orientation alone does not matter,
  since the decode strips the edge flag and returns the authored face either
  way.
- **Refined meshes are correct only by accident and only in Sync order.** A mesh
  refined at its first Sync never ran `ComputeTriangleIndices()`, so the table is
  empty and the raw-`primID` fallback happens to be right.
- **A refinement toggle reads a stale table, possibly out of bounds.** Raising
  the refine level on a live prim rebuilds the geometry and the prototype
  context but nothing clears `_trianglePrimitiveParams`, so the refined context
  decodes coarse-face primIDs through a triangle table. `_ComputeId()` indexes it
  unguarded, unlike `UniformSampler::Sample()`. Three faces with two holes give a
  one-entry table and hits at `primID == 2`. Once refined, the table is frozen,
  so any later topology growth reopens the same hole.

The root cause is that "which domain indexes this table" is decided in three
places that can disagree: the elementId assignment, the uniform sampler, and the
lifetime of the triangle caches themselves. The fix removes the decision rather
than synchronizing it.

## Goals

- Return authored coarse-face element IDs for both triangle and subdivision
  meshes.
- Make `elementId` for a given face identical whether the mesh renders refined
  or unrefined.
- Leave exactly one producer-side rule — triangle caches are non-empty only
  while the live Embree geometry is a triangle mesh — and no consumer-side mode
  logic at all.

## Non-goals

- No second, subdivision-side remap table. The topology never needs one.
- No shared helper for the two bounds checks. A declaration, doc comment and
  definition would cost more than the three lines it removes at one of the two
  sites, and the two arrays are different objects.
- No general audit of `_PopulateRtMesh` lifetime, which plan 13 owns. This plan
  settles only the triangle caches, and only because their emptiness becomes
  load-bearing.

## Implementation

The production change deletes three conditionals and one constructor overload
and adds one bounds branch and one clearing block.

1. Clear the triangle caches on the refined path, at the `doRefine` dispatch in
   `_PopulateRtMesh()` where the discriminator is already visible, immediately
   before the `_CreateEmbreeSubdivMesh()`/`_CreateEmbreeTriangleMesh()` call:
   `_triangulatedIndices`, `_trianglePrimitiveParams`, `_triangleDPdu` and
   `_triangleDPdv`. Comment it with the rule the emptiness now carries, not with
   what `clear()` does.

   All four are safe to clear there. Live samplers hold their own COW copies, the
   rebuild runs with rendering stopped, and the failure path drops the geometry
   and the context so nothing can sample a cleared array. `_triangleDPdu` and
   `_triangleDPdv` are included because the derivative and tangent-frame updaters
   are gated on `!_refined` and therefore never clear them on a toggle today; a
   comment claiming the caches are empty when refined would otherwise be false.

2. Assign `primitiveParams` unconditionally from `_trianglePrimitiveParams` and
   rename the field to `trianglePrimitiveParams`, placed next to `triangleDPdu`
   and `triangleDPdv`, which already name their domain. The name is what tells a
   reader at the consumer what indexes the array; that is cheaper than the
   paragraph it replaces and cheaper than a wrapper type or a mode enum, which
   would only re-express what one field's emptiness already says.

3. Drop the `if (refined)` in `_CreatePrimvarSampler()`'s uniform case and always
   pass `_trianglePrimitiveParams`, then delete the now-unused two-argument
   `UniformSampler` constructor. `UniformSampler::Sample()` already implements the
   empty-means-identity rule, so nothing else changes.

4. Bound the `elementId` branch of `_ComputeId()` the way
   `UniformSampler::Sample()` does: an empty table means the primID is already
   the authored face index; a primID at or past the table's end returns false,
   which yields the documented miss value. After step 1 this is unreachable by
   construction; keep it anyway, for the same reason the sampler has one — it
   turns a future stale-cache mistake into an AOV miss instead of a threaded heap
   over-read.

5. Write the invariant where the next person to break it will read it. `mesh.h`
   already documents `_trianglePrimitiveParams` as a triangle-to-authored-face
   mapping; extend that block with the lifetime rule, because the realistic
   regression is someone populating `_triangulatedIndices` for a refined mesh —
   `_ComputeTriangulatedCornerIds()` already builds triangulated corner data from
   topology regardless of refinement, for wireframe. Then restate the consumer
   side at `PrototypeContext`, replacing the plan-21 placeholder, which is not
   wrong but is too vague to say when the table is present. Fix the stale
   `UniformSampler` class comment, which calls the index "the face index embree
   reports"; it is a triangle index in the only case where the table is
   non-empty.

   Two encodings of "is this a triangle cage" survive this change:
   `PrototypeContext::refined`, which surface shading branches on to pick its
   derivative path, and the emptiness of the triangle caches. They cannot be
   collapsed, because `UniformSampler` holds only the array and cannot see the
   flag. Say so at the field, so the next reader does not try.

6. Document the user-visible semantics in `README.md`, whose AOV table gives the
   `elementId` format but not its meaning: for meshes it is the face index in the
   mesh Hydra received, unchanged by complexity or refinement. Do not write
   "authored face index" without qualification — the implicit-surface scene index
   plugin converts sphere, cube, cone, cylinder, capsule and plane gprims into
   meshes, and those faces are generated, not authored.

   `AGENTS.md` and `ARCHITECTURE.md` need no change; step 5 states the rule at
   the code that owns it.

## Testing

The defect is in the delegate-side assignment, not the renderer-side lookup. A
test that builds a raw Embree scene and drives `ty::Renderer` directly — the
pattern used by most of `testHdEmbreeRenderSetup.cpp` — exercises only the
consumption contract and passes both before and after the fix. The tests must
drive `HdEmbreeMesh` through a real render index.

Extend `testenv/testHdEmbreeRenderSetup.cpp`; it already builds a render index
with `HdUnitTestDelegate` and is registered with CTest. Do not add a test target.
Bind an `elementId` buffer, disable camera jitter, and assert at a pixel whose
whole footprint lies inside one face, since ID AOVs are last-writer-wins per
sample.

Case selection matters more than usual:

- **Unrefined quad or n-gon.** Both triangles of authored face *N* report *N*.
  Fails today. Do not build this case from a triangulated mesh: the
  triangulation writes an identity table there and the test would pass against
  the broken code.
- **Unrefined mesh with hole faces.** Triangulation skips holes, so triangle and
  face indices diverge by more than a constant and no off-by-one-tolerant
  assertion can pass by accident. `HdUnitTestDelegate::AddMesh` has hole-taking
  overloads, but they require explicit color and opacity arguments — not a
  drop-in for the existing call in that file.
- **Refined mesh with several coarse faces.** Needs a refine level above zero and
  a scheme other than `none`; assert the coarse face index.
- **Refinement toggle on one prim.** Render unrefined, raise the refine level,
  render again, assert the same face index both times. This is the only case
  covering the stale-table read, and it **cannot** be written against the manual
  `mesh->Sync()` harness in that file: `SetRefineLevel()` only marks
  `DirtyDisplayStyle` on the change tracker, which a hand-driven Sync never
  consults, so the second render would re-render identical geometry and the
  assertion would pass vacuously. Use the render pass plus
  `renderIndex->SyncAll()` pattern from `testenv/testHdEmbreeWireframe.cpp`.

## Validation

- Run `pixi run build`.
- Run
  `pixi run ctest --test-dir build -R testHdEmbreeRenderSetup --output-on-failure`.
- Run `pixi run ctest --test-dir build -R testHdEmbreeSubdivision --output-on-failure`
  and the wireframe test, which share the triangle caches this plan re-scopes.
- Run the complete Typhoon suite as a no-regression check.
- Launch an adversarial test-review agent after the tests are written.

The full suite cannot verify this fix. It compares color AOVs through
`usdrender` and never binds `elementId`, so a green run only shows that clearing
the triangle caches and deleting the sampler branch did not disturb shading,
primvar interpolation, tangents or wireframe — which is the real regression risk
of this change and worth the run. The focused tests are the only coverage of the
fix itself. Add a follow-up `TODO.md` entry for a permanent `elementId` case in
the external suite rather than growing this plan; the suite is image-diff based
and needs a new comparison mode for integer AOVs.

No performance dimension: one bounds comparison per ID sample, and four
`clear()` calls per geometry rebuild.

## Mandatory final suite gate

```sh
cd /path/to/typhoon-test-suite
powerprofilesctl launch --profile performance -- pixi run pytest --renderer typhoon-local
```

Run all tests to completion, report elapsed time, compare it with a relevant
baseline, and investigate regressions. Do not commit until your human has reviewed
and explicitly approved the change.
