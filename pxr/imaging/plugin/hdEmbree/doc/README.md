# hdEmbree (Typhoon) cleanup plans

These `NN-plan-*.md` files are design/cleanup plans produced from a full-codebase
review against the five project goals in `../AGENTS.md` (simple, clear, readable
reference path tracer). Each plan is self-contained: issue, why it violates a
goal, exact `file:line` locations, suggested fix, sequencing, and validation.

Unless noted, every change is meant to be **behavior-preserving** — validate
with `usdrender -s "{settings}.ty:randomNumberSeed = 1"` and
`oiiotool --diff`.

Every plan ends with the same mandatory final gate: from
`/path/to/typhoon-test-suite`, run
`powerprofilesctl launch --profile performance -- pixi run pytest --renderer typhoon-local`.
Always let the complete suite finish. All tests must pass and elapsed time must
be reported. For a performance-sensitive change, compare the same workload
before and after on the same machine and investigate regressions. Do not commit
a plan implementation until your human has reviewed the completed changes and
explicitly approved committing them.

"Behavior-preserving" does not always mean bit-identical. A move *within* a
translation unit (e.g. 16) must produce bit-identical pixels, and any
difference is a bug. A move *across* translation units (17, 18, and the
header extractions in 14) can lose an inlining opportunity and therefore change
floating-point contraction; those plans state an `oiiotool --diff` tolerance
instead and require any non-zero difference to be explained, not assumed.

## How to read the numbers

The `NN-` prefix is the **recommended implementation order**. The plans are
*not* independent: a few hot files (`renderer.h`/`renderer.cpp`,
`rendererImpl.h`, `mesh.cpp`, `aovOutput.cpp`, `displacementEvaluation.cpp`) are
edited by several plans, so those plans must be **serialized and rebased on each
other**, not developed in parallel. The plans may all be implemented on one
branch; keep logical commit boundaries where they aid review and bisection.
"Depends on" means the listed work must be completed earlier in that branch.
There are no suffix-number exceptions: filenames form one contiguous execution
sequence from `01` through `25`.

## Execution order

### Phase 0 — Build surface (land first; no source changes)

**01 · [Build surface](01-plan-build-surface.md)** — move all 45
`PUBLIC_CLASSES` and 25 `PUBLIC_HEADERS` entries to their private counterparts,
on the recorded decision that **hdEmbree exposes no supported hand-written C++
API**; write down the runtime contract that does exist (plugInfo type names,
`ty:` attributes, AOV names, material render contexts, RenderLab keys); delete
extension-point language describing installed headers. Edits `CMakeLists.txt`
and prose only — **zero source files**, since private headers are still copied
into the build include tree.
_Depends on: nothing. Land it first: every later plan that adds or moves a file
then adds it to a list that is already uniformly private, instead of making a
public/private call that a later plan reverses. `12` removes one entry and
`17`/`18` add several; one-line rebases either way._

### Phase A — Correctness (real bug fixes; need their own tests, not just image diffs)

**02 · [Render setup failure](02-plan-render-setup-failure.md)** — make
`_PreRenderSetup()` validate-before-map and return `bool`; null/type-safe the
unchecked `renderBuffer` casts (incl. the pre-render `MarkAovBuffersUnconverged`
crash path in `aovOutput.cpp`); terminate a failed render. A real crash fix.
_Depends on: 01. Must precede 03 and 06._

**03 · [Render failure signal](03-plan-render-failure-signal.md)** — add a
renderer→render-pass "frame invalid" state so a failed setup does not write a
stale offline product and `usdrender` exits non-zero.
_Depends on: 02 (consumes its `bool` setup return). Separate, larger change than
02 — it touches `renderPass.cpp`'s product-write gate (`IsConverged` at :421)
and removes the `usdrender` fallback writer._

**04 · [MaterialX error handling](04-plan-materialx-error-handling.md)** — make
MaterialX compile/eval failures explicit and non-throwing.
_Depends on: 01. It does not depend on naming. Owns the non-throwing contract
that 10 and 15 rely on._

### Phase B — Core naming

**05 · [Core naming](05-plan-naming-core.md)** — establish full-word quantity
roots and ordered semantic suffixes for role, transport side, coordinate space,
orientation, representation, and measure. Rename the known normal, direction,
IOR, radiance, PDF, coefficient, and SSS families. The exhaustive inventory is
deferred to plan 20 so structural work does not create a second naming pass
inside this plan.
_Depends on: 04. Rename the complete `LightSample` quantity family first.
Plans 06–21 consume these names._

### Phase C — Simplification

**06 · [AOV dispatch](06-plan-aov-dispatch.md)** — replace the AOV
function-pointer table with a direct switch; also dedupe the triplicated
hit-context lookup in the `_ComputeX` helpers.
_Depends on: 02 and 05. Rewrites `renderer.h`/`renderer.cpp` AOV state and
`aovOutput.cpp` writers; strictly serial with 02 and 12 on those files._

**07 · [Closure classification](07-plan-closure-classification.md)** — flatten
`_IsReflectionOnlyNode`'s 65-line generic-lambda `if constexpr` cascade into a
`std::get_if` chain, with a `variant_size_v` `static_assert` for
exhaustiveness. Two commits; a **table-driven classification test lands first**
(none exists today, and `testMaterialXCpp` does not cover this function).
Requires a before/after `perf stat` — it is on a per-path-vertex path.
_Depends on: 05. Must precede 14 (which relocates `rendererImpl.h` code).
Independent of 08._

**08 · [Sampling dispatch](08-plan-sampling-dispatch.md)** — collapse the four
duplicated `Fork`/`Split`/`Distrib`/`Chain` `std::visit` blocks into one
`_NewDomain(_DomainOp, ...)` visitor; `_Draw` keeps its own. Two commits; the
**sampling test expansion lands first** (`Distrib`, `Draw1D`, and the
`monostate` branch are entirely untested). Relies on the `std::visit`
visitor-lambda exception now recorded in AGENTS.md Goal 5.
_Depends on: 05. Solely owns `sampling.h`. Independent of 07._

_The `_pi<T>` template that used to live in this slot moved to 23._

**09 · [Comments & dead code](09-plan-comments-deadcode.md)** — fix wrong/HOW
comments, remove dead `_RayShouldContinue`, delete `renderer/pxrPbrt/` if it is
still unreferenced (nothing includes `pbrtUtils.h` today), individual doc
defects.
_Depends on: 05. `rendererImpl.h`/`renderer.h` edits should precede 14. Low
collision otherwise._

**10 · [Displacement cleanup](10-plan-displacement-cleanup.md)** — one checked
double-to-float conversion helper (5 sites), inline the normal-derivative
projection, use `orientationSign` directly, drop unused frame outputs, and
collapse the now-empty `_XxxImpl` delegations in `displacementEvaluation.cpp`.
Does *not* touch the `catch (...)` wrappers (04) or `auto` (23).
_Depends on: 04 (which removes the catches first, leaving the delegations for
this plan). Owns its own `_TryNormalize`; otherwise near-solely owns its file.
One commit, kept separate from 04 so behavior-changing exception work and
behavior-preserving arithmetic cleanup bisect independently._

### Phase D — Structure (larger refactors; each rewrites a hot file)

**11 · [Command-line authoring](11-plan-command-line-authoring.md)** — add a
repeatable `usdrender -s "{settings}.ty:maxBounces = 12"` option that authors
attribute overrides for any prim into the session layer. Each value is parsed by
the real usda parser in an isolated scratch layer and copied across as a
`VtValue`, so every value type works and no value can author a prim; types come
from the composed stage; every typo is a hard error before anything is authored.
Provides the per-invocation override capability used after plan 12 deletes
render-setting environment variables.
_No code dependency. **Must be completed before 12**, or there is an interval
with no way to override a render setting without editing the stage. Sole editor
of `pxr/usdImaging/bin/usdrender/`; overlaps 12 only on `AGENTS.md`, `README.md`,
`OPTIMIZATION.md`, and this file, which 12 rebases onto._

**12 · [Render settings state](12-plan-render-settings-state.md)** — consolidate
runtime settings into one plain value (owns the `_Execute` setter wall).
_Depends on: 06 and 11 for the replacement override mechanism its doc
updates point at. Rewrites `renderer.h`/`renderer.cpp`; must follow 02 and 06 on
those files — strictly serial._

**13 · [Mesh ownership](13-plan-mesh-ownership.md)** — RAII ownership of the
prototype and instance contexts in `HdEmbreeMesh`: one `_Instance` record vector
replacing the two index-coupled ones, an out-of-line destructor for the
incomplete context types, stated failure postconditions for every
`_PopulateRtMesh()` early return (one of which leaks an `RTCGeometry` today),
and the `_PopulateRtMesh()` ownership/failure declaration contract.
**Ownership only** — the function extraction moved to 16, and primvar-sampler
ownership moved to 15; this plan only centralizes sampler teardown so the
context becoming `unique_ptr` cannot leak them. Sanitizer validation is
opportunistic: no ASan configuration exists in the repo.
_Solely owns `mesh.cpp`; coordinate the `_UpdateTangentFrameCache` algorithm
comment with 09. Precedes 15 and 16._

**14 · [rendererImpl.h decomposition](14-plan-renderer-impl-header.md)** —
break the 1,364-line header (62 definitions, 10 includers) into owned modules:
delete 2 dead symbols, move 7 single-consumer groups into their owning `.cpp`,
add `geometry/surfaceDerivatives` (~500 lines, declaration-only header) and
`integrator/closureClassification` (~110 lines) as `PRIVATE_CLASSES` pairs, plus
five inline headers (`rendererMath`, `rayUtil`, `heroWavelength`,
`integrator/transportPolicy`, `geometry/normalTransforms`), all ~50-135 lines,
i.e. at or below the existing `renderer/` header median. Scope, measured: the
header adds **28 headers / 4,940 lines** on top of `renderer.h`, which all ten
TUs already include — dependencies `renderer.h` supplies are out of reach here.
**Introduces `namespace ty`** (nested in `PXR_NAMESPACE`) with the rule
`ty::Foo` = declared in a header and used by >1 TU, `static _Foo` at `ty` scope
= contained to one TU, and **no anonymous namespaces** — `static` marks
containment at the declaration, which a 1,465-line anonymous block cannot.
Types stay prefixed at `PXR_NAMESPACE` as an explicit interim state that 19
completes.
_Depends on: 05, 07, 09, 10, 12 — every in-place `rendererImpl.h` edit must land
before this relocates the code (09 and 10 both edit lines inside the
`surfaceDerivatives` block). Rebase on all five. Hands the rest of the pass to
19._

**15 · [Primvar binding cache](15-plan-primvar-binding-cache.md)** — resolve
MaterialX `geompropvalue` names to integer handles at graph-compile time and to
samplers once per prototype, instead of hashing a `std::string` against
`primvarMapByString` on every node evaluation. Deletes `primvarMapByString` and
`uniformPrimvarMap` along with their five-site hand synchronization, and gives
`primvarMap` real `unique_ptr` ownership. Surface and displacement compile to
two independent graphs from **one** network, so the handle space is owned by the
material — one name table, one resolved table per prototype, no per-call-site
choice to get wrong. Also adds the missing invalidation path: a material
recompiles behind a *stable* `ty::MaterialData` handle without necessarily
dirtying bound meshes, so `HdEmbreeRenderPass::_Execute()` runs an ungated
`RefreshMaterialBindings()` walk over the `_meshes` registry after `SyncAll()` —
reusing the existing `_displacementVersion` signal (which only material
Sync/Finalize ever bumps, so it already means "material graph changed", and gets
renamed `_materialVersion` here) rather than adding a second counter. Never from
inside a material's `Sync()`, and not gated like the existing camera-dependent
`UpdateSubdivisionLevels()` refresh, which skips triangle meshes entirely.
**Requires a before/after `perf
stat`** — the whole justification is hit-time cost. Four commits, the first
being the `uniform`-authoring audit the behavior-preserving claim rests on; the
`mxcpp` interface change is temporarily slower, so don't cut a release between
commits.
_Depends on: 04 (uses its non-throwing MaterialX error path for the
connected-`geomprop` case), 13 (which must settle `mesh.cpp` lifetimes and
introduce `_ReleasePrimvarSamplers()` first — 15 deletes it), and 14 (which
relocates the `rendererImpl.h` primvar consumers before this plan rewrites
them). Rewrites `geometry/context.h`, which 21
documents, and adds a delegate-level refresh alongside
`UpdateAdaptiveSubdivision()` in `renderDelegate.cpp` plus its trigger in
`renderPass.cpp`'s `_Execute()`._

**16 · [`_PopulateRtMesh()` extraction](16-plan-source-organization.md)** —
extract the instance-update block (`mesh.cpp:2166-2231`) into
`_UpdateInstances()`, handed over from 13 so that plan can be a bisectable
lifetime-only commit. Explicitly **rejects** the other candidate (the
shading-primvar / smooth-normal / tangent block): it is five adjacent
dirty-bit-gated refreshes rather than one responsibility, it kills only one
local, and 15 deletes a third of it. Creates no file and changes no CMake
list. Same-TU move, so bit-identical output is a hard requirement here.
_Depends on: 13, then 15. Not 02/12/14 — those edit renderer files this plan
does not touch._

**17 · [`bsdf/` module directory](17-plan-bsdf-split.md)** — split
`bsdf.cpp` (5,107 lines, plus 4,788 lines of included tables) into a
`materials/bsdf/` directory of layered modules, organized one per kind of BSDF
where that applies, leaving
`bsdf.h`/`bsdf.cpp` holding **only the `mxcpp::Bsdf` namespace API** (627
lines; `Bsdf` is a namespace, not a class). Twelve modules in dependency
order: `mathPrimitives`, `shadingFrame`, `fresnel`, `energyCompensation`,
`thinFilm`, `microfacet`, `sheen`, `diffuse`, `reflectionOnlyInterfaces`,
`dielectric`, `legacySurface`, `closureTraversal` (1,680, the largest), plus
four `*Lut.h` tables. The layered traversal-vs-undifferentiated-lobes split is
**measured and rejected** (one 64-symbol header); the by-kind partition passes
because it distributes those same 64 references across ten small named
interfaces, and its dependency matrix is **strictly lower-triangular — no
cycles**. Internal symbols go into `mxcpp::Bsdf::detail`, not plain `mxcpp`
(which already defines `Clamp01` and `SaturateVec`) and not bare
`mxcpp::Bsdf`, which is the module interface the rest of the renderer calls; the
two colliding locals
are deleted as duplicates of `mxcpp::Clamp01`'s scalar and vector overloads.
Eleven commits, one module each, bottom-up in DAG order; the first two must
diff to zero. Everything new lands in `PRIVATE_CLASSES`/`PRIVATE_HEADERS`
directly.
_Depends on: 14 (the header-vs-TU membership rule only — `mxcpp` already plays
`ty`'s role here, so there is no `mxcpp::ty`; internals nest as
`mxcpp::Bsdf::detail`). Rebase after 04 and 15, which own
`renderer/materials/MaterialXCpp/`. Must precede 19._

**18 · [Light sampler split](18-plan-light-samplers-split.md)** —
split the 1,646-line file **one `.cpp` per light type** (`rectLight`,
`sphereLight`, `diskLight`, `cylinderLight`, `distantLight`, `domeLight`,
118-305 lines each), header-less in `CPPFILES` like the existing
`renderer/integrator/*.cpp`, over **one** `lightSamplerDispatch.h` declaring
their 13 entry points — `ty::SampleXLight()` /
`ty::EvaluateXLightDirection()`, each with its own contract — plus a
`lightSamplerCommon` holding the radiometry every type applies once its
geometry is resolved, over 12 owned exports plus `Pi` and `IsFinite` reused
from `rendererMath.h`. `lightSamplers` → `lightSampler` (one class). Removes
**all four** forward-declaration groups, and empties the type dispatcher: the
sphere solid-angle override and the two 28-line rect/disk shaping splits move
out of `_EvaluateLightDirection` and `operator()` into the shapes that own
them, leaving seven one-line visitor overloads. +222 lines (12.5%), 11 files where
there are 2, and **17 new cross-TU boundaries** — 13 of them one per light
sample/evaluation, which no amount of `inline` can recover without LTO; that
is the plan's one irreducible cost and its measurement gate. Nine commits;
**the first adds the missing cylinder unit test**
(the one type with no coverage today) against current code, with the
adversarial test review. Validates against the named per-type stages in
`typhoon-tests/usdlux/`, **tiered** — a full sweep is 328 frames, so only the
common extraction and the final rename run all of them. Two earlier drafts are **rejected** in the plan:
infinite-vs-finite, and one header per type.
_Depends on: 05 (its `LightSample` family rename must land first), 14. Must
precede 19, 20, and 23 (which collapses shared `ty::Pi<T>` to scalar
`ty::Pi`).
Independent of 16 and 17._

**19 · [`ty` namespace pass](19-plan-ty-namespace.md)** — complete the namespace
ownership invariant over `renderer/`: **every renderer-owned declaration lives in
`PXR_NAMESPACE::ty`**, renderer types drop their redundant `HdEmbree`/`HdEmbree_`
prefix (`HdEmbreePrototypeContext` → `ty::PrototypeContext`, `HdEmbree_Rect` →
`ty::RectLight`, `HdEmbreeRenderer` → `ty::Renderer`). Its amendment preserves
existing translation-unit linkage and anonymous namespaces: file-local
implementation stays outside `ty`, while shared definitions are explicitly
qualified. `MaterialXCpp/` and the pxr-independent `integrator/medium.*` API
remain in `mxcpp`; `TF_DEBUG_CODES` remains at `PXR_NAMESPACE` scope because
the macro specializes `TfDebug::_Traits`. The one header-level anonymous
namespace in `proceduralHelpers.h` was removed separately by making its
existing inline entities externally coherent. Exempt: vendored `BSDL/` and
`lights/pxrIES/` entirely, all of `delegate/` (`HdEmbreeMesh` reads as
"hdEmbree's `HdMesh`", which `ty::Mesh` loses), and the two
`plugInfo.json`-registered types — `TfType` lookup is by string and the failure
mode is a silent load failure, not a build error.
`HdEmbreeTyphoonRenderSettingsAPI` is a codeless schema with no C++ class at all.
_Depends on: 14 (establishes `ty` and the membership rule), 17 and 18 (final
filenames — 16 creates no file, so it is not a dependency here). Must precede
20's naming sweep and 21's contracts._

### Phase E — Final naming audit, contracts, and documentation

**20 · [Naming audit](20-plan-naming-audit.md)** — run the repository-wide
identifier inventory after behavior, module, and namespace work has settled.
Rename remaining unexplained single-letter, abbreviated, or space-less
first-party identifiers across production code and `testenv/`, one semantic
family per commit.
_Depends on: 19. Completes the convention introduced by 05._

**21 · [API contracts](21-plan-api-contracts.md)** — documentation-only
contract sweep across the final module boundaries and names. It changes no
defaults, validation, aggregate layout, or runtime behavior.
_Depends on the settled behavior and declarations from 02, 03, 04, 05, 12,
13, 14, 15, 17, 18, 19, and 20. **De-duplicate:** each owning plan writes its
own declaration contracts; 21 fills only the remaining gaps (`LightSample`,
final `geometry/context.h`, light-shape invariants, and post-03/04
render-pass/material synchronization contracts)._

**22 · [Documentation roles](22-plan-documentation-roles.md)** — give README /
ARCHITECTURE / AGENTS / overview.dox distinct authoritative roles.
_Depends on: 21. Restructures docs the other plans have churned._

### Phase F — Final sweep (over the settled tree)

**23 · [`auto` types](23-plan-auto-types.md)** — remove `auto` outside the
permitted cases across **`renderer/` + `delegate/` only**; also collapses the
`_pi<T>` variable template to a plain `constexpr float` as its own commit.
Its prerequisite **AGENTS.md Goal 5 amendment has already
landed**: `auto` binding a **lambda closure** (14 sites) and `auto` in a
**structured binding** (2 sites) are now permitted — both unnameable in C++17,
same justification as the existing `std::visit` exception — along with the
exact-type-preservation rule and an explicit statement that a
non-`std::visit` generic lambda parameter is still a violation. Explicitly
**defers** `MaterialXCpp/`
(412 matches — first-party `mxcpp`, a scope decision, *not* a vendored
exemption) and `testenv/` (84) to separate unscheduled audits, and **exempts**
`pxrIES/` as vendored. `pxrPbrt/`, if it survives 09, is adapted Typhoon-owned
code under `ty::pbrt` and remains in scope. No identifier renaming — that is
owned by 05 and 20.
_Depends on: everything. A mechanical sweep over the settled tree — running it
earlier wastes effort on code that 06/09/10/14 delete or relocate. The `_pi`
item must follow 14, which relocates the `rendererImpl.h` definition to
`rendererMath.h`, and 18, which owns the extracted light-sampler uses._

### Phase G — Correctness follow-ups discovered by contract review

**24 · [Anonymous AOV convergence](24-plan-empty-aov-convergence.md)** — fix
empty caller AOV bindings that never converge because the legacy `_converged`
flag is reset but never promoted. Add a focused render-pass test and use the
anonymous buffers as the authoritative completion state.
_Discovered by 21. Implement after the settled cleanup sequence._

**25 · [elementId primitive map](25-plan-element-id-primitive-map.md)** —
correct the inverted triangle primitive-parameter assignment by deleting the
condition rather than synchronizing it: clear the triangle caches on the refined
path so their emptiness is the single rule, then drop the mode logic at both
consumers. **Not behavior-preserving**: unrefined meshes currently report
triangle indices where clients expect face indices, so `elementId` values change.
_Discovered by 21. Implement after the settled cleanup sequence._

## Overlap ownership (who is authoritative)

Several plans touch the same edit; each such edit has one authoritative owner:

| Edit | Authoritative owner | Others reference it |
| --- | --- | --- |
| `_Execute` render-settings wall | 12-render-settings-state | — |
| `_PopulateRtMesh` lifetime and failure contract | 13-mesh-ownership | 16-source-organization, 21-api-contracts, 25-element-id-primitive-map (triangle caches only) |
| `_PopulateRtMesh` function extraction | 16-source-organization | 13-mesh-ownership explicitly declines it |
| `bsdf.cpp` file layout | 17-bsdf-split | 19-ty-namespace |
| Light-sampler file layout and per-type contracts | 18-light-samplers-split | 19-ty-namespace, 21-api-contracts |
| Primvar sampler ownership and material binding invalidation | 15-primvar-binding-cache | 13-mesh-ownership, 21-api-contracts |
| Systematic declaration contracts | 21-api-contracts | owning plans write contracts for declarations they introduce |
| Displacement exception contract | 04-materialx-error-handling | 10-displacement-cleanup |
| Hit-context lookup dedup | 06-aov-dispatch | 14-renderer-impl-header |
| Closure-classification test target | 07-closure-classification | 14-renderer-impl-header retargets it |
| `ty` membership rule | 14-renderer-impl-header introduces it; 19-ty-namespace completes it | 17-bsdf-split, 18-light-samplers-split |
| Installed/private build surface | 01-build-surface | 12, 14, 17, and 18 consume the private lists |
| `LightSample` known field renames | 05-naming-core | 21-api-contracts |
| Remaining naming inventory and exceptions | 20-naming-audit | 05-naming-core defines the convention |
| Documentation authority boundaries | 22-documentation-roles | all earlier plans update affected prose |
| `_pi<T>` → scalar | 23-auto-types | 14 relocates the shared renderer definition; 18 reuses it; 19 owns `ty::pbrt` |
| Empty-binding convergence behavior | 24-empty-aov-convergence | 21 documents the temporary failure |
| Triangle/subdivision elementId mapping | 25-element-id-primitive-map | 21 documents only the optional field meaning |

## Hot-file serialization (must be sequential, never parallel)

- `renderer.h` / `renderer.cpp`: **02 → 06 → 12**, plus 09 and 23.
- `rendererImpl.h`: **05, 07, 09, 10, 12 → 14 → 15 → 19 → 23**.
- `mesh.cpp`: **09 → 13 → 15 → 16 → 23**.
- `renderer/lights/lightSampler.cpp` and per-type sampler files:
  **18 → 19 → 20 → 21 → 23**.
- `CMakeLists.txt`: **01 → 12, 14, 17, 18**.
- `geometry/context.h` / `geometry/primvarSampling.h`: **13 → 15 → 20 → 21**.
- `renderer/materials/MaterialXCpp/`: **04 → 15 → 17 → 20 → 21**.
- `delegate/renderPass.cpp`: **03 → 15 → 20 → 21**.
- `aovOutput.cpp`: **02 → 06 → 14 → 23**.
- `displacementEvaluation.cpp`: **04 → 10 → 15 → 23**.
- `renderer/pxrPbrt/`: **09 → 19 → 23**, only if it survives 09.
- Shared structs: **20 → 21**.
