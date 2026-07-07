# hdEmbree Agent Guide

This directory contains the `hdEmbree` Hydra render delegate plugin. It is a
CPU path tracer built on Embree 4 with MaterialX/OpenPBR shading, USD Lux light
support, Hydra render settings, and stage-authored `UsdRender` product output.

Use this file to get oriented quickly before changing code in this plugin.

Important maintenance rule: agents MUST keep this `AGENTS.md` up to date as
hdEmbree, RenderLab, usdrender, build tasks, tests, or surrounding USD/Hydra
integration change. If a code change invalidates or adds workflow knowledge,
update this file in the same change.

## Current Build And Run Workflow

The current repo workflow is Pixi-based. The task definitions live in the root
`pixi.toml`.

- Configure the CMake build:
  `pixi run configure`
- Build and install into the Pixi environment:
  `pixi run build`
- Configure an optimized profiling build with debug information and frame
  pointers in the separate `build-profile` tree:
  `pixi run configure-profile`
- Build and install the profiling version of hdEmbree:
  `pixi run build-profile`
- Launch usdview with hdEmbree:
  `pixi run usdview <stage.usda>`
- Render stage-authored products with hdEmbree:
  `pixi run usdrender <stage.usda>`
- Render stage-authored products under a separate output directory:
  `pixi run usdrender --outputRoot <dir> <stage.usda>`
- Run legacy frame recording:
  `pixi run usdrecord <stage.usda> <output-image>`

The Linux `configure` task currently runs CMake with Ninja, Release mode, and:

- `-DPXR_BUILD_EMBREE_PLUGIN=TRUE`
- `-DPXR_BUILD_OPENIMAGEIO_PLUGIN=TRUE`
- `-DPXR_OIIO_PLUGIN_ENABLED=TRUE`
- `-DPXR_ENABLE_OPENQMC_SUPPORT=TRUE`
- `-DPXR_HDEMBREE_ENABLE_OPENQMC=2`
- `-DCMAKE_INSTALL_PREFIX=$CONDA_PREFIX`

The Linux `configure-profile` task leaves the normal `build` tree untouched
and configures `build-profile` with `RelWithDebInfo`, `-O3`, debug information,
and frame pointers. `build-profile` installs only the hdEmbree subtree into the
Pixi environment, replacing the active plugin while retaining the regular
Release OpenUSD runtime. Run the normal `pixi run build` task to restore the
Release plugin after profiling.

The Windows Pixi task uses the same core Embree/OIIO/OpenQMC options, disables
precompiled headers, suppresses CMake regeneration, adds `/utf-8`, and installs
to `$CONDA_PREFIX`.

The `build` task runs `cmake --build build --target install` and writes an
`openusd-pxr.pth` file so Python imports resolve to the installed OpenUSD Python
modules inside the Pixi environment. Prefer running direct CMake or test
commands through Pixi so the same dependency, plugin, and Python environment is
used:

- `pixi run cmake --build build --target hdEmbree`
- `pixi run cmake --build build --target testHdEmbreeRenderSettings`
- `pixi run ctest --test-dir build -R testHdEmbreeRenderSettings --output-on-failure`

Relevant Pixi-managed dependencies include Python 3.11, Embree 4.4, OpenQMC
0.7.1, OpenImageIO 2.5, OpenSubdiv, TBB, PySide6, PyOpenGL, CMake, Ninja, and
compilers.

## Profiling hdEmbree

Use the profile build when investigating CPU performance:

```sh
pixi run configure-profile
pixi run build-profile
```

The first build compiles a separate dependency closure and can take several
minutes; later builds are incremental. To rebuild without rerunning the Pixi
task dependency, use `pixi run --skip-deps build-profile`. Confirm that the
installed plugin has line-level debug information before collecting a profile:

```sh
readelf -S .pixi/envs/default/plugin/usd/hdEmbree.so \
    | rg 'debug_info|debug_line'
```

Linux `perf` must be allowed to sample the renderer. Check
`kernel.perf_event_paranoid` if `perf` reports a permissions error. Obtain the
exact command for one test without rendering it:

```sh
cd /home/anders/code/typhoon-tests
pixi run pytest material-fidelity \
    --typhoon-provider /home/anders/code/openusd-omniverse \
    -k <case-name> --typhoon-dry-run -s
```

Profile the printed `usdrender` command directly instead of profiling pytest,
FLIP comparison, and report generation. Set `HDEMBREE_RANDOM_NUMBER_SEED` to a
fixed value. Keep resolution, sample count, bounce count, adaptive sampling,
and other scene-authored render settings unchanged between measurements.
Scene-authored `ty:` settings take precedence over environment-backed defaults.
Run the profiling commands below from `/home/anders/code/openusd-omniverse` so
the `$PWD/.pixi` path identifies the provider environment.

Use at least five `perf stat` repetitions for before-and-after measurements:

```sh
pixi run --clean-env -x /usr/bin/env \
    PATH="$PWD/.pixi/envs/default/bin:/usr/bin:/bin" \
    HDEMBREE_RANDOM_NUMBER_SEED=1 \
    /usr/bin/perf stat -r 5 -d -o /tmp/hdembree-stat.txt -- \
    usdrender --complexity high --renderer Embree --disableCameraLight \
    <stage.usda> --outputRoot /tmp/hdembree-profile-stat
```

Collect call stacks for the renderer and all worker threads with:

```sh
pixi run --clean-env -x /usr/bin/env \
    PATH="$PWD/.pixi/envs/default/bin:/usr/bin:/bin" \
    HDEMBREE_RANDOM_NUMBER_SEED=1 \
    /usr/bin/perf record -o /tmp/hdembree.data \
    -F 499 -e cycles:u -g --call-graph fp -- \
    usdrender --complexity high --renderer Embree --disableCameraLight \
    <stage.usda> --outputRoot /tmp/hdembree-profile-record

perf report -i /tmp/hdembree.data
```

The profile build uses frame pointers so `--call-graph fp` has lower overhead
than DWARF unwinding. Use a self-cost report to find leaf hotspots and an
inclusive/children report to understand their calling paths. On hybrid Intel
CPUs, inspect the `cpu_core` and `cpu_atom` event sections separately. Re-record
the profile after every rebuild so `perf.data` has the same build ID as the
installed `hdEmbree.so`.

OpenUSD tracing complements statistical profiling by measuring coarse phases:

```sh
PXR_ENABLE_GLOBAL_TRACE=1 \
HDEMBREE_RANDOM_NUMBER_SEED=1 \
pixi run usdrender --disableCameraLight \
    <stage.usda> --outputRoot /tmp/hdembree-profile-trace \
    > /tmp/hdembree-trace.txt 2>&1
```

`HdEmbreeRenderer` traces pre-render setup, Embree scene commit, preview trace
and resolve, full-resolution sample trace and resolve, convergence checks, and
AOV finalization. Keep trace scopes outside per-ray, per-hit, and per-BSDF
loops so instrumentation does not materially perturb the render.

After changing performance-sensitive code, compare renderer-reported time and
samples per second as well as end-to-end `perf stat` time. Run the focused
image-fidelity case to detect correctness regressions before running broader
suites. Use several workloads before generalizing a result: material-heavy,
texture-heavy, many-light, and deep-path scenes should be represented. Record
durable findings and baselines in `OPTIMIZATION.md`; `/tmp` profile artifacts
are ephemeral.

Run `pixi run build` when profiling is finished to restore the normal Release
plugin in the active Pixi environment.

## Directory Map

- `rendererPlugin.*`: Hydra plugin entry point. `HdEmbreeRendererPlugin`
  creates `HdEmbreeRenderDelegate` instances.
- `renderDelegate.*`: render delegate factory, supported Hydra prim types,
  render setting descriptors/defaults, Embree device and top-level scene
  ownership, global render thread, and shared `HdEmbreeRenderParam`.
- `renderPass.*`: `HdEmbreeRenderPass`; consumes `HdRenderPassState`, active
  `RenderSettings` data, cameras, AOV bindings, convergence state, and active
  `RenderProduct` writing.
- `renderer.*`: `HdEmbreeRenderer`; the progressive path tracer. Owns render
  loop state, camera state, AOV bindings, lights, sampling configuration,
  adaptive buffers, and Embree ray traversal calls.
- `mesh.*`: `HdEmbreeMesh`; translates Hydra mesh data into Embree prototype
  geometry and top-level instances.
- `instancer.*`: Hydra instancer support for per-instance transforms and
  instance contexts.
- `context.h`: Embree geometry user data used by ray hits: owning Rprim,
  primvar samplers, primitive params, bound material, uniform primvars, and
  instance transforms.
- `material.*`: `HdEmbreeMaterial`; pulls Hydra material networks and compiles
  them into `mxcpp::EvalGraph` objects for CPU shading.
- `mxcppAdapter.*` and `MaterialXCpp/`: MaterialX/OpenPBR/UsdPreviewSurface
  conversion and evaluation.
- `oiioTextureSystem.*`: texture lookup implementation used by MaterialXCpp.
- `light.*`: Hydra/USD Lux light Sprim implementation. Handles cylinder, disk,
  distant, dome, rect, and sphere lights; textures; IES shaping; and finite
  visible light geometry.
- `lightSamplers.*`: direct-light and dome-light sampling helpers.
- `meshSamplers.*`, `sampler.*`, `sampling.h`: primvar sampling and OpenQMC
  sample-domain logic.
- `renderBuffer.*`: CPU-backed Hydra render buffer implementation for AOVs.
- `config.*`: startup defaults from `HDEMBREE_*` environment variables.
- `schema.usda`, `generatedSchema.usda`, `plugInfo.json`: the
  `TyphoonRenderSettingsAPI` applied USD API schema and schema registration.
- `testenv/`: focused C++ tests for render settings, sampling, light sampling,
  and basic rendering.
- `README.md`: user-facing render setting descriptions. Check it for current
  behavior, but verify against code when changing settings.

## USD To Hydra To hdEmbree Flow

1. A client such as `usdview`, `usdrecord`, or `usdrender` opens a USD stage.
   USD render structure is described by `UsdRenderSettings`,
   `UsdRenderProduct`, and `UsdRenderVar`; see `pxr/usd/usdRender/overview.dox`
   and `pxr/usd/usdRender/doxygen/renderSettings.usda`.
2. UsdImaging turns USD prims into Hydra scene data. Important adapters and
   scene-index code are in `pxr/usdImaging/usdImaging/*Adapter.*`,
   especially `renderSettingsAdapter.*`, `renderProductAdapter.*`,
   `cameraAdapter.*`, `gprimAdapter.*`, and `pluginLightAdapter.*`.
3. Hydra creates the `HdEmbreeRenderDelegate` through
   `HdEmbreeRendererPlugin`. The delegate advertises supported Rprim, Sprim,
   and Bprim types in `renderDelegate.cpp`.
4. Hydra calls `HdRenderIndex::SyncAll()`. Dirty `HdEmbreeMesh`,
   `HdEmbree_Light`, `HdEmbreeMaterial`, and `HdEmbreeRenderBuffer` objects
   pull only their dirty data from `HdSceneDelegate` and update renderer-owned
   state.
5. Scene-changing Sync work uses `HdEmbreeRenderParam::AcquireSceneForEdit()`
   or `NotifySceneChange()`. That stops the render thread and increments the
   shared scene version so the render pass restarts accumulation.
6. `HdEmbreeRenderPass::_Execute()` observes scene version, render settings
   version, frame/time, camera state, data window, and AOV binding changes. It
   pushes updated state into `HdEmbreeRenderer` and starts or restarts the
   background `HdRenderThread`.
7. `HdEmbreeRenderer::Render()` commits or reuses the Embree scene, traces
   tiled samples, evaluates materials/lights/media, writes Hydra AOV buffers,
   and reports convergence.
8. When the active stage has `RenderSettings`/`RenderProduct` output and the
   renderer converges, `HdEmbreeRenderPass::_WriteActiveRenderProducts()` reads
   Hydra `HdRenderSettingsSchema`/`HdRenderProductSchema` data from the
   terminal scene index and writes color/raw raster products through Hio.

Hydra fundamentals worth reading when behavior is unclear:

- `pxr/imaging/hd/renderDelegate.h`: render setting map, descriptors,
  namespaces, and delegate lifecycle.
- `pxr/imaging/hd/renderPass.h` and `pxr/imaging/hd/renderPassState.h`:
  render pass execution inputs.
- `pxr/imaging/hd/sceneDelegate.h`: legacy data pull API used by hdEmbree
  prims during `Sync()`.
- `pxr/imaging/hd/renderSettings.h`: legacy `HdRenderSettings` Sprim data.
- `pxr/imaging/hd/renderSettingsSchema.*` and
  `pxr/imaging/hd/renderProductSchema.*`: scene-index data consumed by active
  RenderSettings/Product code.
- `pxr/imaging/hd/sceneIndexAdapterSceneDelegate.cpp`: bridges scene-index
  data back into legacy `HdSceneDelegate` queries.

## Render Settings Flow

hdEmbree supports two related settings paths:

- Hydra render delegate settings, configured through
  `HdRenderDelegate::SetRenderSetting()` and reflected by
  `GetRenderSettingDescriptors()`.
- USD-authored settings on `RenderSettings` prims, exposed through
  `TyphoonRenderSettingsAPI` attributes and Hydra namespaced settings.

The canonical hdEmbree-specific USD attributes are in the `ty:` namespace.
Tokens are defined in `HDEMBREE_RENDER_SETTINGS_TOKENS` in `renderDelegate.h`.
Defaults and UI labels are set in `HdEmbreeRenderDelegate::_Initialize()` in
`renderDelegate.cpp`, mostly from `HdEmbreeConfig`.

`HdEmbreeRenderDelegate::GetRenderSettingsNamespaces()` currently returns
`ty` and the empty namespace. `ty` asks UsdImaging/Hydra for hdEmbree-specific
`ty:` attributes. The empty namespace asks for generic unnamespaced custom
settings, currently used for `domeLightCameraVisibility`.

`HdEmbreeRenderPass::_GetNamespacedRenderSettings()` reads settings from the
active `HdRenderSettingsSchema`:

- all non-empty `ty:` settings are bridged into render delegate settings;
- `domeLightCameraVisibility` is read as an unnamespaced generic Hydra setting;
- legacy `ty:domeLightCameraVisibility` is intentionally ignored.

`_UpdateRenderSettingsFromActiveRenderSettingsPrim()` owns the bridge from the
active RenderSettings prim into render delegate settings. It tracks which
delegate values it set, resets bridge-owned settings back to defaults when USD
authored settings disappear, and avoids clobbering direct UI or explicit
delegate-setting overrides.

`_Execute()` then reads final values back from `HdRenderDelegate` and pushes
them into `HdEmbreeRenderer` through setters such as
`SetSamplesToConvergence()`, `SetEnableLighting()`,
`SetDomeLightCameraVisibility()`, `SetSamplerSequence()`,
`SetMaxBounces()`, `SetDisableShadows()`, and material-context handling.

When adding or changing a render setting, update all relevant surfaces:

- `renderDelegate.h`: token definition.
- `renderDelegate.cpp`: descriptor label, default, and default population.
- `config.h/.cpp`: environment variable-backed default if appropriate.
- `renderPass.cpp`: bridge and `_Execute()` push into `HdEmbreeRenderer`.
- `renderer.h/.cpp`: storage and runtime behavior.
- `schema.usda` and `generatedSchema.usda`: `TyphoonRenderSettingsAPI`.
- `plugInfo.json`: schema registration only if schema identity changes.
- `testenv/testHdEmbreeRenderSettings.cpp`: descriptor, namespace, bridge, and
  auto-apply coverage.
- RenderLab/usdview plugin code if the setting is exposed in that UI. Edit the
  source under `extras/usd/examples/usdviewPlugins/renderLab/`, especially
  `renderSettingsMetadata.py` and `renderSettingsEditor.py`, not the installed
  copy under `.pixi/envs/default/lib/python/renderLab/`.

Do not re-add `ty:domeLightCameraVisibility`; dome-light camera visibility is a
generic Hydra setting named `domeLightCameraVisibility`.

## Geometry And Primvars

`HdEmbreeMesh` is the only supported Rprim type. Its initial dirty mask asks
for topology, points, transform, visibility, culling, double-sided state,
display style, subdivision tags, primvars, normals, instancer, and material
binding data.

`HdEmbreeMesh::Sync()` receives a `HdEmbreeRenderParam`, obtains the Embree
scene/device, and calls `_PopulateRtMesh()`. `_PopulateRtMesh()` updates
triangle or subdivision prototype geometry, top-level instances, contexts,
primitive params, double-sided/refined state, normals, tangents, and cached
surface derivatives.

Primvars are pulled into `_primvarSourceMap` by `_UpdatePrimvarSources()` and
`_UpdateComputedPrimvarSources()`, then converted into `HdEmbreePrimvarSampler`
objects in `meshSamplers.*`. The sampler map is stored in
`HdEmbreePrototypeContext` so the renderer can evaluate primvars at ray hits.

Sync methods may run in parallel. Only pull data whose dirty bit is set, and
keep Embree context/object lifetimes valid until corresponding geometry is
released in `Finalize()`.

## Materials And Textures

`HdEmbreeMaterial::Sync()` pulls `HdMaterial::GetMaterialResource()` from the
scene delegate. It accepts modern `HdMaterialNetwork2` and legacy material
network maps, converts them in `mxcppAdapter.*`, and compiles an
`mxcpp::EvalGraph`.

MaterialXCpp supports EDF-only materials authored as `ND_uniform_edf`
connected to the `edf` input of an `ND_surface` terminal. The uniform EDF is
carried through graph evaluation as a typed `mxcpp::UniformEdf` closure;
`ND_surface` writes it to `SurfaceClosure::emissiveColor`, applies its opacity
as cutout presence, and clears the legacy BSDF summary when no scattering
closure is present. `ND_surface.bsdf` carries typed `mxcpp::BsdfClosure`
values from MaterialX PBR BSDF nodes into `SurfaceClosure::bsdfTree`; keep the
legacy summary cleared so missing or empty BSDF inputs do not fall back to the
old diffuse/specular defaults. When the connected BSDF tree contains a
`SubsurfaceData` node, `ND_surface` must also copy its color/radius/anisotropy
into the `SurfaceClosure` subsurface summary fields because the renderer's
random-walk SSS path uses those fields after `Bsdf::SampleSurface()` marks a
subsurface event.

MaterialXCpp implements `ND_tiledcircles_color3`,
`ND_tiledcloverleafs_color3`, and `ND_tiledhexagons_color3` directly from the
MaterialX stdlib nodegraph formulas. Keep the regular branch based on
`mod(texcoord * uvtiling - uvoffset) * 2 - 1`, and keep the staggered branch
constants aligned with stdlib. `ND_cloverleaf_float` doubles both texcoord and
center before evaluating its four circle lobes. `ND_hexagon_float` follows the
stdlib folded-coordinate SDF, including the swapped absolute delta vector and
both reflection folds before the final inside/outside test.

The active material render context is controlled by `ty:materialRenderContext`.
`HdEmbreeRenderDelegate::GetMaterialRenderContexts()` returns context priority
for Hydra material network selection. When that setting changes,
`HdEmbreeRenderPass::_ResyncMaterialNetworksForRenderContextChange()` asks
materials to recompile.

Textures go through `HdEmbreeOiioTextureSystem`, which implements the
MaterialXCpp texture system over OpenImageIO.

## Lights

`HdEmbree_Light` supports USD Lux cylinder, disk, distant, dome, rect, and
sphere lights. It reads common LightAPI inputs, color temperature, normalize,
texture files for dome/rect lights, shaping API inputs, and IES files.

Dome lights are sampled with distributions built from lat-long texture
luminance and solid angle. Finite light geometry can be visible to primary
rays through `visibleInPrimaryRay`; those shapes are inserted as Embree
geometry and tracked by `HdEmbreeRenderer::AddLightGeometry()`/
`RemoveLightGeometry()`.

Camera visibility for dome-light backgrounds is controlled by the generic
`domeLightCameraVisibility` render setting, not a light prim attribute.

## Light And Shadow Linking

`HdEmbree_Light::Sync()` reads Hydra's resolved `lightLink` and `shadowLink`
category tokens. Mesh and instancer category memberships come from
`HdSceneDelegate::GetCategories()` and native-instance memberships from
`GetInstanceCategories()`. Flattened instance records keep transforms and
categories together; their complete, immutable membership snapshot is stored
on `HdEmbreeInstanceContext` for renderer hit tests.

Light links filter surface and medium next-event estimation as well as finite,
distant, and dome emitters reached through BSDF or phase sampling. Shadow
links are evaluated for every mesh boundary in the existing transparent-shadow
intersection loop, before presence, transmission, or interior-medium state is
applied. Empty link tokens retain the default match-all behavior.

Use source values from `GetInstanceIndices()` to index native-instance
categories; local flattened ordinals are not equivalent. Whole-instancer
categories apply to all point-instancer instances because Hydra does not expose
per-point categories through these APIs. The upstream light-linking scene
index also has limited category support for nested native-instance proxies, so
hdEmbree consumes the memberships Hydra supplies but cannot reconstruct absent
per-proxy data.

## Rendering And Output

`HdEmbreeRenderer` owns the path tracing loop. Key responsibilities include:

- camera ray generation, depth of field, exposure compensation, and frame/time;
- tile scheduling and progressive accumulation;
- OpenQMC sample sequence/domain selection;
- adaptive sampling and the `adaptiveHeatmap` diagnostic AOV;
- path depth, Russian Roulette, direct light sampling, MIS, caustic policy,
  transparent-shadow approximation, media, and SSS;
- MaterialXCpp graph evaluation and primvar sampling;
- writing resolved AOV values into `HdEmbreeRenderBuffer`.

The render pass handles Hydra-facing output concerns: camera/data-window/AOV
state, fallback color/depth buffers, convergence checks, and
RenderProduct file writing. `usdrender` is the stage-authored output command;
its Python entry point is `pxr/usdImaging/bin/usdrender/usdrender.py`.
`UsdAppUtilsFrameRecorder` remains in
`pxr/usdImaging/usdAppUtils/frameRecorder.*` and is used by `usdrecord` and
`usdrender`.

## USD Render References

For stage-authored render data, read these before changing output behavior:

- `pxr/usd/usdRender/schema.usda`: canonical USD Render schema definitions.
- `pxr/usd/usdRender/overview.dox`: concepts, active render settings,
  products, products inheriting settings, cameras, resolution, data windows,
  and renderer-specific `UsdRenderSettingsAPI` extension guidance.
- `pxr/usd/usdRender/settings.*`, `product.*`, `settingsBase.*`,
  `renderVar.*`, and `spec.*`: C++ APIs and `UsdRenderComputeSpec()`.
- `pxr/usd/usdRender/doxygen/renderSettings.usda`: compact authored example.
- `pxr/usdImaging/usdImaging/renderSettingsAdapter.*` and
  `renderProductAdapter.*`: how USD Render prims become Hydra scene data.
- `pxr/usdImaging/usdImaging/renderSettingsFlatteningSceneIndex.*`: flattening
  and product inheritance behavior in the imaging path.

## Focused Tests

Common focused checks:

- `pixi run cmake --build build --target testHdEmbreeRenderSettings`
- `pixi run ctest --test-dir build -R testHdEmbreeRenderSettings --output-on-failure`
- `pixi run cmake --build build --target testHdEmbreeSampling`
- `pixi run cmake --build build --target testHdEmbreeLightSamplers`
- `pixi run cmake --build build --target testMaterialXCpp`

Use broader `ctest` filters when touching shared rendering, material, sampling,
or USD imaging behavior. If a test executable depends on installed plugins or
resources, prefer CTest through Pixi over running the binary directly.

## Editing Pitfalls

- Stop or restart the render thread before mutating renderer-readable state.
  `HdEmbreeRenderParam::NotifySceneChange()` is the usual path from Sync.
- Do not assume scene-index and legacy `HdSceneDelegate` paths carry identical
  data. Check `sceneIndexAdapterSceneDelegate.cpp` when values look missing.
- Keep render settings type-stable. Descriptor defaults, USD schema attribute
  types, UI-authored values, and renderer `GetRenderSetting<T>()` calls must
  agree.
- Active RenderSettings bridging must not overwrite direct UI edits or explicit
  delegate settings. Preserve the bridge-owned tracking behavior.
- Product output is convergence-driven. If an image never reaches convergence,
  `_WriteActiveRenderProducts()` will not run.
- Embree geometry user data must outlive geometry that can be hit by in-flight
  rays. Release contexts and geometry in `Finalize()` paths.
- Generated schema files are part of the plugin contract. Keep `schema.usda`,
  `generatedSchema.usda`, and tests in sync when changing
  `TyphoonRenderSettingsAPI`.
