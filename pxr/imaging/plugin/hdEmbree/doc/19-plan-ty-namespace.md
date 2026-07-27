# Plan: Complete the `ty` namespace pass over `renderer/`

Status: required. Repository-wide rename over `renderer/`, no behavior change.
Completes the namespace ownership invariant that `14` begins.

Sequencing: depends on `14` (introduces `namespace ty` and the membership rule
for the code it extracts), `17` and `18` (their file splits must settle first,
or every moved symbol is renamed twice). Must precede the cross-struct naming
sweep in `20` and the final contracts in `21`.

`01-plan-build-surface.md` was formerly part of this plan and is now
independent. Build visibility and namespace placement are unrelated concerns:
private code can be named `HdEmbreeMesh`, and a namespace controls neither
installation nor support commitments. `01` owns what ships; this plan owns what
things are called.

## Target invariant

> Every Typhoon-owned declaration under `renderer/` lives in its owner
> namespace — `PXR_NAMESPACE::ty`, or `mxcpp` under `MaterialXCpp/`.
> Typhoon-owned types drop the redundant `HdEmbree` / `HdEmbree_` prefix.
> TU-contained definitions are `static`. **No anonymous namespace remains in
> Typhoon-owned renderer or MaterialXCpp code.** Vendored `BSDL/` and
> `pxrIES/` are unchanged.

Note what is and is not exempt. `MaterialXCpp/` is exempt from **`ty`**, not
from the rest: it keeps its own `mxcpp` namespace, but its TU-contained code
becomes `static` at `mxcpp` scope like everywhere else. Only the two vendored
trees are exempt from the anonymous-namespace rule, and only because not
modifying vendored source is worth more than uniformity. `delegate/` is not part
of this sweep at all.

The point is ownership, not brevity. Inside `renderer/`, `HdEmbree` on every
type is noise that says nothing — every type there is hdEmbree's. `ty::` says
the same thing once, at the namespace, and leaves the type name free to describe
the type. It also makes the one thing that *is* worth distinguishing visible:
a `delegate/` type appearing in a renderer signature keeps its `HdEmbree` prefix
and now stands out as the Hydra-facing thing it is.

## The TU-containment convention

Cross-TU and TU-contained code both live under `ty`. TU-contained code is
`static`, at `ty` scope, in the `.cpp`. **No anonymous namespaces.**

```cpp
PXR_NAMESPACE_OPEN_SCOPE
namespace ty {

static float _DotZeroClip(float x) { ... }
static constexpr float _kVolumePdfEps = 1e-7f;

}
PXR_NAMESPACE_CLOSE_SCOPE
```

So the complete rule, which `14` establishes and this plan finishes:

- `ty::Foo` — declared in a header, used by more than one translation unit.
- `static _Foo` at `ty` scope in a `.cpp` — contained to one translation unit.
- No anonymous namespaces.

`static` marks TU-containment at the declaration; an anonymous namespace marks
it at a block boundary that can be a thousand lines away. `lightSamplers.cpp`'s
is 1,465 lines long. Write `static` explicitly even on `const`/`constexpr`
objects, where namespace scope already implies internal linkage — uniformity is
the point, so that its *absence* means "shared".

### Types are the one gap

`static` settles functions and objects completely: internal linkage means the
symbol is never exported, so two `.cpp` files may define `static _Foo`
differently with no collision and no ODR question.

It cannot be applied to a `struct`, `class`, or `enum`. A TU-contained type is
therefore declared `_Foo` at `ty` scope with no `static`, and its name has
external linkage — two `.cpp` files defining *different* types under the same
qualified name is an ODR violation, and one the compiler is not required to
diagnose.

How much that matters depends on whether the type emits anything. Measured over
the six TU-contained types in the renderer today:

| Type | File | Kind | Emits |
| --- | --- | --- | --- |
| `_SssWalkState` | `integrator/sss.cpp:42` | data aggregate | nothing |
| `_SssTraceResult` | `integrator/sss.cpp:76` | data aggregate | nothing |
| `_WireframeParametricFrame` | `integrator/surfaceShading.cpp:28` | data aggregate | nothing |
| `_ShapeSample` | `lights/lightSamplers.cpp:130` | data aggregate | nothing |
| `_ColorSpaceResolution` | `materials/oiioTextureSystem.cpp:122` | `enum class` | nothing |
| `_ScopedThreadScheduler` | `renderer.cpp:51` | class, inline TBB constructor | **weak/COMDAT symbols** |

Five of the six have no member functions, no virtuals, and no static data, so
they emit no symbols and a duplicate name could not mislink even in principle.
`_ScopedThreadScheduler` is the only one whose implicitly-inline constructor and
destructor become weak symbols the linker would resolve arbitrarily.

So the exposure is one class with an already-distinct name, not a standing
hazard — `static` plus the tree-uniqueness check in the validation section is
proportionate, and per-module detail namespaces are not (see "Rejected" below).

`18` promotes `_ShapeSample` into `lightSamplerCommon.h` as a cross-TU
`ty::ShapeSample`, so five remain by the time this plan runs. Keep the count low
deliberately: prefer a function-local type where it is used inside one function,
and prefer promoting to a header over duplicating a name. Run the check rather
than trusting the table above to stay current.

## Ownership boundaries

| Tree | Namespace | Note |
| --- | --- | --- |
| `renderer/` generally | `ty` | this plan's scope |
| `renderer/materials/MaterialXCpp/` | `mxcpp` | already serves `ty`'s role; **no `ty::mxcpp`, no `mxcpp::ty`**. Anonymous namespaces still go — `static` at `mxcpp` scope |
| `renderer/pxrPbrt/` | `ty::pbrt` | Typhoon-owned adapted code, not a vendored tree — `pxr_pbrt` becomes `ty::pbrt`, keeping ownership explicit and provenance in the inner name. See the dead-code note below |
| `renderer/materials/BSDL/` | unchanged | vendored third party; do not touch |
| `renderer/lights/pxrIES/` | `pxr_ccl` (`ies.h`) and `PXR_NAMESPACE` (`pxrIES.h`) | vendored from Cycles via Pixar; retain both, do not rename `IESFile`/`PxrIESFile` |
| `delegate/` | `PXR_NAMESPACE` | Hydra-facing adapters; **out of scope** |

`delegate/` stays out on purpose. `HdEmbreeMesh` reads as "hdEmbree's
`HdMesh`" — the prefix carries role information that `ty::Mesh` loses, and these
are the classes a reader coming from Hydra looks for first. `HdEmbreeMesh`,
`HdEmbree_Light`, `HdEmbreeRenderBuffer`, `HdEmbreeInstancer`,
`HdEmbreeMaterial`, `HdEmbreeRenderDelegate`, `HdEmbreeRenderPass`,
`HdEmbreeRenderParam`, and `HdEmbreeInstancer` all keep their names and their
`PXR_NAMESPACE` scope.

### The registered types

`plugInfo.json` registers two C++ types by string name, resolved through
`TfType`:

| plugInfo.json | source |
| --- | --- |
| `HdEmbreeRendererPlugin` (`:9`) | `delegate/rendererPlugin.cpp:15` |
| `HdEmbree_ImplicitSurfaceSceneIndexPlugin` (`:33`) | `delegate/implicitSurfaceSceneIndexPlugin.cpp:23` |

`TfType::Define<T>()` derives the canonical name from the demangled C++ type, so
an enclosing `ty::` stops it matching `plugInfo.json`. The failure is a silent
no-plugin-found at load time, not a build error. Both are in `delegate/` and so
are already out of scope; this table exists so nobody moves them opportunistically.

`plugInfo.json`'s third entry, `HdEmbreeTyphoonRenderSettingsAPI`, is **not** a
C++ type at all. `schema/generatedSchema.classes.txt` has an empty
`# Public Classes` section — it is a codeless schema, so there is nothing to
place in a namespace and nothing to protect.

## Inventory — renderer types

Every type declared in a `renderer/` header outside the exempt trees, with its
target name. Reproduce this table before starting; it is the completion
criterion, and an illustrative list would not be checkable.

| Header | Current | Target |
| --- | --- | --- |
| `config.h` | `HdEmbreeConfig` | *deleted by `12`* |
| `geometry/context.h` | `HdEmbreeWireframeMode` | `ty::WireframeMode` |
| | `HdEmbreePrototypeContext` | `ty::PrototypeContext` |
| | `HdEmbreeInstanceContext` | `ty::InstanceContext` |
| `geometry/displacementEvaluation.h` | `HdEmbreeDisplacedSubdivFrame` | `ty::DisplacedSubdivFrame` |
| `geometry/meshSamplers.h` | `HdEmbreeRTCBufferAllocator` | `ty::RtcBufferAllocator` |
| | `HdEmbreeConstantSampler` | `ty::ConstantSampler` |
| | `HdEmbreeUniformSampler` | `ty::UniformSampler` |
| | `HdEmbreeTriangleVertexSampler` | `ty::TriangleVertexSampler` |
| | `HdEmbreeTriangleFaceVaryingSampler` | `ty::TriangleFaceVaryingSampler` |
| | `HdEmbreeSubdivSampler` | `ty::SubdivSampler` |
| | `HdEmbreeSubdivVertexSampler` | `ty::SubdivVertexSampler` |
| | `HdEmbreeSubdivVaryingSampler` | `ty::SubdivVaryingSampler` |
| | `HdEmbreeSubdivFaceVaryingSampler` | `ty::SubdivFaceVaryingSampler` |
| `geometry/primvarSampler.h` | `HdEmbreeTypeHelper` | `ty::TypeHelper` |
| | `HdEmbreeBufferSampler` | `ty::BufferSampler` |
| | `HdEmbreePrimvarSampler` | `ty::PrimvarSampler` |
| `geometry/primvarSampling.h` | `HdEmbreePrimvarLookup` | `ty::PrimvarLookup` |
| | `HdEmbreeSubdivTexcoordJacobian` | `ty::SubdivTexcoordJacobian` |
| `geometry/wireframe.h` | `HdEmbreeWireframeSample` | `ty::WireframeSample` |
| | `HdEmbreeSubdivWireframeTopology` | `ty::SubdivWireframeTopology` |
| `integrator/medium.h` | `MediumTransportModel` | `ty::MediumTransportModel` |
| | `AdobeOpenPbrVolumeProperties` | `ty::AdobeOpenPbrVolumeProperties` |
| | `MediumProperties` | `ty::MediumProperties` |
| `integrator/sss.h` | `HdEmbreeSssInput` | `ty::SssInput` |
| | `HdEmbreeSssOutput` | `ty::SssOutput` |
| `lights/light.h` | `HdEmbree_UnknownLight` | `ty::UnknownLight` |
| | `HdEmbree_Cylinder` | `ty::CylinderLight` |
| | `HdEmbree_Disk` | `ty::DiskLight` |
| | `HdEmbree_Distant` | `ty::DistantLight` |
| | `HdEmbree_Dome` | `ty::DomeLight` |
| | `HdEmbree_Rect` | `ty::RectLight` |
| | `HdEmbree_Sphere` | `ty::SphereLight` |
| | `HdEmbree_LightVariant` | `ty::LightVariant` |
| | `HdEmbree_LightTexture` | `ty::LightTexture` |
| | `HdEmbree_IES` | `ty::IesShaping` |
| | `HdEmbree_DirectionalShapingDistribution` | `ty::DirectionalShapingDistribution` |
| | `HdEmbree_DirectionalShapingSample` | `ty::DirectionalShapingSample` |
| | `HdEmbree_Shaping` | `ty::Shaping` |
| | `HdEmbree_LightData` | `ty::LightData` |
| `lights/lightLinking.h` | `HdEmbreeCategorySet` | `ty::CategorySet` |
| `lights/lightRegistry.h` | `HdEmbreeLightRegistry` | `ty::LightRegistry` |
| `lights/lightSampler*.h` | `HdEmbreeLightSampler` | `ty::LightSampler` |
| `materials/material.h` | `HdEmbreeMaterialData` | `ty::MaterialData` |
| `materials/materialEvalContext.h` | `HdEmbreeMaterialEvalServices` | `ty::MaterialEvalServices` |
| `materials/oiioTextureSystem.h` | `HdEmbreeOiioTextureSystem` | `ty::OiioTextureSystem` |
| `renderBuffer.h` | `HdEmbreeRenderBufferInterface` | `ty::RenderBufferInterface` |
| `renderer.h` | `HdEmbree_RayMask` | `ty::RayMask` |
| | `HdEmbreeRayDifferential` | `ty::RayDifferential` |
| | `HdEmbreeCameraDepthOfField` | `ty::CameraDepthOfField` |
| | `HdEmbreeMediumState` | `ty::MediumState` |
| | `_HeroWavelengthState` | `ty::HeroWavelengthState` |
| | `HdEmbreeRenderer` | `ty::Renderer` |
| `sampling/sampling.h` | `HdEmbreeSamplerSequence` | `ty::SamplerSequence` |
| | `HdEmbreeSampleDomainKey` | `ty::SampleDomainKey` |
| | `HdEmbreeOpenQMCVariant` | `ty::OpenQmcVariant` |
| | `HdEmbreeSampleDomain` | `ty::SampleDomain` |
| | `HdEmbreeSampler` | `ty::Sampler` |

Notes on individual rows:

- **`HdEmbree_Rect` → `ty::RectLight`, not `ty::Rect`.** Dropping the prefix
  from the light shapes leaves names too generic to survive contact with `GfRect`
  and friends. `18` already applies this reasoning to filenames
  (`rectLight.cpp`, not `rect.cpp`) for the same reason; keep type and file
  naming aligned. `HdEmbree_IES` → `ty::IesShaping` for the same reason plus
  the all-caps convention.
- **`HdEmbreeSampleDomain` is declared twice** — `integrator/sss.h` and
  `sampling/sampling.h`. Confirm they are the same type and that one is a
  forward declaration before renaming; if they are genuinely distinct types
  sharing a name, that is a defect to report, not to preserve.
- **`_HeroWavelengthState`** is the one existing type carrying a `_` prefix at
  namespace scope in a header, which the rule reserves for TU-local code. It
  becomes `ty::HeroWavelengthState`. `14` moves its two `HdEmbreeRenderer`
  member users into `heroWavelength.h`; those become `ty::Renderer` members.
- **Forward declarations** in `renderer.h` (`ShadingContext`, `SurfaceClosure`,
  `TextureSystem`, `AdobeOpenPbrPreparedSurface`) and
  `geometry/displacementEvaluation.h` (`HdEmbreePrototypeContext`) must move
  with their definitions or they will silently declare a *new* type in the wrong
  namespace and fail at link time rather than compile time. Check every
  forward declaration in the tree, not only the ones listed here.
- **`materials/materialEvalContext.h`'s `TextureSystem`** is an `mxcpp` type
  reached through a forward declaration; it stays in `mxcpp`.

## Inventory — non-type declarations

The invariant covers **every** declaration, not only types. The type table is
the largest single group but not the whole job; these categories also move, and
a type-only sweep would leave the invariant false:

| Category | Where | Target |
| --- | --- | --- |
| 22 `HdEmbree`-prefixed free functions declared in renderer headers — `HdEmbreeCompute*`, `HdEmbreeEvaluate*`, `HdEmbreeSample*`, `HdEmbreeBuild*`, the Dwivedi SSS group, `HdEmbreeMatchesLink`, `HdEmbreeMergeCategories` | `geometry/{displacementEvaluation,wireframe}.h`, `integrator/sss.h`, `lights/{light,lightLinking}.h` | `ty::Compute*` etc., prefix dropped |
| `HdEmbreeAovTokens` (`TF_DECLARE_PUBLIC_TOKENS`) | `renderer.h:52` | `ty::AovTokens`; check the macro expands correctly inside a nested namespace before committing |
| `HdEmbreePrimvarSamplingDetail`, an existing **named** detail namespace | `geometry/primvarSampling.h:124-139` | `ty::PrimvarSamplingDetail` — it is already the shape this plan wants, only misprefixed |
| `mxcppAdapter.h`, `debugCodes.h` declarations | `materials/mxcppAdapter.h`, `debugCodes.h` | `ty::`, prefix dropped. `debugCodes.h`'s `TF_DEBUG_CODES` entry `HDEMBREE_LIGHT_CREATE` is a **runtime-visible name** — leave it alone |
| Constants and tokens surviving `12` and `14` | wherever `14` left them | `ty::`, `_`-prefixed and `static` if TU-contained |

Regenerate the free-function list rather than trusting the count; `12`, `14`,
`17`, and `18` all add and remove declarations before this plan runs.

`pxrPbrt/pbrtUtils.h` moves `pxr_pbrt` → `ty::pbrt`. It is adapted pbrt-v4 code
(Apache-2.0, upstream copyright retained in the header), not a vendored tree, so
Typhoon owns it and the invariant applies; the inner `pbrt` keeps the provenance
that the outer `ty` would otherwise erase. Its four symbols are `pi<T>`,
`SphericalDirection`, `SampleUniformCone`, and `InvUniformConePDF`. The `pi<T>`
variable template is `23`'s to collapse, not this plan's.

> **Check whether this file should exist first.** Nothing in the tree includes
> `pbrtUtils.h` — it appears only in `CMakeLists.txt`'s header list and in
> `ARCHITECTURE.md`. If it is genuinely dead, deleting it is strictly better
> than renaming it, and that decision belongs to `09-plan-comments-deadcode.md`.
> Resolve it before doing the rename; do not migrate dead code and then delete
> it a plan later.

## Inventory — anonymous namespaces

Measured on the current tree: **34 blocks across 30 files**.

| Bucket | Blocks | Files | Disposition |
| --- | --- | --- | --- |
| Typhoon-owned renderer | 15 | 12 | `static` at `ty` scope |
| `MaterialXCpp/` | 17 | 16 | `static` at `mxcpp` scope |
| Vendored `BSDL/`, `pxrIES/` | 2 | 2 | unchanged |

The 12 Typhoon-owned files, with block counts:

`aov/aovOutput.cpp` (1), `geometry/displacementEvaluation.cpp` (1),
`geometry/meshSamplers.cpp` (1), `geometry/wireframe.cpp` (1),
`integrator/medium.cpp` (1), `integrator/sss.cpp` (**2**),
`integrator/surfaceShading.cpp` (1), `lights/lightSamplers.cpp` (1),
`materials/mxcppAdapter.cpp` (1), `materials/oiioTextureSystem.cpp` (1),
`renderer.cpp` (1), `rendererImpl.h` (**3**).

`rendererImpl.h` is deleted by `14` before this plan runs. Each remaining block
loses its `namespace { ... }` wrapper: every function and object inside gains
`static`, the six types listed above cannot, and the whole block moves to `ty`
scope.

**Regenerate these numbers before starting.** `14` deletes three blocks, `17`
splits one file into twelve, and `18` splits another into nine; the counts
above will be wrong by then. Use the permissive pattern — `namespace\s*\{`,
not `^namespace {` — because it is what catches `namespace{`, indented blocks,
and a brace on the following line. An anchored pattern undercounts, which is
how `aov/aovOutput.cpp` was missed in an earlier draft of this plan.

**Two headers contain an anonymous namespace** — `rendererImpl.h`, which `14`
deletes, and `MaterialXCpp/nodes/helpers/proceduralHelpers.h`. In a header this
is not a style question: internal linkage means one copy per includer, which is
exactly why `rendererImpl.h` constructs its seven `static const TfToken`s ten
times at static-init. Fix `proceduralHelpers.h` here or raise it against `17`.

## Rejected: per-module named detail namespaces

An alternative for TU-contained code was to abolish anonymous namespaces in
favour of `static` for functions and objects **plus** a named per-module
namespace for types — `ty::pathIntegratorDetail::PathState`. Rejected, but the
reasoning is worth recording because it is a close call and the codebase
contains one instance of the pattern already
(`HdEmbreePrimvarSamplingDetail`).

For: qualified names are distinct by construction, so no uniqueness check is
needed. It is fair to note that external linkage *alone* is not an ODR
violation — only duplicate qualified names are — and a mandated unique module
namespace prevents those structurally.

Against: it was an alternative to `static`, and `static` is strictly better
wherever it applies — it marks containment at the declaration and needs no
invented name. That leaves the detail namespace covering only the types, where
it costs one invented namespace per file for **five** of them, four of which
emit no symbols and one of which is an enum. Machinery in exchange for a check
that fits on one line, against Goal 1's requirement that an abstraction earn at
least twice its size. It also creates a third naming tier
(`ty::` / `ty::fooDetail::` / `_Foo`) where two suffice.

Revisit if check 4 ever fires, or if a TU-contained type acquires virtuals,
out-of-line members, or static data — that is the point at which a duplicate
name stops being inert.

## Order

Migrate leaf-first so each commit builds and is separately bisectable:

1. `renderer/geometry/` — `context.h`, `primvarSampler.h`, `primvarSampling.h`,
   `meshSamplers.h`, `wireframe.h`, `displacementEvaluation.h`.
2. `renderer/sampling/` — `sampling.h`.
3. `renderer/lights/` — `light.h`, `lightLinking.h`, `lightRegistry.h`, and
   whatever `18` leaves in `lightSampler*`.
4. `renderer/integrator/` — `medium.h`, `sss.h`.
5. `renderer/materials/` — `material.h`, `materialEvalContext.h`,
   `oiioTextureSystem.h`. `MaterialXCpp/` is step 9, not here.
6. `renderer/renderBuffer.h`.
7. `renderer/renderer.h`, `HdEmbreeRenderer` → `ty::Renderer`, and the modules
   `14` left behind. This is the widest commit: `renderer.h` is included by 15
   files, among them `delegate/renderPass.h` and `delegate/renderDelegate.h`,
   which hold `ty::Renderer` by value.
8. **Anonymous-namespace removal across Typhoon-owned `renderer/` sources.**
   Every block becomes `static` at `ty` scope; no block is nested or retained.
   Regenerate the file list first — `14` deletes three blocks, `17` splits one
   file into twelve, `18` splits another into nine, so any count written here
   is stale by the time this runs. Process every non-vendored block the check in
   step 10 reports, not a fixed list.
9. **`MaterialXCpp/`.** No renaming and no `ty` — this step is only the
   anonymous-namespace removal, to `static` at `mxcpp` scope. It is scheduled
   separately because it is the largest single bucket and shares no files with
   steps 1-8. Covers:
   - the `MaterialXCpp/*.cpp` blocks, including whatever `17` leaves in
     `materials/bsdf/`;
   - `nodes/helpers/proceduralHelpers.h`, the one header violation — internal
     linkage in a header means one copy per includer, which is the defect, not
     the style;
   - the five test sources under `MaterialXCpp/tests/`, which sit at **global
     scope** with `PXR_NAMESPACE_USING_DIRECTIVE` rather than inside `mxcpp`;
     their TU-contained helpers become `static` in place. Do not move test
     bodies into `mxcpp`.
   - **`main()` is the language-required exception.** `testMaterialXCppMain.cpp:62`
     defines it at global scope and it must stay there — a namespaced `main` is
     not a program entry point. Its supporting declarations may move; the
     function may not. Record this in the plan's completion criteria so the
     owner-namespace check does not flag it as a miss.
10. **Run the Validation checks** and fix what they report: the
    anonymous-namespace search, the three prefix searches, the TU-contained type
    uniqueness search, and the owner-namespace inventory. This is a step in the
    sequence, not a post-hoc audit — the completion criteria require them to
    return empty, so the last commit is the one that makes them do so.

`delegate/` is edited throughout — every use site of a renamed renderer type
needs `ty::` qualification — but no `delegate/` type is renamed.

## Validation

Mechanical checks, all expected to return nothing. Run from the plugin root.
`rg`'s `-g` patterns match the path **as printed**, so when the search path is
`renderer` they must be written `renderer/...` — a bare `materials/BSDL/**`
silently fails to exclude anything.

```sh
VENDORED=(-g '!renderer/materials/BSDL/**' -g '!renderer/lights/pxrIES/**')

# 1. No anonymous namespace in Typhoon-owned or MaterialXCpp code, headers and
#    sources alike. The permissive pattern matters: `namespace{`, an indented
#    block, and a brace on the following line all count.
rg -Un 'namespace\s*\{' renderer "${VENDORED[@]}"

# 2. No Typhoon-owned declaration still carrying the prefix. Match the
#    declaration, not the line: filtering whole lines would drop a line that
#    mentions a permitted delegate type *and* a stale renderer one.
rg -n '^\s*(struct|class|enum class|enum|using)\s+HdEmbree' renderer \
    "${VENDORED[@]}" -g '!renderer/materials/MaterialXCpp/**'
rg -n '\bHdEmbree[A-Za-z0-9_]*\s*\(' renderer \
    "${VENDORED[@]}" -g '!renderer/materials/MaterialXCpp/**'
rg -n 'namespace\s+HdEmbree' renderer "${VENDORED[@]}"

# 3. TU-contained type names are unique across all non-vendored sources. These
#    carry external linkage because `static` cannot apply to a type.
rg -N --no-filename -o '^\s*(struct|class|enum class|enum)\s+_\w+' renderer -g '*.cpp' \
    "${VENDORED[@]}" | awk '{print $NF}' | sort | uniq -d
```

Check 2 is split into three narrow searches rather than one broad search with a
whole-line exclusion list. Permitted `delegate/` types (`HdEmbreeMesh`,
`HdEmbree_Light`, `HdEmbreeRenderDelegate`, and the rest of the list under
"Ownership boundaries") legitimately appear in renderer signatures; a
line-level `rg -v` would suppress any line that mentioned one, hiding a stale
renderer declaration sharing that line.

**Check 4 — owner namespace — is a reviewed inventory, not a grep.** A
`grep for 'namespace ty'` heuristic produces false positives on every
documented exception, so write the exception list down instead and check the
remainder by eye once:

| Exception | Why |
| --- | --- |
| `renderer/materials/MaterialXCpp/**` | `mxcpp`, not `ty` |
| `MaterialXCpp/tests/*` | global scope with `PXR_NAMESPACE_USING_DIRECTIVE` |
| `testMaterialXCppMain.cpp:62` | `main()` cannot be namespaced |
| `renderer/materials/BSDL/**`, `renderer/lights/pxrIES/**` | vendored |
| files declaring nothing (pure includes, forwarding `.cpp`) | nothing to own |

Everything not on that list must open `namespace ty`. Check 3's uniqueness
search exists specifically to prevent duplicate namespace-scope `_Foo` **type**
names; it is not a claim that such types are otherwise unsafe.

Plus:

- Clean build per migration commit — that is what catches a forward declaration
  left behind in the old namespace, which fails at link time rather than compile
  time.
- Full focused test run; the test targets name these types directly. Note that
  `testMaterialXCpp` compiles `renderer/integrator/medium.cpp`,
  `renderer/integrator/sss.cpp`, and `renderer/materials/mxcppAdapter.cpp`
  directly into its binary, so it is affected by steps 4 and 5 even though it
  does not link `hdEmbree`.
- Launch `usdview` and `usdrender`; verify plugin **and** schema discovery.
  Nothing in this plan should be able to break either, which is exactly why it
  is worth checking — a stray rename of a `delegate/` registered type fails
  silently at load time.
- Fixed-seed render comparison across several workloads: material-heavy,
  texture-heavy, many-light, deep-path. This is a pure rename and must be
  **bit-identical**. Any difference is a bug, not a tolerance.

## Completion criteria

- Every row of the type inventory is at its target name, and every category in
  the non-type inventory is migrated.
- The anonymous-namespace, prefix, and type-uniqueness searches return empty,
  and the owner-namespace inventory has been walked with every exception on the
  documented list.
- **Zero anonymous namespaces** in Typhoon-owned renderer and MaterialXCpp
  code, headers and sources alike, including `proceduralHelpers.h`. Vendored
  `BSDL/` and `pxrIES/` unchanged.
- Every TU-contained function and object is `static`; every TU-contained type
  has a tree-unique `_Foo` name.
- `delegate/` type names, the `HDEMBREE_LIGHT_CREATE` debug code, `main()` at
  global scope in `testMaterialXCppMain.cpp`, and the two
  `plugInfo.json`-registered types are unchanged, with plugin and schema
  discovery verified.
- `renderer/pxrPbrt/` is either at `ty::pbrt` or deleted as dead code — not left
  at `pxr_pbrt`.
- `AGENTS.md` records the complete membership rule: `ty::Foo` for header-declared
  cross-TU symbols, `static _Foo` for TU-contained ones, the type gap and its
  uniqueness requirement, and no anonymous namespaces.
- The TU-contained type uniqueness search is wired into CI, not run once — it is
  the only thing standing between a newly added TU-contained type and a silent
  duplicate.
- Bit-identical renders.

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
