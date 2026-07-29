# Plan: Deduplicate `HdEmbreeSampleDomain`'s five copies of the same `std::visit`

Status: readability cleanup from the full-codebase review against
`pxr/imaging/plugin/hdEmbree/AGENTS.md`. Targets **Goal 1** ("Avoid fancy
C++-isms. Use simple, readable code") and relies on the `std::visit`
visitor-lambda exception now recorded in **Goal 5** (see "The Goal-5 exception"
below).

Split out of an earlier combined refactor draft. It shares no code, no file, and
no validation with `07-plan-closure-classification.md`; the two are independent
and can land in either order.

All commands below run from the **repository root**
(`/path/to/openusd`).

## The duplication

`renderer/sampling/sampling.h:170-253`. Four methods — `Fork`, `Split`,
`Distrib`, `Chain` — each repeat the same ~18-line `std::visit` + generic lambda
+ `std::decay_t` + `if constexpr (is_same_v<SamplerT, std::monostate>)`
scaffold, differing only in which method they call on the sampler (`newDomain`,
`newDomainSplit`, `newDomainDistrib`, `newDomainChain`). `_Draw<Size>`
(`sampling.h:279-294`) repeats the monostate-guarded visit a fifth time. This is
the file a reader most needs in order to understand sampling.

## Design decision: an operation enum inside one visitor

An earlier draft proposed a `_WithSampler(Op&& op)` helper taking a per-call
operation lambda. **That proposal was wrong on its own terms.** Each call site
would pass `[&](auto const& s) { return s.newDomain(domainKey); }` — itself a
generic lambda. The honest accounting:

| Design | Generic lambdas in `sampling.h` |
| --- | --- |
| Current (5 duplicated visits) | 5 |
| `_WithSampler(Op&& op)` | 6 (1 visitor + 4 operations + `_Draw`) |
| **Operation enum in one visitor** | **2** (1 visitor + `_Draw`) |

`_WithSampler` removes duplicated *scaffolding* but *increases* generic-lambda
occurrences. Take the operation-enum form instead: one visitor, one flat switch
naming the four operations, four one-line public methods.

```cpp
private:
    // Which OpenQMC domain-derivation call `_NewDomain()` makes.
    enum class _DomainOp { Fork, Split, Distrib, Chain };

    // Derives a child sample domain from the active sampler. `size` applies to
    // Split only; `index` to Split, Distrib, and Chain; Fork uses neither.
    // Returns an empty domain when no sampler is active (monostate).
    //
    // Goal-5 exception: std::visit over HdEmbreeOpenQMCVariant needs a generic
    // lambda under C++17. This is the only domain-dispatch one in the file.
    HdEmbreeSampleDomain _NewDomain(_DomainOp op,
                                    int domainKey,
                                    int size,
                                    int index) const
    {
        return std::visit(
            [&](auto const& sampler) -> HdEmbreeSampleDomain {
                using SamplerT = std::decay_t<decltype(sampler)>;
                if constexpr (std::is_same_v<SamplerT, std::monostate>) {
                    return HdEmbreeSampleDomain();
                } else {
                    switch (op) {
                    case _DomainOp::Fork:
                        return HdEmbreeSampleDomain(
                            sequence, sampler.newDomain(domainKey));
                    case _DomainOp::Split:
                        return HdEmbreeSampleDomain(
                            sequence,
                            sampler.newDomainSplit(domainKey, size, index));
                    case _DomainOp::Distrib:
                        return HdEmbreeSampleDomain(
                            sequence,
                            sampler.newDomainDistrib(domainKey, index));
                    case _DomainOp::Chain:
                        return HdEmbreeSampleDomain(
                            sequence,
                            sampler.newDomainChain(domainKey, index));
                    }
                    return HdEmbreeSampleDomain();  // unreachable
                }
            },
            openQmcDomain);
    }

public:
    HdEmbreeSampleDomain Fork(HdEmbreeSampleDomainKey key) const
    {
        return _NewDomain(_DomainOp::Fork,
                          static_cast<int>(HdEmbreeSampleDomainKeyValue(key)),
                          1,
                          0);
    }

    HdEmbreeSampleDomain Split(HdEmbreeSampleDomainKey key,
                               int size,
                               int index) const
    {
        return _NewDomain(_DomainOp::Split,
                          static_cast<int>(HdEmbreeSampleDomainKeyValue(key)),
                          std::max(size, 1),
                          std::max(index, 0));
    }
```

**Cost, stated plainly:** `Fork` passes two ignored arguments and `Distrib`/
`Chain` one, which the unified signature makes unavoidable. That is the price of
getting from five generic lambdas to one. It is the right trade here, but it is
a real cost, not a free win.

~72 duplicated lines become one helper plus four short bodies — comfortably
passing the 2x rule.

## `_Draw` keeps its own visit

`_Draw<Size>` is templated on `Size`, returns `std::array<float, Size>`, and is
filled through an out-pointer
(`sampler.template drawSample<Size>(sample.data())`), so it does not fit
`_NewDomain`'s `HdEmbreeSampleDomain`-returning shape. Forcing a shared helper
would produce exactly the over-general abstraction Goal 1 forbids. Four sites
share the helper; `_Draw` stays as it is, with its own monostate guard.

## The Goal-5 exception

**Decided and recorded.** AGENTS.md Goal 5 now permits an `auto` parameter in a
`std::visit` visitor lambda, kept to one per dispatch site and commented as the
exception. Nothing here is left to the implementer.

The reasoning, for the record. Goal 5 previously read: "Do not use `auto` except
for range-for loop variables and to hide ridiculous standard-library iterator
types." A generic lambda's `auto const& sampler` parameter *is* a variable
declared with `auto`, so the surviving visitor lambda did not comply as written,
and an earlier draft of this plan papered over that by claiming Goal 5 covered
only "variable types" — a distinction the goal does not make. The honest
resolution is to widen the goal, not to misread it.

The alternatives, and why they are worse:

- **C++20 template-parameter lambda** (`[&]<class SamplerT>(SamplerT const&)`)
  would name the type explicitly. Unavailable: the project builds at C++17
  (`cmake/defaults/CXXDefaults.cmake:12`).
- **A visitor functor struct** with
  `template <class SamplerT> HdEmbreeSampleDomain operator()(SamplerT const&) const`
  names the parameter type and complies literally. It costs a struct declaration
  with captured state passed as members — more machinery and a jump away from
  the call site, against Goals 1 and 2.
- **Seven explicit `operator()` overloads** (six samplers + monostate) comply
  fully with no templates, but reintroduce six-way duplication — the exact thing
  this plan removes.

So: keep the single generic lambda, with the inline comment shown above
recording that it is the sanctioned exception rather than an oversight. The
AGENTS.md Goal 5 wording is already updated, so this plan's commits do not need
to touch it.

The exception is narrow. It licenses the visitor lambda in a `std::visit` call
and nothing else — not `auto` locals inside the lambda body, and not a second
generic lambda passed *into* the visitor, which is exactly the shape this plan
rejects above.

## Prerequisite: expand sampling test coverage first

`testenv/testHdEmbreeSampling.cpp` does not currently cover the behavior this
refactor touches:

- `Distrib` is **never called** — no test exists for it at all.
- `Draw1D` is never called.
- The `monostate` branch is never exercised. A default-constructed
  `HdEmbreeSampleDomain` holds `monostate`, and every method's empty-domain
  return is untested.
- `Fork`/`Split`/`Chain` determinism is checked only under `OpenQMCSobolBN`
  (`TestOpenQmcDomainsAreDeterministicAndSeparated` at `:135`,
  `TestOpenQmcSplitAndChainAreStable` at `:161`). Only
  `TestAllOpenQmcSequencesDrawSamples` (`:195`) sweeps all six sequences, and it
  exercises `Fork` + `Draw4D` only.

A fixed-seed image diff is weak cover for a dispatch rewrite: the untested
combinations are exactly the ones a mistake in the unified switch would break —
for example, swapping the `Distrib` and `Chain` arms, which no current test and
possibly no current scene would reveal. Land the tests as their **own commit,
before** the refactor:

1. All four domain ops x all six sequences: samples are finite, within `[0, 1)`,
   and repeatable across two identical call chains.
2. `Distrib` determinism and index-decorrelation, mirroring the existing
   `Fork`/`Split` patterns.
3. Default-constructed (`monostate`) domain: all four ops return an empty domain
   and every `DrawND` returns zeros without crashing.
4. `Draw1D`.
5. Distinct domain keys and distinct `size`/`index` arguments produce distinct
   sequences — this is what pins each enum arm to the right sampler call.

Per the repository test rule, run an adversarial review agent over the new tests
before landing them: confirm they test intended behavior rather than encoding
whatever the current code returns.

## Sequencing and validation

Two commits:

1. **Sampling test expansion** (tests only, no source change) + adversarial test
   review.
2. **The `_NewDomain` refactor** in `sampling.h`.

Commit 2 is behavior-preserving:

- The expanded `testHdEmbreeSampling` must pass.
- A fixed-seed render must be bit-identical (`oiiotool --diff`) for a scene that
  exercises the sampler through progressive accumulation.

### Running the tests

`testHdEmbreeSampling` is built but **never registered with CTest** —
`CMakeLists.txt:192` has no `pxr_register_test()` call, and `ctest -N` lists only
`testHdEmbreeSubdivision`, `testHdEmbreeLightLinking`, and
`testHdEmbreeRenderSettings` for this plugin. A `ctest -R testHdEmbreeSampling`
filter silently matches nothing and passes.

Build and run the build-tree binary directly, from the repository root:

```sh
pixi run cmake --build build --target testHdEmbreeSampling
pixi run build/pxr/imaging/plugin/hdEmbree/testHdEmbreeSampling
```

`--target` builds into the build tree; it does **not** refresh
`$CONDA_PREFIX/tests/`. And `$CONDA_PREFIX` is unset in the caller's shell
before `pixi run` supplies it, so a `pixi run "$CONDA_PREFIX/tests/..."` command
expands to a bare path and fails. Use the build-tree path above.

Then the registered suite, with an anchored regex — unanchored `testHdEmbree`
matches every target beginning with that name:

```sh
pixi run ctest --test-dir build -R '^testHdEmbree' --output-on-failure
```

Registering `testHdEmbreeSampling` (and `testHdEmbree`,
`testHdEmbreeLightSamplers`, `testMaterialXCpp`) with `pxr_register_test()` is a
real gap worth fixing, but it is CMake/test-infrastructure work with its own
blast radius. Raise it separately rather than folding it into this refactor.

_Depends on: 05 (naming). Solely owns `sampling.h`; no collision with any other
plan. Independent of 07._

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
