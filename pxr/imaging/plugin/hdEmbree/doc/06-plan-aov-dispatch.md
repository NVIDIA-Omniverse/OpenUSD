# Plan: Replace the AOV function-pointer dispatch table with a direct switch

Status: readability cleanup from the full-codebase review against
`pxr/imaging/plugin/hdEmbree/AGENTS.md`. Targets **Goal 1** (don't add an
abstraction unless it nets a code-size reduction of at least twice what it adds;
avoid fancy C++-isms) and **Goal 2** (prefer linear flow; don't force the
reader to jump around).

Depends on `02-plan-render-setup-failure.md` and the core names from
`05-plan-naming-core.md`.

Two independent reviewers flagged this as the densest unjustified abstraction
in the reference renderer.

## The problem

The AOV write path is routed through a precomputed C-style function-pointer
table:

- `renderer/renderer.h:1016-1190` declares an `_AovWriteFn` typedef, an
  `_AovWriter` struct (`{buffer, writeFn, token}`), a `_BuildAovDispatchTable()`
  builder, and **nine** static writer functions — `_WriteColor`,
  `_WriteColorHeatmap`, `_WriteDepth`, `_WriteClipDepth`, `_WriteId`,
  `_WriteNormal`, `_WriteNormalEye`, `_WritePrimvar`, `_WriteAdaptiveHeatmap` —
  each with the identical signature `(HdEmbreeRenderer* self, _AovWriter const&
  writer, RTCRayHit const& rayHit, GfVec4f const& color, unsigned int x,
  unsigned int y)` and a full doxygen block. That is ~175 header lines.
- `_UpdateVariance` (`renderer.h:1187-1190`) is a tenth static that also takes
  `self` and is called as `_UpdateVariance(this, x, y, rgb)`
  (`renderer/renderer.cpp:781`). It is not part of the table, but it is the same
  pattern and is cleaned up here.
- The table is built by `_BuildAovDispatchTable()`, defined in
  `renderer/aov/aovOutput.cpp:316-371` and called once per frame from
  `_PreRenderSetup` (`renderer/renderer.cpp:478`). It is consumed by the loop in
  `_EvaluatePixelSample` (`renderer/renderer.cpp:784`):

  ```cpp
  for (const auto& writer : _aovWriters) {
      if (!writer.buffer->IsConverged()) {
          writer.writeFn(this, writer, result.primaryHit, result.color, x, y);
      }
  }
  ```
- The nine writer definitions live in `renderer/aov/aovOutput.cpp:394-504`,
  several of which ignore half their parameters (e.g. `_WriteColorHeatmap` at
  aovOutput.cpp:407 ignores `rayHit` and `color`).

Why this fails the goals:

- Every writer takes `self` and immediately casts it back to the renderer,
  defeating the purpose of a member function — the indirection buys nothing.
- To learn what a color/depth/normal write actually does, the reader must jump
  from the loop through a stored function pointer into one of nine writers in a
  different file. That is the opposite of linear flow.
- The per-writer *logic* is real and is not being deleted — it moves into the
  switch. What fails the 2x-reduction test is the machinery wrapped around it:
  the typedef, the `_AovWriter` struct, the uniform six-parameter callback
  signature repeated nine times (half of whose parameters each writer ignores),
  the nine header declarations, and nine doxygen blocks that redundantly
  document the same `self`/`writer`/`x`/`y` contract. That scaffolding is far
  larger than the handful of statements it exists to route to.

## Behavior that must be preserved exactly

The writers are not trampolines. They carry policy that has to survive the move
into the switch. Every item below is currently load-bearing:

| Concern | Current behavior |
| --- | --- |
| Camera exposure | Applied to RGB (not alpha) for ordinary color only (`aovOutput.cpp:400-403`). The two heatmaps and every geometric AOV do not apply it. |
| Color heatmap vs dedicated heatmap | `color`-replacing heatmap uses `WriteOutput` with `_pixelSampleCount[idx]` (`aovOutput.cpp:414-417`); the dedicated `adaptiveHeatmap` AOV uses `Write` with `_pixelSampleCount[idx] + 1` (`aovOutput.cpp:500-503`). Different function *and* different value. |
| ID misses | `_WriteId` writes `-1` when `_ComputeId` fails (`aovOutput.cpp:451-454`), so the pixel is always written. |
| Other misses | Depth, normal, and primvar writers write nothing on failure, leaving the cleared buffer value in place. |
| `cameraDepth` vs `depth` | `cameraDepth` is ray distance (`_ComputeDepth(..., clip=false)` → `rayHit.ray.tfar`); `depth` is projected/clip depth (`clip=true`). The current writer names invert the reader's expectation; see naming below. |
| Write coordinate | All writes use `GfVec3i(x, y, 1)` — the render-buffer `Write`/`WriteOutput` signature takes a `GfVec3i` pixel, not separate `x`/`y` (`renderer/renderBuffer.h:32-37`). |
| Converged skip | Per-AOV `IsConverged()` check before writing, evaluated per sample. |

## Suggested change

Replace the table with a direct switch. The renderer already owns the real work
in member helpers `_ComputeDepth`, `_ComputeId`, `_ComputeNormal`, and
`_ComputePrimvar` (`renderer/aov/aovOutput.cpp`), plus `_HeatmapColor`, which is
retained as-is.

Classify each validated AOV once into a small enum named after the AOV tokens,
so the `cameraDepth`/`depth` distinction is visible at the call site:

```cpp
enum class _AovKind {
    Color,
    ColorAdaptiveHeatmap,  // color AOV replaced by the sample-count heatmap
    CameraDepth,           // ray distance
    Depth,                 // projected clip depth
    Id,
    Normal,
    EyeNormal,
    Primvar,
    AdaptiveHeatmap        // dedicated heatmap AOV
};

struct _AovOutput {
    HdEmbreeRenderBufferInterface* buffer;
    _AovKind kind;   // no default: see below
    TfToken token;   // read only by Id and Primvar
};
```

Deliberately give `kind` no default initializer. A default of `Color` would mean
a classifier branch that forgot to set it silently turns that AOV into a color
write — writing exposure-scaled RGBA into, say, an int32 ID buffer. With no
default, every construction site must be an aggregate initializer naming the
kind explicitly, and a missing one is a compile error rather than a rendering
bug. (`buffer` keeps its `nullptr` default only if that stays consistent with
aggregate initialization; simplest is to drop both defaults and always brace-init
all three fields.)

The write loop in `_EvaluatePixelSample` stays short, and the switch stays in
`aovOutput.cpp` so the `renderer.* = orchestration` / `aov/ = behavior`
boundary documented in `AGENTS.md:198-201` holds:

```cpp
for (_AovOutput const& aov : _aovOutputs) {
    if (!aov.buffer->IsConverged()) {
        _WriteAov(aov, result.primaryHit, result.color, x, y);
    }
}
```

`_WriteAov` is an ordinary non-static member holding the switch, with one case
per kind calling the existing `_ComputeX` member directly. Illustrative cases,
matching the real signatures:

```cpp
case _AovKind::Color: {
    GfVec4f exposed = color;
    exposed[0] *= _cameraExposureScale;
    exposed[1] *= _cameraExposureScale;
    exposed[2] *= _cameraExposureScale;
    aov.buffer->Write(GfVec3i(x, y, 1), 4, exposed.data());
    break;
}
case _AovKind::CameraDepth: {
    float depth;
    if (_ComputeDepth(rayHit, &depth, /*clip=*/false)) {
        aov.buffer->Write(GfVec3i(x, y, 1), 1, &depth);
    }
    break;
}
case _AovKind::Id: {
    int32_t id;
    if (!_ComputeId(rayHit, aov.token, &id)) {
        id = -1;
    }
    aov.buffer->Write(GfVec3i(x, y, 1), 1, &id);
    break;
}
```

Write the switch with a case for every enumerator and **no `default:` label**, so
that adding a kind later without handling it produces a `-Wswitch` warning
instead of silently falling through to a no-op. There is nothing for a default
to do anyway: unclassifiable AOVs never enter `_aovOutputs` in the first place
(see requirement 4 below).

This deletes:

- the `_AovWriteFn` typedef and `_AovWriter` struct (renderer.h:1018-1032),
- the nine static `_WriteX` writers and their doxygen (renderer.h:1039-1172,
  aovOutput.cpp:394-504),
- the `self`-passed-to-static pattern, once `_UpdateVariance` also becomes an
  ordinary member called as `_UpdateVariance(x, y, rgb)`.

`_BuildAovDispatchTable` (defined only in `aovOutput.cpp:316`) is replaced by a
classification function in the same file; the call site at `renderer.cpp:478` is
retargeted to it. `_aovWriters` (renderer.h:1319) becomes `_aovOutputs`.

## Classification requirements

The replacement for `_BuildAovDispatchTable` must keep all of the following.
Several are easy to lose because they are side effects of a function whose name
only mentions the table:

1. **`_needColor` and `_colorClearValue`.** Both are computed in
   `_BuildAovDispatchTable` (`aovOutput.cpp:319-330`), not by the caller.
   `_needColor` starts as `_enableAdaptiveSampling`, becomes true if any `color`
   AOV is bound, and gates whether the integrator runs at all
   (`renderer.cpp:763`). `_colorClearValue` is taken from the first `color`
   binding and is read by the integrators
   (`renderer/integrator/lighting.cpp:33`, `unlitIntegrator.cpp:38`,
   `surfaceShading.cpp:222`). Losing either silently changes what gets rendered,
   not just where it is written.
2. **Call ordering inside `_PreRenderSetup`.** The color-vs-heatmap choice reads
   `!_pixelSampleCount.empty()` (`aovOutput.cpp:341`), and that vector is sized
   immediately above at `renderer.cpp:470-476`. Classification must stay after
   the adaptive-array allocation.
3. **Unsupported AOVs are warned about in `_ValidateAovBindings` and then simply
   omitted** from the vector — an unrecognized token produces no entry, not a
   default case.
4. **Format gating.** The current builder re-checks `GetFormat()` per kind
   (`Float32` for depth, `Int32` for ids, `Float32Vec3` for normals and
   primvars) even though `_ValidateAovBindings` already warns and invalidates on
   mismatch. Keep the re-check. It is defensive duplication, but removing it
   changes behavior for any path that reaches setup with stale validation state,
   and that is not this plan's business to decide.
5. **Color has no format re-check** in the builder — it relies on validation
   alone. Keep that asymmetry rather than "fixing" it here.
6. **Dedicated `adaptiveHeatmap` is omitted entirely** when
   `_enableAdaptiveSampling` is false or `_pixelSampleCount` is empty
   (`aovOutput.cpp:364-368`) — it is not written with a fallback value.
7. **Color is replaced by its heatmap variant** only when `_showAdaptiveHeatmap
   && _enableAdaptiveSampling && !_pixelSampleCount.empty()`.
8. **Borrowed mapped-buffer lifetime.** The stored `HdEmbreeRenderBufferInterface*`
   are borrowed from buffers mapped earlier in `_PreRenderSetup`
   (`renderer.cpp:447-448`) and remain valid only until the next setup. Keep the
   comment that says so.

## Related cleanup: deduplicate the triplicated hit-context lookup

`_ComputeId`, `_ComputeNormal`, and `_ComputePrimvar` each repeat the same
instance/prototype-context retrieval verbatim (`renderer/aov/aovOutput.cpp:
541-552, 607-618, 646-657`), including the identical comment "We don't use
embree's multi-level instancing". This plan is already editing all three; fold
the extraction in rather than leaving it as separate churn on the same file.

The lookup dereferences `_scene` (aovOutput.cpp:546), so a `static` free function
taking only `rayHit` will not do. Use a file-local helper in `aovOutput.cpp` that
accepts the scene — this avoids adding another declaration to the already large
`renderer.h`:

```cpp
// Returns false if the hit has no valid instance/prototype context.
// Outputs are written only on success.
bool _GetHitContexts(RTCScene scene, RTCRayHit const& rayHit,
                     HdEmbreeInstanceContext const** instanceContext,
                     HdEmbreePrototypeContext const** prototypeContext);
```

If the helper promises `false` for invalid contexts, it must check the whole
chain, not just the IDs. Return `false` on any of:

- either output pointer is null;
- `rayHit.hit.geomID == RTC_INVALID_GEOMETRY_ID`;
- `rayHit.hit.instID[0] == RTC_INVALID_GEOMETRY_ID`;
- `rtcGetGeometry(scene, rayHit.hit.instID[0])` returns null;
- its user data (the `HdEmbreeInstanceContext`) is null;
- `instanceContext->rootScene` is null;
- `rtcGetGeometry(instanceContext->rootScene, rayHit.hit.geomID)` returns null;
- its user data (the `HdEmbreePrototypeContext`) is null.

Neither output is written unless every check passes, so a caller that ignores the
return value cannot read a partially-populated pair.

The `instID[0]` check matters and is worth stating precisely, because it is
tempting to assume the existing light-hit rejection already covers it. It does
not. `_GetLightGeometryHit` (`renderer/integrator/visibility.cpp:275-282`)
returns `nullptr` whenever `instID[0] != RTC_INVALID_GEOMETRY_ID`, so it only
filters *un-instanced* geometry that is registered as a light. A top-level
un-instanced hit that is not a registered light passes the existing
`!_GetLightGeometryHit(rayHit)` guard and then calls
`rtcGetGeometry(_scene, RTC_INVALID_GEOMETRY_ID)`. The real invariant is "all
non-light geometry is instanced", which holds today but is not enforced by that
guard. Having the helper return `false` turns a latent undefined-behavior path
into a clean miss at all three call sites.

**This part is not strictly behavior-preserving**, and that is intentional. For
any scene satisfying the invariant — every scene the renderer produces today —
behavior is identical. For a scene that violates it, the old code has undefined
behavior and the new code writes the miss value. That is a deliberate defensive
correctness improvement, not a pure refactor, which is why it is confined to
commit 2 and called out here rather than folded silently into the dispatch work.

The three callers keep their own `geomID` and `_GetLightGeometryHit` checks
(their miss semantics differ — see the preservation table) and add an early-out
on the helper's return.

## Sequencing

Two commits, one change. No runtime-contract changes. Commit 1 is behavior-preserving;
commit 2 is behavior-preserving for all valid scenes and hardens one invalid one.

**Commit 1 — dispatch:**

1. Add `_AovKind` and `_AovOutput`; replace `_BuildAovDispatchTable` with the
   classification function, carrying over every item in *Classification
   requirements*.
2. Add `_WriteAov` in `aovOutput.cpp` with one case per kind, transcribing the
   *Behavior that must be preserved exactly* table case by case.
3. Replace the loop at `renderer.cpp:784` with the two-line version.
4. Delete the typedef, struct, and nine static writers from `renderer.h` and
   `aovOutput.cpp`; rename `_aovWriters` to `_aovOutputs`.
5. Convert `_UpdateVariance` to a non-static member; update `renderer.cpp:781`.

**Commit 2 — hit context (deduplication plus defensive hardening):**

6. Add `_GetHitContexts` and call it from the three `_ComputeX` members. Note in
   the commit message that this narrows a latent invalid-dereference path to a
   clean miss; it is not a pure refactor.

## Validation

Commit 1 is behavior-preserving by construction, so validation is about catching
transcription slips in the switch — particularly the heatmap
`Write`/`WriteOutput` and `count`/`count + 1` distinctions, which no existing
test would notice.

**Existing suite (necessary, not sufficient).**
`pixi run ctest --test-dir build -R testHdEmbree --output-on-failure`.
`testenv/testHdEmbree.cpp:493` hard-rejects any AOV other than `color`,
`cameraDepth`, and `primId`, so the existing suite exercises three of the nine
kinds.

**Expand the existing single-AOV test matrix.** Keep
`testenv/testHdEmbree.cpp` structurally single-binding: it pushes one binding at
`testHdEmbree.cpp:199`, then renders and reads back only that AOV. Extend its
allow-list and format-specific readback so separate invocations cover:

- `color`;
- `cameraDepth` and `depth`;
- `primId`, `elementId`, and `instanceId`;
- `normal` and `Neye`;
- one known primvar;
- `adaptiveHeatmap`.

Use `usdrender -s "{settings}.ty:randomNumberSeed = 1"`, a non-unit camera
exposure, and background misses. Add a separate color invocation with
`-s "{settings}.ty:showAdaptiveHeatmap = true"` for the
`ColorAdaptiveHeatmap` case. Compare every invocation before/after; any
non-zero difference is a bug because the switch move stays within the same
translation unit.

Per project instructions, run an adversarial test-review agent over the
expanded matrix.

**Hot-path sanity check.** `_WriteAov` runs per sample, so record wall-clock
renderer time for one representative color invocation before and after and
confirm no material regression. A single before/after timing pair is enough;
only profile if it moves.

**Inspected, not image-tested.** Converged-buffer skipping; call it out in
review.

## Documentation

Update the AOV wording that describes the table:

- `AGENTS.md:198` ("integrator selection/AOV dispatch") and `AGENTS.md:200-201`
  ("...and writer dispatch").
- `ARCHITECTURE.md:54-55`, `458-464` ("dispatches the prebuilt AOV writers"),
  `483`, and `494` (which names `_WriteColor()` explicitly).

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
