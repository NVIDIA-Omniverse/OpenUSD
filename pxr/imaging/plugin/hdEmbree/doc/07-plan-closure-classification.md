# Plan: Flatten `_IsReflectionOnlyNode`'s closure classification

Status: readability cleanup from the full-codebase review against
`pxr/imaging/plugin/hdEmbree/AGENTS.md`. Targets **Goal 1** ("Avoid fancy
C++-isms. Use simple, readable code").

This plan owns exactly one construct in one file. The sampling-dispatch
duplication that used to share this plan is now `08-plan-sampling-dispatch.md`
(different subsystem, different validation, no shared code). The `_pi<T>`
variable template moved to `23-plan-auto-types.md`. The AOV function-pointer
table is `06-plan-aov-dispatch.md`.

All commands below run from the **repository root**
(`/home/anders/code/openusd-omniverse`).

## The construct

`renderer/rendererImpl.h:491-556`. A `std::visit` over the **15**-alternative
BSDF node variant (`renderer/materials/MaterialXCpp/materials/closureTree.h:242-256`),
using a generic lambda `[&](auto const& data) -> bool` with
`using T = std::decay_t<decltype(data)>;` and a chained
`if constexpr (std::is_same_v<T, ...>)` cascade, recursing into child nodes.
65 lines. This is the densest "fancy" construct in the reviewed code.

(For whoever implements this: it is `if constexpr`, not SFINAE. An earlier draft
of this plan called it "SFINAE-style"; it isn't, and the distinction matters
when judging what the replacement must preserve.)

## Do not add a node-kind enum

The closure tree has no kind discriminator today: `Bsdf::Node` holds only
`NodeData` (a bare `std::variant`), and there is no `Kind()` or `As<T>()`
accessor. Adding one would duplicate the variant tag into a second,
hand-synchronized enum and couple `rendererImpl.h` more tightly to
`MaterialXCpp/` — the opposite of Goal 1.

## Suggested change

Replace the generic lambda with a flat `std::get_if<T>()` chain using the
concrete data type per branch. This is the style already used for the light
variant at `renderer/lights/lightSamplers.cpp:1263-1295`, so it introduces no
new idiom:

```cpp
const mxcpp::Bsdf::NodeData& data = node->data;

// Pure reflection lobes: always reflection-only regardless of parameters.
if (std::get_if<mxcpp::Bsdf::OrenNayarDiffuseData>(&data) ||
    std::get_if<mxcpp::Bsdf::BurleyDiffuseData>(&data) ||
    std::get_if<mxcpp::Bsdf::ConductorData>(&data) ||
    std::get_if<mxcpp::Bsdf::SheenData>(&data)) {
    return true;
}

// Dielectric: reflection-only when disabled, or when authored R-only.
if (const mxcpp::Bsdf::DielectricData* const dielectric =
        std::get_if<mxcpp::Bsdf::DielectricData>(&data)) {
    return _IsEffectivelyZero(dielectric->weight) ||
           dielectric->scatterMode == mxcpp::Bsdf::ScatterMode::Reflection;
}

// ... one block per remaining leaf type; Mix/Layer/Add/Multiply recurse.

return false;
```

Order the branches with the closure types that dominate real scenes first
(`AdobeOpenPbrData`, the diffuse lobes, `DielectricData`, then the composites),
so the common cases exit the chain early. Add a one-line WHAT comment per
closure category, as above.

## Exhaustiveness

Neither the current `if constexpr` cascade (trailing `else return false`) nor
the `get_if` chain (trailing `return false`) gets compiler-checked
exhaustiveness, so a newly added closure type is silently classified as *not*
reflection-only. A `switch` with a `default` arm would be no better. Close the
hole cheaply, without a parallel enum, by pinning the alternative count next to
the trailing return:

```cpp
static_assert(std::variant_size_v<mxcpp::Bsdf::NodeData> == 15,
              "A closure node kind was added: classify it above.");
return false;
```

Adding a variant alternative then breaks the build at the one site that must be
updated.

## Performance: this is not free, so measure it

`_IsReflectionOnlyClosure` is called per path vertex
(`renderer/integrator/pathIntegrator.cpp:659`,
`renderer/integrator/lighting.cpp:142`) and walks the whole closure tree on
every call. The change replaces one `std::visit` jump-table dispatch with up to
15 sequential tag comparisons per node. That will very likely disappear into the
tree walk and the branch predictor, but "readability-only" is a claim about
*output*, not about *speed*, and this plan must not treat the two as the same
thing.

Required, per the profiling procedure in `AGENTS.md`: a before/after
`perf stat -r 5` on a material-heavy scene, using the profile build and
`-s "{settings}.ty:randomNumberSeed = 1"`. Report renderer-reported samples/sec alongside
wall time. If the regression exceeds run-to-run noise, reorder the chain or stop
and reconsider — do not land a measured slowdown for a readability win.

Separately, and **out of scope here**: the deeper cost is that this
classification is recomputed per bounce over an immutable tree. The real fix is
to compute the flag once when the closure is built. That is its own performance
change with its own measurement; record the observation in
`OPTIMIZATION.md`/`TODO.md` rather than acting on it in this commit.

## Validation

The change is behavior-preserving. Two things must verify it, and the obvious
candidate is not one of them.

**`testMaterialXCpp` does not exercise this function.** It compiles
`rendererImpl.h` transitively (`renderer/integrator/sss.cpp:8`), but no test in
that target ever calls `_IsReflectionOnlyNode` or `_IsReflectionOnlyClosure`;
its BSDF tests only check closure-tree *construction*. Building or running it
proves nothing about this change. Do not cite it as coverage.

### 1. Add a table-driven classification test (do this first, own commit)

There is currently **no** direct test of this function at any level. Add one
before touching the implementation, so the refactor has a real oracle.

`_IsReflectionOnlyNode` is an `inline` function in an anonymous namespace inside
`rendererImpl.h` (the block spanning `:350-1207`), so a test translation unit can
call it simply by including the header — no linkage change needed.

Cases, built as hand-constructed `Bsdf::ClosureTree` values:

- Each of the 15 leaf types classified in isolation, at both outcomes where the
  type has a parameter-dependent answer (`DielectricData` with
  `scatterMode = Reflection` / `Transmission` / zero weight;
  `DielectricInterfaceData` zero and non-zero `transmissionWeight`;
  `GeneralizedSchlickData` likewise; `AdobeOpenPbrData` across opacity,
  transmission weight, and subsurface weight; `TranslucentData` and
  `SubsurfaceData` at zero and non-zero weight).
- `UnsupportedData` and any type with no explicit branch — pins the
  fall-through answer so the `static_assert` above has a companion behavioral
  check.
- `MixData` at `mix = 0`, `mix = 1`, and an intermediate value, with each of the
  four combinations of reflection-only / not for `fg` and `bg`.
- `LayerData`, `AddData`, `MultiplyData`, each with both children
  reflection-only and with one not.
- A nested composite (Mix of Layer of Add) to cover recursion.
- Invalid `NodeId` and an empty tree — `tree.Get()` returns null; confirm
  `false`.

**Placement.** Cheapest option: add the source file to the existing
`testMaterialXCpp` target in `CMakeLists.txt:321` — its include and link closure
already compiles `rendererImpl.h`, so no new library list is needed. Cleaner but
larger option: a standalone `testHdEmbreeClosureClassification` target
duplicating that closure. Take the cheap option unless it drags in unrelated
link dependencies; note in the commit message which was chosen. Either way, add
a `pxr_register_test()` call so CTest actually runs it (see below).

Run an adversarial review over the new test per the repository test rule —
specifically, check that it asserts *intended* classification semantics rather
than transcribing whatever the current cascade happens to return.

### 2. Fixed-seed image diff

A material scene reaching `_IsReflectionOnlyNode` must render bit-identically
(`oiiotool --diff`) with `-s "{settings}.ty:randomNumberSeed = 1"`. Use an existing
scene rather than authoring a synthetic 15-closure stage — the table-driven test
above is what provides category coverage, and an image is a poor instrument for
it. A mixed reflection/transmission material scene from the fidelity suite is
sufficient here.

### Running the tests

`testHdEmbreeSampling`, `testMaterialXCpp`, `testHdEmbree`, and
`testHdEmbreeLightSamplers` are built but **never registered with CTest** —
`CMakeLists.txt` has no `pxr_register_test()` for any of them, and `ctest -N`
lists only `testHdEmbreeSubdivision`, `testHdEmbreeLightLinking`, and
`testHdEmbreeRenderSettings` for this plugin. A `ctest -R` filter naming them
silently matches nothing and passes.

Build and run the build-tree binary directly, from the repository root:

```sh
pixi run cmake --build build --target testMaterialXCpp
pixi run build/pxr/imaging/plugin/hdEmbree/testMaterialXCpp
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

Registering the four unregistered targets is a real gap, but it is a
CMake/test-infrastructure change with its own blast radius; raise it separately
rather than folding it into a readability commit. The one exception is the new
classification test from step 1 — register that one when adding it.

## Sequencing

1. Table-driven classification test (tests + CMake only) + adversarial review.
2. The `get_if` rewrite, with the `static_assert` and the perf measurement.

_Depends on: 05 (naming). Must precede 14 (`rendererImpl.h` split), which
relocates this code. Independent of 08._

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
