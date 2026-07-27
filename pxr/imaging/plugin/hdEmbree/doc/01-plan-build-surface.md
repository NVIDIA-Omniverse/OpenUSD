# Plan: Make hdEmbree's installed build surface private

Status: build-configuration and documentation change. **Edits `CMakeLists.txt`
and prose only — zero source files.** Lands first, before Phase A.

Sequencing: no code dependency on any plan. Every later plan that adds or moves
a file already edits the same `CMakeLists.txt` lists; after this plan those
lists are uniformly private, so new files land private by default instead of
requiring a public/private judgement that a later plan has to reverse. `12`
removes one entry (`renderer/config`) and `17`/`18` add several; all are
one-line rebases in either direction.

This plan does **not** touch namespaces. `19-plan-ty-namespace.md` owns the
`ty` type rename, and the two are independent — see "Why this is not the `ty`
plan" below.

## Issue

`CMakeLists.txt` marks 45 entries `PUBLIC_CLASSES` and 25 `PUBLIC_HEADERS`,
against 4 `PRIVATE_CLASSES`. Public means installed: renderer orchestration,
light registry and samplers, material graph internals, closure types, every
MaterialX node-family registration unit, geometry contexts, the AOV bridge, and
implementation helpers all ship as headers under
`include/pxr/imaging/hdEmbree/`, implying support commitments for code
`ARCHITECTURE.md` describes as internal.

Nothing consumes them. A repository-wide search for
`pxr/imaging/plugin/hdEmbree/` includes returns hits only from inside the
plugin directory itself. The in-tree test targets reach the code three
different ways, none of them through the install tree:

- `testHdEmbree`, `testHdEmbreeLightSamplers`, `testHdEmbreeWireframe`,
  `testHdEmbreeSubdivision`, `testHdEmbreeLightLinking`, and
  `testHdEmbreeRenderSettings` link the `hdEmbree` target;
- `testMaterialXCpp` links **no** hdEmbree target — it compiles renderer
  sources directly into its own binary (`renderer/integrator/medium.cpp`,
  `renderer/integrator/sss.cpp`, `renderer/materials/mxcppAdapter.cpp`, and the
  seven `MaterialXCpp/materials/*.cpp`);
- `testHdEmbreeSampling` links only `tf` and compiles one test source, reaching
  `sampling.h` purely through the copied-header include path.

All three work identically after the conversion, because `_copy_headers()`
copies public and private headers to the same place.

The surface is also unmanaged rather than merely over-broad. The current Pixi
environment's `include/pxr/imaging/hdEmbree/` contains both the present layout
(`delegate/`, `renderer/geometry/`, …) and stale flat copies from before the
source reorganization — including `renderer/sampler.h`, which exists nowhere in
the source tree. `install` never removes anything, so nobody has been able to
see what is actually shipped.

## The decision this plan makes

**hdEmbree exposes no supported hand-written C++ API.** The plugin is loaded by
Hydra through `plugInfo.json`; it is not something an external project links
against or includes. Its contract is the runtime one in Part 2.

Everything follows mechanically from that: there is no classification exercise,
no per-entry judgement, no deprecation policy, and no external consumer to
migrate. MaterialXCpp is internal too — if it is ever wanted as a reusable
component it should be extracted into its own project with its own deliberate
façade, which is a different piece of work from this one.

## Goals

- Install no hand-written hdEmbree header.
- Write down the contract that *does* exist, so a future change knows what it
  can break.
- Remove extension-point language from the docs that describes an API nobody
  offers.
- Make the install tree auditable, so the stale-header drift above cannot
  recur unnoticed.

## Non-goals

- Do not split hdEmbree into multiple libraries.
- Do not hide `plugInfo.json`, the generated schema, or resource files.
- Do not rename anything — no types, no namespaces, no quantities.
  `19-plan-ty-namespace.md` and `05-plan-naming-core.md` own renames.

## Part 1 — CMake conversion

Move all 45 `PUBLIC_CLASSES` entries to `PRIVATE_CLASSES` and all 25
`PUBLIC_HEADERS` entries to `PRIVATE_HEADERS`. `RESOURCE_FILES`,
`DOXYGEN_FILES`, `INCLUDE_SCHEMA_FILES`, and `SCHEMA_CLASSES_FILE` are
unchanged.

The macro semantics are settled, not a risk to investigate. In
`cmake/macros/Private.cmake`:

- `_classes(${NAME} ... PUBLIC|PRIVATE)` differs *only* in which of
  `${NAME}_PUBLIC_HEADERS` / `${NAME}_PRIVATE_HEADERS` the `.h` is appended to.
  Both append the `.cpp` to `${NAME}_CPPFILES` identically, so every file still
  compiles into the target.
- `_copy_headers()` (`:1370`) receives `${args_PUBLIC_HEADERS}` and
  `${args_PRIVATE_HEADERS}` together with the same `PREFIX`, so both land in the
  build include tree at the same path.
- `install(FILES ...)` runs only over `args_PUBLIC_HEADERS` (`:1436`).

Two consequences of that third point:

- **No source file changes.** Every
  `#include "pxr/imaging/plugin/hdEmbree/..."` resolves through the build
  include tree exactly as it does today, in plugin sources and in the
  `pxr_build_test` targets alike. This plan cannot change behavior, so its
  render-output validation is satisfied trivially — do not spend a fixed-seed
  image comparison on it.
- **Doxygen coverage narrows.** `_copy_doxygen_files()` (`:1406`) is fed
  `"${args_PUBLIC_HEADERS};${args_DOXYGEN_FILES}"`, so under
  `PXR_BUILD_DOCUMENTATION` hdEmbree will contribute `overview.dox` and nothing
  else. That is the intended outcome for a plugin with no API — record it as a
  decision in the commit message rather than discovering it later.

## Part 2 — Write down the real contract

Replace every "public C++ surface" statement in the docs with the contract that
can actually break a consumer. Produce it as a table in `ARCHITECTURE.md`, with
each row naming its authoritative source file:

| Contract | Source of truth |
| --- | --- |
| Renderer identity `Embree` (`usdview`/`usdrender -r`, `loadWithRenderer`) | `plugInfo.json` |
| `HdEmbreeRendererPlugin` type name, `HdRendererPlugin` base, priority | `plugInfo.json`, `delegate/rendererPlugin.cpp` |
| `HdEmbree_ImplicitSurfaceSceneIndexPlugin` type name, base, `loadWithRenderer` | `plugInfo.json`, `delegate/implicitSurfaceSceneIndexPlugin.cpp` |
| `TyphoonRenderSettingsAPI` schema identity, auto-apply to `RenderSettings` | `plugInfo.json`, `schema/generatedSchema.usda` |
| The 27 `ty:` attribute names, types, defaults, and allowed tokens | `schema/schema.usda`, `HdEmbreeRenderDelegate::_Initialize()` |
| Generic unnamespaced settings (`domeLightCameraVisibility`) and the namespace list | `HdEmbreeRenderDelegate::GetRenderSettingsNamespaces()` |
| Material render-context tokens | `HdEmbreeRenderDelegate::GetMaterialRenderContexts()` |
| Supported AOV names | `renderer/aov/aovOutput.cpp` |
| The two renderer identifiers RenderLab matches on (`"HdEmbreeRendererPlugin"`, `"Embree"`) | `renderLab/renderSettingsMetadata.py:11-12` |
| RenderLab setting keys, a **subset** of the delegate descriptors | `renderLab/renderSettingsMetadata.py` |

These are what break stages, RenderLab, plugin discovery, and the delegate.
Installed headers are not on the list.

State the configuration boundary precisely, because it is easy to overstate:

> The USD `TyphoonRenderSettingsAPI` schema is hdEmbree's supported external
> settings interface. Hydra's direct delegate-settings path is an internal
> application-control path, required by usdview and RenderLab, and is not a
> consumer-facing C++ API.

Do **not** describe RenderLab as authoring USD. It calls the generic
`StageView.SetRendererSetting()` path (`renderSettingsEditor.py:345`, `:362`)
deliberately. `12` states the same boundary in `ARCHITECTURE.md`; keep the two
statements identical rather than paraphrased.

RenderLab's key set is a subset by design: it carries explicit metadata for 25
of the 27 `ty:` attributes, and `ty:disableShadows` and `ty:textureCacheSize`
fall through to the default category. No RenderLab key names a nonexistent
attribute. That is the invariant to preserve — subset, never superset.

Note for the implementer: `schema/generatedSchema.classes.txt` has an empty
`# Public Classes` section. `TyphoonRenderSettingsAPI` is a **codeless** schema
— there is no generated C++ class, only plugin metadata. Do not describe it as a
source type.

## Part 3 — Documentation

- `ARCHITECTURE.md`: add the Part 2 contract table; delete any extension-point
  language describing hand-written headers as consumer-facing.
- `AGENTS.md`: `Source Boundary` gains one line — no hdEmbree header is
  installed; `renderer/rendererImpl.h`'s existing "do not treat it as an
  installed API or extension point" note generalizes to the whole plugin.
- `overview.dox`: becomes the plugin's only doxygen input; make sure it reads
  as a plugin overview rather than an API index.
- `README.md` is **not** in scope. Its configuration-precedence text is owned
  by `11`/`12`.

## Validation

**Header audit, against a staged install.** A normal install does not remove
headers an older build wrote, so auditing the active Pixi environment would
report stale files as current — `renderer/sampler.h` there is the worked
example. Stage into a guaranteed-empty directory:

```sh
STAGE=$(mktemp -d)
DESTDIR="$STAGE" pixi run cmake --build build --target install
```

`mktemp -d`, not a fixed path: a stale directory from an earlier run would
defeat the audit by making removed headers look present.

**`DESTDIR` prepends, it does not replace.** The staged tree is at
`$STAGE/$CMAKE_INSTALL_PREFIX/...`, and this build's prefix is `$CONDA_PREFIX`
— an absolute path several levels deep — so the files are *not* directly under
`$STAGE/include`. Resolve it rather than assuming:

```sh
PREFIX=$(pixi run printenv CONDA_PREFIX)
ROOT="$STAGE$PREFIX"
RES="$ROOT/plugin/usd/hdEmbree/resources"

test ! -e "$ROOT/include/pxr/imaging/hdEmbree" && echo "no headers installed: PASS"
ls "$ROOT/plugin/usd/hdEmbree.so" \
   "$RES/plugInfo.json" \
   "$RES/schema.usda" \
   "$RES/generatedSchema.usda"
```

Confirm no `include/pxr/imaging/hdEmbree/` exists at all, and that the plugin
DSO, `plugInfo.json`, and **both** schema resources are present. Presence is the
right bar: this change edits only header visibility, so the resource files
cannot have changed content — do not write a comparison the check does not
actually perform. Delete `$STAGE` afterwards.

**Focused builds and tests.** Build each test target explicitly; a failure here
most likely means a target is missing its dependency on the copied-header step,
not that it was including from the install tree — check which before assuming.

```sh
for t in testHdEmbreeSampling testMaterialXCpp testHdEmbreeRenderSettings \
         testHdEmbreeWireframe testHdEmbreeSubdivision testHdEmbreeLightLinking; do
    pixi run cmake --build build --target $t
done
pixi run ctest --test-dir build -R 'testHdEmbree|testMaterialXCpp' --output-on-failure
```

`testHdEmbree` and `testHdEmbreeLightSamplers` are guarded by
`if (X11_FOUND OR APPLE)` (`CMakeLists.txt:166`), so they may not exist in this
configuration. Add them only if configure created them, which you can ask the
build system directly rather than guessing:

```sh
for t in testHdEmbree testHdEmbreeLightSamplers; do
    pixi run cmake --build build --target help 2>/dev/null | grep -qx "\.\.\. $t" \
        && pixi run cmake --build build --target $t \
        || echo "skipped (not configured): $t"
done
```

An absent target here is a configuration fact, not a failure.

Note the `-R` filter: an unfiltered `ctest --test-dir build` runs the entire
configured OpenUSD suite, not the hdEmbree tests.

**Runtime discovery, from the normal Pixi install.** The staged tree above is a
header-and-resource audit only; do not try to run from it. Run `pixi run build`
as usual, then launch `usdview` and `usdrender` and verify **both** plugin
discovery (renderer appears in the list, renders) and schema discovery (a `ty:`
attribute authored on a `RenderSettings` prim actually takes effect). A
registration mistake surfaces only here.

Because the Pixi environment retains stale headers from earlier builds, runtime
discovery there proves the plugin still loads but says nothing about what is
installed — that is what the staged audit is for. The two checks are
complementary and neither substitutes for the other.

**RenderLab.** Open it and confirm the settings list populates, then check the
subset invariant: every metadata key names a real delegate/schema setting, and
the settings without explicit metadata (`ty:disableShadows`,
`ty:textureCacheSize`) still appear under the default category. Confirm both
renderer identifiers (`"HdEmbreeRendererPlugin"`, `"Embree"`) still resolve.

## Risks

- A test may turn out to include from the install tree. Fix the include path;
  do not restore public status to keep it building.
- `PXR_BUILD_MONOLITHIC` bakes plugins into the monolithic library. Header
  visibility does not affect that path, but build it once if it is a
  configuration you care about.

## Completion criteria

- `CMakeLists.txt` has no `PUBLIC_CLASSES` or `PUBLIC_HEADERS` entry.
- No source file changed in this commit.
- A `DESTDIR`-staged install into a `mktemp -d` directory contains no
  `include/pxr/imaging/hdEmbree/`, and still contains the plugin DSO,
  `plugInfo.json`, and both schema resource files.
- Every hdEmbree test target that the current configuration defines builds and
  passes with no include-path edits.
- Plugin **and** schema discovery verified from the normal `pixi run build`
  install — not from the staged tree, which is an audit artifact only.
- RenderLab populates, and its metadata keys remain a subset of the delegate
  descriptors.
- `ARCHITECTURE.md` carries the contract table; no document describes a
  hand-written hdEmbree header as an extension point.

## Why this is not the `ty` plan

An earlier draft combined this CMake change with the `ty` namespace and type
rename, on the claim that build visibility and namespace placement must express
the same boundary. That claim is unsupported: private code can be named
`HdEmbreeMesh`, and namespace placement controls neither installation nor
support commitments. The two are independently motivated — this plan by a real
defect in what ships, `19` by namespace ownership — and have wildly different
diffs. Combining them would have put a zero-source-file build fix behind a
rename over every file in `renderer/`, and would have made this plan
un-landable until `14`, `17`, and `18` had all shipped.

`19-plan-ty-namespace.md` carries the rename. It is required, and it depends on
nothing here.

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
