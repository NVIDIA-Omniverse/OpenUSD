# Plan: Remove `auto` in the integration layer, and collapse the duplicated `_pi`

Status: readability cleanup from the full-codebase review against
`pxr/imaging/plugin/hdEmbree/AGENTS.md`. Targets **Goal 5**: always name
meaningful, reasonably short types explicitly.

**Scope is the production integration layer only** — `renderer/` and
`delegate/`, excluding vendored code and `MaterialXCpp/`. This plan is *not* a
full-codebase Goal-5 compliance pass; see "Scope and deferrals" for what is
explicitly left out and why.

It also carries one related item folded in from an earlier combined refactor
draft (now split into `07-plan-closure-classification.md` and
`08-plan-sampling-dispatch.md`): the duplicated `_pi` constant. Same mechanical
type-spelling character, same final-sweep timing, but it lands as its **own
commit** — it is a separate concern from `auto` and must bisect independently.

About 160 grep matches to triage across `renderer/` and `delegate/`, 57 of them
in the core reference path (integrators, `renderer.cpp`, `rendererImpl.h`). That
is the triage list, not the edit list — a large share are already-permitted
range-for variables and lookup iterators. Purely a type-spelling change; it must
not change behavior.

## Prerequisite: amend AGENTS.md Goal 5 — **DONE, landed ahead of this plan**

Goal 5 originally permitted `auto` only for range-for variables, ridiculous
lookup-iterator types, and a `std::visit` visitor parameter. That list was
**incomplete**, and this sweep could not be executed against it: it would have
mandated removing `auto` from constructs that have no other legal or readable
spelling in C++17. Two cases were added, plus the type-preservation rule from
Method step 3 and an explicit statement that a non-`std::visit` generic lambda
parameter is still a violation.

This is no longer a commit of this plan — it is already in `AGENTS.md`. The
sections below record *why* each exception exists and which sites it covers, so
the sweep can be triaged against them. **Read the current Goal 5 as
authoritative**, not this summary.

### Lambda closure objects

A lambda's closure type is unnameable. The only alternatives are
`std::function` (type erasure, indirection, and a possible allocation — several
of these are on per-path-vertex code), a named functor struct, or a free
function plus explicit capture-passing. All three add machinery and cost more
than they remove, which is exactly the reasoning already recorded for
`std::visit`. There are **14** such bindings in scope, including:

- `delegate/adaptiveSubdivision.cpp:52` `const auto findRoot = [&parents](size_t group) {...}`
- `delegate/adaptiveSubdivision.cpp:59` `const auto unite = [&parents, &findRoot](...)`
- `renderer/integrator/volumeTransport.cpp:55` `const auto throughputIsBlack = [&]() {...}`
- `renderer/integrator/volumeTransport.cpp:60` `const auto addFiniteLightHit = [&]() {...}`
- `renderer/lights/lightRegistry.cpp:22` `auto eraseDomeEntries = [this](...)`

**Recorded in Goal 5:** `auto` is permitted when binding a lambda closure object.

### Structured bindings

`auto [a, b] = ...` is required syntax; C++17 has no typed form. It *can* be
avoided by declaring the underlying `std::pair`/`std::tuple` explicitly, but
that reintroduces the long container iterator type the lookup exception exists
to hide, and loses the two names that make the site readable. In scope:

- `delegate/adaptiveSubdivision.cpp:126` `const auto [it, inserted] = groupIndices.emplace(...)`
- `delegate/mesh.cpp:1487` `auto [it, inserted] = groupMap.emplace(...)`

(The five in `renderer/materials/mxcppAdapter.cpp:250-291` are range-for
variables and are already permitted twice over.)

**Recorded in Goal 5:** `auto` is permitted in a structured binding declaration.

### Still narrow: the `std::visit` exception

AGENTS.md Goal 5 permits an `auto` **parameter** on a `std::visit` visitor
lambda, one per dispatch site. **This sweep must not "fix" those parameters.**
The exception covers the visitor parameter only — `auto` locals *inside* a
visitor body are still in scope for this sweep.

**Do not work from a site list.** Any inventory written here is stale by
design: `07` flattens the `rendererImpl.h:501` generic-lambda visitor
(`_IsReflectionOnlyNode`) out of existence, `08` collapses four of
`sampling.h`'s five, `14` relocates whatever is left of `rendererImpl.h`, and
`18` reorganizes `lightSamplers.cpp`. Note also that
`lightSamplers.cpp:1498` is **not** a lambda at all — it visits the
`HdEmbreeLightSampler` functor's `operator()` overloads, so there is nothing
there to exempt.

Instead: re-enumerate with `rg -n 'std::visit' renderer delegate`, exclude
`BSDL/` and `MaterialXCpp/`, and for each surviving *generic-lambda* visitor
confirm the one-line exception comment is present, adding it where missing.

## Policy

Permitted:

- `auto`/`auto&` as a range-for loop variable.
- `auto` for ridiculous standard-library iterator types returned by `.find()`
  and similar lookup operations. This exception exists to avoid obscuring the
  algorithm with long container implementation types.
- An `auto` parameter on a `std::visit` visitor lambda, one per dispatch site,
  commented as the exception.
- `auto` binding a lambda closure object.
- `auto` in a structured binding declaration.

Not permitted (fix these):

- `auto` for the result of `new`, `dynamic_cast`, `static_cast`,
  `reinterpret_cast` — the concrete type is on the same line.
- `auto` for lookup results whose concrete type is short and meaningful; the
  iterator exception is for genuinely ridiculous standard-library types.
- `auto` for values with short, known types (`std::chrono::time_point`, plain
  structs, pointers).
- A generic `auto` parameter on a lambda that is **not** a `std::visit` visitor
  (see below — there is exactly one, and it has a concrete fix).

## Scope and deferrals

| Tree | `auto` matches | This plan |
| --- | --- | --- |
| `renderer/` + `delegate/`, less the rows below | ~160 | **in scope** |
| `renderer/materials/MaterialXCpp/` | 412 | **deferred** to its own audit |
| `testenv/` | 84 | **deferred** to its own audit |
| `renderer/materials/BSDL/` | — | exempt (vendored) |
| `renderer/lights/pxrIES/` | 1 | exempt (vendored) |
| `renderer/pxrPbrt/` | 0 | **in scope if it survives 09**; adapted code owned as `ty::pbrt` |

Two corrections to earlier drafts of this plan:

- **`MaterialXCpp/` is first-party, not third-party.** `mxcpp` is Typhoon-owned
  code that `04`, `15`, `17`, and `19` all edit. Excluding it is a *scope
  decision* — 412 matches is a second sweep of comparable size, and folding it
  in would make this plan unreviewable — not a vendored-code exemption. Say so
  rather than mislabelling it.
- **`pxrIES/` is exempt here because `19` exempts it.** `19-plan-ty-namespace.md`
  records `renderer/lights/pxrIES/` as vendored from Cycles via Pixar and
  explicitly unchanged. This plan must match, or the two collide. That decision
  also removes `pxrIES.cpp:28` from the `_pi` item below.

Consequently the completion criterion is: **within `renderer/` and `delegate/`,
excluding `MaterialXCpp/`, `BSDL/`, and `pxrIES/`, every remaining `auto` is a
range-for variable, a ridiculous lookup iterator, a `std::visit` visitor
parameter, a lambda closure binding, or a structured binding.** Any other
remaining use is a miss. If `pxrPbrt/` survives 09, it is included under its
decided `ty::pbrt` ownership.

## Confirmed violations (representative, not exhaustive)

Delegate layer:

- `delegate/renderDelegate.cpp:476` `auto* mesh = new HdEmbreeMesh(rprimId);`
  → `HdEmbreeMesh*`.
- `delegate/renderDelegate.cpp:492` `auto* mesh = dynamic_cast<HdEmbreeMesh*>(...)`
  should be explicit. The iterator at `:494` may remain `auto` under the
  lookup-iterator exception.
- `delegate/mesh.cpp:2024` `auto* protoCtx = _GetPrototypeContext();`
  → `HdEmbreePrototypeContext*` (spelled explicitly at mesh.cpp:1867, 2058).
- `delegate/mesh.cpp:1215, 1276, 1288, 1334` are `.find()` iterator results
  and may remain `auto` when spelling the container iterator would be
  ridiculous.
- `delegate/renderPass.cpp:77-78, 99-100, 166, 174, 598, 602, 619` — cluster of
  `auto ds`, `auto sampled`, `auto handle`. Spell the handle types for
  consistency with renderPass.cpp:62, 94, which already use
  `HdContainerDataSourceHandle`.

Renderer core:

- `renderer/renderer.cpp:377` `auto now = std::chrono::steady_clock::now();`
  → `std::chrono::steady_clock::time_point`.
- `renderer/renderer.cpp:784` `for (const auto& writer : _aovWriters)` →
  `for (_AovWriter const& writer : ...)` (this file is being reworked in
  `06-plan-aov-dispatch.md`; coordinate).
- `renderer/rendererImpl.h:718, 804, 933, 969` are `.find()` iterator results
  covered by the permitted iterator exception. Keep `auto`.
- `renderer/rendererImpl.h:782, 789, 806, 813, 973-978`
  `auto* ...Sampler = dynamic_cast<...>` — spell
  `HdEmbreeTriangleVertexSampler*` etc.
- Framebuffer-lock `auto lock` at `renderer.cpp:538, 600` is not a
  range-for or lookup iterator. Spell its reasonably short RAII type.

Integrators:

- `renderer/integrator/surfaceShading.cpp:99, 106, 133, 145`
  `auto const* const instanceContext` / `prototypeContext` →
  `HdEmbreeInstanceContext const*` / `HdEmbreePrototypeContext const*`.
- `renderer/integrator/sss.cpp:312` `auto const* prototypeContext` from a
  `static_cast` → spell the type.
- `renderer/integrator/surfaceShading.cpp:426, 439, 449, 500-501` are
  `.find()` iterator results and may retain `auto`.

Geometry / AOV:

- `renderer/geometry/displacementEvaluation.cpp:614`
  `auto const* faceVertexCounts` → `uint32_t const*`.
- `renderer/aov/aovOutput.cpp:336` `const auto& aovName` — borderline; spell
  the token type.

**Do not rename identifiers in this sweep.** Earlier drafts asked for
opportunistic iterator renaming ("use names that identify the looked-up
value"); that contradicts "purely a type-spelling change" and belongs to
`05-plan-naming-core.md`. Preserve every name.

## The one non-`std::visit` generic lambda

`delegate/adaptiveSubdivision.cpp:234`:

```cpp
const auto clipToPlane = [&](auto const& distance) { ... };
```

This is a generic parameter outside the `std::visit` exception, so the policy
does not cover it. It has a concrete fix rather than a new exception: all seven
call sites (`:264-278`) pass **captureless** lambdas of the shape
`double(GfVec4d const&)`, so the parameter can be spelled
`double (*distance)(GfVec4d const&)` and the lambdas convert implicitly. The
closure binding on the left-hand side stays `auto` under the Goal-5
closure exception.

Verify the captureless property still holds at execution time before applying
this; if a capture has been added by then, escalate rather than reaching for
`std::function`.

## `_pi`: five copies of π, not three

Folded in from that earlier combined refactor draft. Earlier drafts said "three
definitions plus one consumer, four files". That inventory was wrong in **both**
directions. The real state of the tree today:

| Site | Form | Owner |
| --- | --- | --- |
| `renderer/rendererImpl.h:50` | `template <typename T> constexpr T _pi` | **this plan**, at wherever `14` lands it |
| `renderer/lights/lightSamplers.cpp:28` | `template <typename T> constexpr T _pi` | **`18`** (it must, to build its six extracted TUs) |
| `renderer/lights/pxrIES/pxrIES.cpp:28` | `template <typename T> constexpr T _pi` | **nobody** — vendored, exempt per `19` |
| `renderer/pxrPbrt/pbrtUtils.h` | `pi<T>` (used at `:43, :53`) | **this plan if it survives 09** — adapted code owned as `ty::pbrt` |
| `delegate/light.cpp:36` | `constexpr float _pi` | **already in the target form**; no change |

So this plan's guaranteed `_pi` work is one definition and its consumers, plus
the conditional `ty::pbrt::pi` definition if plan 09 does not delete it.

### Settled end state: three module-local constants, or four if pxrPbrt survives

Do **not** create a new shared π constant, and do **not** have one module
include another's header to borrow it. Coupling `lightSamplerCommon.h` to
`rendererMath.h` for a single `float` fails Goal 1's two-for-one test outright.
The end state is three, each owned by the module that uses it:

| Location | Spelling | Owner |
| --- | --- | --- |
| `renderer/rendererMath.h` (from `14`) | `inline constexpr float _pi` | **this plan** |
| `18`'s `lightSamplerCommon.h` | `inline constexpr float _pi` | **`18`** |
| `delegate/light.cpp:36` | `constexpr float _pi` | already correct; no change |
| `renderer/pxrPbrt/pbrtUtils.h` | `inline constexpr float pi` in `ty::pbrt` | **this plan, only if the file survives 09** |

**The linkage differs by location, and getting it wrong reverses `14`.**
`14-plan-renderer-impl-header.md:288` records that constants moving into a
shared header become `inline constexpr` — at namespace scope a plain
`constexpr` implies internal linkage, so every includer gets its own copy and
`14`'s "one entity, not ten" goal silently fails. Both header definitions above
are therefore `inline constexpr`. `delegate/light.cpp`'s is `.cpp`-local, where
plain `constexpr` is correct and already in place.

### Work

- Convert the `rendererImpl.h:50` definition to
  `inline constexpr float _pi = static_cast<float>(M_PI);` — but note
  `14-plan-renderer-impl-header.md:277` relocates it to
  `renderer/rendererMath.h`, consumed by the seven `integrator/*.cpp` and by
  `camera.cpp`. Apply the change wherever it has landed, not at the line number
  above. If `14` has already made it `inline constexpr T _pi`, this is a
  template-to-scalar change only, keeping `inline`.
- Drop `<float>` at every consumer. `renderer/integrator/lighting.cpp:297`
  (`1.0f / _pi<float>`) is the cross-file one that breaks if the definition is
  edited without it; after `14` there will be more, since the header becomes a
  shared math leaf. Sweep with `rg -n '_pi<' renderer delegate` before and
  after; the only permitted survivor is vendored `pxrIES/`.
- If `renderer/pxrPbrt/pbrtUtils.h` survives 09, collapse `ty::pbrt::pi<T>` to
  `inline constexpr float pi` and remove `<float>` at both consumers. Keep it
  inside `ty::pbrt`; do not couple adapted pbrt helpers to `rendererMath.h`.
- Verify `18` left `lightSamplerCommon.h`'s definition as
  `inline constexpr float`, and report it if not. `23` does not edit it — a
  later plan cannot make an earlier one build — but it is the one place the
  three-constant end state can silently come out wrong.

Because `18` and `14` both move the ground under this item, **re-run the audit
at execution time** rather than trusting the table above. The table records
ownership, not a work list.

## Method

1. Enumerate every `auto` in scope:
   ```sh
   rg -n '\bauto\b' renderer delegate -g '*.cpp' -g '*.h' \
       -g '!**/MaterialXCpp/**' -g '!**/BSDL/**' \
       -g '!**/pxrIES/**' -g '!**/pxrPbrt/**'
   ```
2. Triage each against the policy above. Do not spell a long container
   `const_iterator` merely to satisfy the sweep.
3. **Preserve the deduced type exactly.** A replacement must match in reference
   category, top-level constness, and pointee constness; do not substitute a
   merely convertible type, a base class, or a value where a reference was
   deduced. `auto*` from a member returning `T const*`, and `auto` deducing a
   value copy where the reader assumes a reference, are the two realistic ways
   to silently change behavior here.
4. Make that rule checkable rather than aspirational. For each non-trivial
   replacement, temporarily insert
   `static_assert(std::is_same_v<decltype(x), T>);`
   immediately after the declaration, compile, then delete it. Cheap, and it
   converts the whole plan's risk into a compile error.
5. Do it file-by-file so each diff is small and independently reviewable.

## Commit split

The Goal 5 amendment already landed, so this plan is two commits:

1. **`auto` sweep**, file-by-file. Type spelling only — no renames, no
   restructuring. Includes the `adaptiveSubdivision.cpp:234` function-pointer
   parameter, which is the one non-mechanical edit and should be its own hunk
   in the review.
2. **`_pi` collapse.** Unrelated to `auto`; kept separate so the two bisect
   independently.

## Validation

- **Build before testing.** `ctest` does not compile changed targets:
  `pixi run cmake --build build --target install` first, then
  `pixi run ctest --test-dir build --output-on-failure`.
- `ctest` does not cover everything. `testHdEmbree`, `testHdEmbreeSampling`,
  `testHdEmbreeLightSamplers`, and `testMaterialXCpp` are built but never
  passed to `pxr_register_test()`, so CTest never runs them. Because this sweep
  touches files those tests exercise, run their binaries directly from
  `"$CONDA_PREFIX/tests/"` as well.
- **Bit-identity, against named stages.** The change is type-spelling only, so
  a fixed-seed render must be bit-identical, not merely close. Three stages,
  chosen to cover the three densest clusters of edits — the integrators, the
  primvar-sampler `dynamic_cast` block, and the light samplers / `_pi`
  consumers. Run from `/home/anders/code/openusd-omniverse`, once before the
  sweep and once after:

  ```sh
  SUITE=/home/anders/code/typhoon-test-suite
  for WHEN in before after; do
    for STAGE in \
        materials/open_pbr/feature_specular.usda \
        materials/geometric/geompropvalue_color3.usda \
        usdlux/dome.usda; do
      pixi run usdrender --complexity high \
          -s "{settings}.ty:randomNumberSeed = 1" \
          "$SUITE/$STAGE" --outputRoot "/tmp/hdembree-23-$WHEN"
    done
  done

  for P in open_pbr/feature_specular.exr \
           geometric/geompropvalue_color3.exr \
           dome-embree.0001.exr; do
    pixi run oiiotool --diff "/tmp/hdembree-23-before/$P" \
                             "/tmp/hdembree-23-after/$P"
  done
  ```

  The product names are the stages' authored `productName` values
  (`feature_specular.usda:48`, `geompropvalue_color3.usda:72`,
  `dome.usda:30` — the last is `dome-embree.{frame:04d}.exr`, so confirm the
  frame number `usdrender` expands rather than assuming `0001`). Leave each
  stage's authored resolution and sample settings alone.

  Zero difference is required for all three. Paste the `oiiotool --diff` output
  into the commit message; a non-reproducible "bit-identical" claim is not a
  validation.
- **Suite sweep on the `_pi` commit.** `_pi<float>` → `inline constexpr float _pi` is
  the same value and the same type, so it too must diff to zero, but it reaches
  every light type. Run
  `cd /home/anders/code/typhoon-test-suite && pixi run pytest usdlux` after it.
- After the sweep, re-run the grep from Method step 1 and confirm every survivor
  falls under the five permitted cases listed in "Scope and deferrals".

## Note

This sweep overlaps line-for-line with `06-plan-aov-dispatch.md` (renderer.cpp:784)
and `05-plan-naming-core.md`. Land those structural changes first where they
delete the offending lines outright, then sweep the remainder. Identifier
renaming belongs to `05`, not here.

The two deferred audits — `MaterialXCpp/` (412) and `testenv/` (84) — are
follow-on work, not part of this plan. Neither is scheduled; open them as new
plans if and when they are wanted.

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
