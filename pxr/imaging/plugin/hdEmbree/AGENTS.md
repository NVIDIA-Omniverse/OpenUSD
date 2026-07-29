# hdEmbree Agent Guide

This directory contains the `hdEmbree` Hydra render delegate plugin also known as `Typhoon`. It is a
CPU path tracer built on Embree 4 with MaterialX/OpenPBR shading, USD Lux light
support, Hydra render settings, and stage-authored `UsdRender` product output.

Use this file to get oriented quickly before changing code in this plugin.

Important maintenance rule: agents MUST keep `AGENTS.md`, `README.md`, and
`ARCHITECTURE.md` up to date as hdEmbree, RenderLab, usdrender, build tasks,
tests, or surrounding USD/Hydra integration change. Update all affected
documents in the same change whenever behavior, architecture, extension
points, or workflow knowledge changes.

## Project Goals

Typhoon is a **reference path tracer**. It should be easily readable by a human (and agents) above all other concerns.

1. Keep it simple. Don't add unneccesary complexity via abstractions. Don't add an abstraction unless it results in a net reduction in code size of at least twice what the abstraction adds. Avoid "fancy" C++-isms. Use simple, readable code.
2. Prefer linear flow. Don't add lots of tiny helper functions that mean the reader has to jump around in the codebase to see what the code is doing, even if doing so adds more code.
3. Variable naming must stay consistent for common quantities. Use the
   `normal`, `omegaIn`/`omegaOut`, `radiance`, `iorIn`/`iorOut`, `eta`,
   `absorption`, `scattering`, and `extinction` roots defined by the
   authoritative naming table in `ARCHITECTURE.md`; IOR sides use the optics
   incident/transmitted convention, and `eta` means only `iorIn / iorOut`.
   Derivatives use `d<Quantity>d<Variable>`, such as
   `dPdu`/`dPdv` for surface parameterization and `dPdx`/`dPdy` for
   screen-space ray differentials.
4. Comment everything with WHAT the code is intended to do, not HOW. Ensure function declarations contain details of expected invariants on inputs and possible failure modes plus errors returned/raised. Comment each logical section of a definition with WHAT and WHY the code is doing what it is.
5. Always name variable types correctly. Prefer an explicit type whenever it is
   reasonably short and meaningful. Do not use `auto` except in these cases:

   - A range-for loop variable.
   - A ridiculous standard-library iterator type returned by `.find()` and
     similar lookup operations. This exists to keep container implementation
     types from obscuring the algorithm; it does not license `auto` for a
     lookup result whose type is short and meaningful.
   - A `std::visit` visitor lambda parameter. The project builds at C++17,
     which has no syntax for naming a lambda parameter's type, and the
     alternatives - a visitor functor struct, or one explicit `operator()`
     overload per variant alternative - add more machinery than they remove.
     Keep such lambdas to one per dispatch site and comment them as this
     exception. This covers the parameter only; `auto` locals inside the
     visitor body are not exempt. A generic `auto` parameter on a lambda that
     is not a `std::visit` visitor is not covered - give it a concrete type,
     or a function pointer type if every argument is captureless.
   - Binding a lambda closure object. A closure type is unnameable, and the
     alternatives - `std::function`, a named functor struct, or a free
     function with the captures passed explicitly - all add indirection or
     machinery, some of it on per-path-vertex code.
   - A structured binding declaration. C++17 has no typed form; spelling the
     underlying `std::pair` or `std::tuple` reintroduces the long iterator
     type the lookup exception exists to hide, and loses the names that make
     the site readable.

   When replacing an `auto`, preserve the deduced type exactly: reference
   category, top-level constness, and pointee constness. Never substitute a
   merely convertible type or a base class.

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
- `-DPXR_ENABLE_MATERIALX_SUPPORT=TRUE`
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
0.7.1, MaterialX Render 1.39.4, OpenImageIO 2.5, OpenSubdiv, TBB, PySide6,
PyOpenGL, CMake, Ninja, and compilers.

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
FLIP comparison, and report generation. Set `ty:randomNumberSeed` to a fixed
value with `--set`. Keep resolution, sample count, bounce count, adaptive
sampling, and other scene-authored render settings unchanged between
measurements. Scene-authored `ty:` settings take precedence over hard-coded
defaults; `--set` takes precedence over both.
Run the profiling commands below from `/home/anders/code/openusd-omniverse` so
the `$PWD/.pixi` path identifies the provider environment.

Use at least five `perf stat` repetitions for before-and-after measurements:

```sh
pixi run --clean-env -x /usr/bin/env \
    PATH="$PWD/.pixi/envs/default/bin:/usr/bin:/bin" \
    /usr/bin/perf stat -r 5 -d -o /tmp/hdembree-stat.txt -- \
    usdrender --complexity high --renderer Embree \
    -s "{settings}.ty:randomNumberSeed = 1" \
    <stage.usda> --outputRoot /tmp/hdembree-profile-stat
```

Collect call stacks for the renderer and all worker threads with:

```sh
pixi run --clean-env -x /usr/bin/env \
    PATH="$PWD/.pixi/envs/default/bin:/usr/bin:/bin" \
    /usr/bin/perf record -o /tmp/hdembree.data \
    -F 499 -e cycles:u -g --call-graph fp -- \
    usdrender --complexity high --renderer Embree \
    -s "{settings}.ty:randomNumberSeed = 1" \
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
pixi run usdrender \
    -s "{settings}.ty:randomNumberSeed = 1" \
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

## Source Boundary

- `delegate/` contains the Hydra-facing plugin, render delegate/pass, scene primitives, and AOV bridge.
- `renderer/` contains the path tracer and its rendering, sampling, shading, texture, and third-party support components.
- Dependencies should flow from `delegate/` to `renderer/`; new renderer code should not depend on Hydra-facing delegate implementation details.
- Include first-party hdEmbree headers by their absolute project path, starting
  with `pxr/imaging/plugin/hdEmbree/`; do not use same-directory or `../`
  relative paths.
- `schema/` contains the authored and generated `TyphoonRenderSettingsAPI` schema files. Runtime plugin metadata remains in root `plugInfo.json`.
- No hand-written hdEmbree header is installed or supported as a C++ API or extension point.

## Renderer Namespace And Linkage

- Shared renderer helpers declared in headers live in
  `PXR_NAMESPACE::ty` and have no leading underscore.
- Translation-unit-contained renderer functions and constants must not live in
  `ty`: use `static _Foo` directly at `PXR_NAMESPACE` scope, or preserve an
  existing anonymous namespace when it groups file-local implementation.
  Do not churn between those two internal-linkage forms for style alone.
- `renderer/materials/MaterialXCpp/` keeps its existing `mxcpp` and anonymous
  namespace conventions. Do not introduce `mxcpp::ty`.
- Renderer types remain at `PXR_NAMESPACE` scope with their existing
  `HdEmbree` or leading-underscore names as an interim state. Plan 19 moves
  renderer types into `ty` and completes the namespace pass.

## Directory Map

- `delegate/rendererPlugin.*`: Hydra plugin entry point. `HdEmbreeRendererPlugin`
  creates `HdEmbreeRenderDelegate` instances.
- `delegate/renderDelegate.*`: render delegate factory, supported Hydra prim types,
  render setting descriptors/defaults, Embree device and top-level scene
  ownership, global render thread, and shared `HdEmbreeRenderParam`.
- `delegate/renderPass.*`: `HdEmbreeRenderPass`; consumes `HdRenderPassState`, active
  `RenderSettings` data, cameras, AOV bindings, convergence state, and active
  `RenderProduct` writing.
- `renderer/renderer.*`: `HdEmbreeRenderer` façade, persistent frame/settings
  state, progressive preview/full-resolution orchestration, and per-pixel
  integrator selection/AOV output classification.
- `renderer/camera/camera.cpp`: camera/lens sampling, primary-ray construction, and ray differentials; tile traversal stays in `renderer/renderer.cpp`.
- `renderer/aov/aovOutput.cpp`: AOV validation, accumulation, adaptive convergence,
  hit outputs, and direct buffer writes.
- `renderer/integrator/pathIntegrator.cpp`: lit multi-bounce control loop and
  shared path state; owns its primary hit and surface-event ordering.
- `renderer/integrator/volumeTransport.cpp`: participating-medium segment
  transport and active-medium boundary ownership.
- `renderer/integrator/unlitIntegrator.cpp`: independent single-hit camera-light
  and ambient-occlusion integrator.
- `renderer/integrator/surfaceShading.cpp`, `lighting.cpp`, `sss.cpp`, and
  `visibility.cpp`: shared shading contexts and ray differentials, direct and
  environment lighting, subsurface transport, and linked/transparent traversal.
- `renderer/rendererMath.h`, `rayUtil.h`, `heroWavelength.h`, and
  `geometry/normalTransforms.h`: focused inline numeric, ray, spectral, and
  normal-transform helpers shared by renderer translation units.
- `renderer/geometry/surfaceDerivatives.*`: triangle/subdivision shading
  frames, authored/displaced normals, and surface derivatives.
- `renderer/integrator/closureClassification.*`: renderer transport
  classification of compiled material closures.
- `renderer/integrator/transportPolicy.h`: shared contribution cutoff,
  firefly-clamping, and multi-sample MIS policy.
- `delegate/mesh.*`: `HdEmbreeMesh`; translates Hydra mesh data into Embree
  prototype geometry. `_UpdateInstances()` separately materializes the
  top-level Embree instances after prototype updates.
- `delegate/instancer.*`: Hydra instancer support for per-instance transforms and
  instance contexts.
- `renderer/geometry/context.h`: Embree geometry user data used by ray hits: owning Rprim,
  primvar samplers, primitive params, bound material, uniform primvars, and
  instance transforms.
- `delegate/material.*`: `HdEmbreeMaterial`; pulls Hydra material networks and compiles
  them into `mxcpp::EvalGraph` objects for CPU shading.
- `renderer/materials/mxcppAdapter.*` and `renderer/materials/MaterialXCpp/`:
  MaterialX/OpenPBR/UsdPreviewSurface conversion and evaluation. Coupled
  dielectric straight shadows use exact Fresnel through alpha 0.002, blend
  to the BSDL-generated directional transmission LUT through alpha 0.07, and
  use the LUT directly above that band; regenerate the committed
  runtime table when its analytic endpoint, quadratic roughness mapping, or MIS
  bake changes.
- `renderer/materials/oiioTextureSystem.*`: texture lookup implementation used by MaterialXCpp.

- `delegate/light.*`: Hydra/USD Lux light Sprim adapter. Populates renderer-owned light data for cylinder, disk,
  distant, dome, rect, and sphere lights; textures; IES shaping; and finite
  visible light geometry.
- `renderer/lights/light.h`, `lightRegistry.*`, and `lightSamplers.*`: runtime light data, synchronized lookup ownership, and direct/dome sampling.
- `renderer/materials/material.h`: stable compiled-material handle populated by the delegate.
- `renderer/renderBuffer.h`: renderer AOV-output interface implemented by the delegate buffer.
- `renderer/geometry/meshSamplers.*`, `renderer/geometry/primvarSampler.*`, `renderer/sampling/sampling.h`: primvar sampling and OpenQMC
  sample-domain logic.
- `delegate/renderBuffer.*`: CPU-backed Hydra render buffer implementation for AOVs.
- `renderer/renderSettings.h`: renderer-consumed settings, hard-coded defaults,
  token conversions, and pass-owned setting defaults.
- `schema/schema.usda`, `schema/generatedSchema.usda`, `plugInfo.json`: the
  `TyphoonRenderSettingsAPI` applied USD API schema and schema registration.
- `testenv/`: focused C++ tests for render settings, sampling, light sampling,
  and basic rendering.
- `README.md`: user-facing render setting descriptions. Check it for current
  behavior, but verify against code when changing settings.

Keep transmissive model policy explicit in compiled closures. OpenPBR and
metalness-workflow UsdPreviewSurface coupled interfaces enable combined
reflection/refraction energy compensation through an additive cosine
multiple-scattering lobe, and choose glossy rough branches from the sampled
microfacet Fresnel response. Standard Surface retains separate
reflection and transmission lobes, with `thin_walled` using IOR 1. OpenPBR
`geometry_thin_walled` uses the coupled thin-sheet interface and retains
authored-IOR Fresnel.

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
   and Bprim types in `delegate/renderDelegate.cpp`.
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
7. `HdEmbreeRenderer::Render()` first validates the scene, AOV interface types,
   formats, dimensions, and data-window containment before scene commit or
   buffer mapping. Failure maps no buffers, traces no tiles, marks usable
   buffers converged, and returns. Successful setup commits or reuses the
   Embree scene, maps every buffer exactly once, traces tiled samples,
   evaluates materials/lights/media, writes Hydra AOV buffers, then unmaps each
   buffer exactly once and reports convergence. AOV validation is deliberately
   uncached because buffer properties can change without rebinding.
8. When an offline client sets `enableInteractive = false`, the active stage
   has `RenderSettings`/`RenderProduct` output, and the renderer both passes
   setup and converges,
   `HdEmbreeRenderPass::_WriteActiveRenderProducts()` reads Hydra
   `HdRenderSettingsSchema`/`HdRenderProductSchema` data from the terminal
   scene index and writes color/raw raster products through Hio. Unset or true
   `enableInteractive` values suppress file output for viewers such as usdview.

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
Tokens are defined in `HDEMBREE_RENDER_SETTINGS_TOKENS` in `delegate/renderDelegate.h`.
Defaults live in `renderer/renderSettings.h`; UI labels and descriptors are set
in `HdEmbreeRenderDelegate::_Initialize()` in `delegate/renderDelegate.cpp`.

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

`_Execute()` then reads final values back from `HdRenderDelegate`, resolves
cross-setting policy and token values, and pushes one
`HdEmbreeRenderSettings` value through
`HdEmbreeRenderer::SetRenderSettings()`. Material-context handling remains
pass-owned.

When adding or changing a render setting, update all relevant surfaces:

- `delegate/renderDelegate.h`: token definition.
- `delegate/renderDelegate.cpp`: descriptor label and default population.
- `renderer/renderSettings.h`: hard-coded default and renderer field, or a
  named pass-owned default.
- `delegate/renderPass.cpp`: bridge and `_Execute()` push into `HdEmbreeRenderer`.
- `renderer/renderer.h/.cpp`: setting storage and frame orchestration; runtime behavior lives in the owning `renderer/aov/`, `renderer/camera/`, or `renderer/integrator/` implementation.
- `schema/schema.usda` and `schema/generatedSchema.usda`: `TyphoonRenderSettingsAPI`.
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
triangle or subdivision prototype geometry, prototype contexts, primitive
params, double-sided/refined state, normals, tangents, and cached surface
derivatives, then calls `_UpdateInstances()` to resize and populate top-level
instance geometry and contexts when instance state is dirty.

Primvars are pulled into `_primvarSourceMap` by `_UpdatePrimvarSources()` and
`_UpdateComputedPrimvarSources()`, then converted into `HdEmbreePrimvarSampler`
objects in `renderer/geometry/meshSamplers.*`. The sampler map is stored in
`HdEmbreePrototypeContext` so the renderer can evaluate primvars at ray hits.
The context owns samplers in `primvarMap`. Material compilation assigns each
constant `geomprop` name one integer handle shared by the surface and
displacement graphs; each mesh resolves that handle table to observing sampler
and uniform-value vectors after sampler/material Sync. Material recompiles
increment the material version even on failure, and the render pass refreshes
all mesh bindings before subdivision recommits or rendering. Never add a
hit-time string or `TfToken` lookup fallback. Connected, absent, or non-string
`geomprop` inputs violate MaterialX's uniform contract; compile them to invalid
handle -1, emit one recoverable diagnostic, and evaluate the node's authored
default without invalidating the terminal.

For refined primvars, preserve indexed face-varying data rather than flattening it: each face-varying primvar needs an independent Embree attribute topology because different primvars can have different seams. Topology 1 is reserved for linear `varying` data; face-varying topologies start at 2. Embree maps `none` to `SMOOTH_BOUNDARY`, the three OpenSubdiv corner variants to `PIN_CORNERS`, `boundaries` to `PIN_BOUNDARY`, and `all` to `PIN_ALL`. Embree interpolation buffers and outputs must remain 16-byte padded. Low-complexity triangle samplers still need indexed values flattened before triangulation.

At `low` complexity, subdivision meshes use their triangulated control cage. `medium`, `high`, and `veryhigh` use screen-space adaptive subdivision targeting 4, 1, and 0.5 pixel edges. `HdEmbreeRenderPass` requires an attached `HdCamera` and snapshots the first valid camera/data window and triggers `HdEmbreeRenderDelegate::UpdateAdaptiveSubdivision()` for initial geometry and later scene edits using that frozen view. `ty:dynamicSubdvTesselation = true` additionally refreshes the snapshot and levels after projection or data-window changes. `delegate/adaptiveSubdivision.*` projects every coarse edge through all instance transforms and clips edges to the homogeneous view volume. Displaced quads additionally evaluate final positions on a fixed 3x3 `(u,v)` grid; midpoint-to-chord errors above 0.5 pixel at medium or 0.25 pixel at high/very-high can raise only the affected parametric direction, capped at 2x the fresh camera baseline. Propagate the 2x factor through shared edges and quad-opposite pairs along the complete edge strip before changing levels; a one-sided propagated level creates Embree transition-fan triangles that can fold after displacement. Non-quads and failed probes retain the baseline except where they share a boosted edge. Always run shared-edge consolidation and quad 2:1 balancing after displacement refinement, write shared-edge-consistent `RTC_BUFFER_TYPE_LEVEL` values, and let `HdEmbreeMesh` recommit the prototype scene. Keep levels in Embree’s `[1, 4096]` range and never multiply the previously cached levels, which would ratchet across updates. Surface shading, displacement callbacks, and dicing probes share `HdEmbreeSamplePrimvar`, so constant, uniform, vertex, varying, and face-varying geomprops use the same production samplers in all paths. Instance primvars cannot vary prototype displacement because Embree tessellates the shared prototype before applying instance transforms.

Hydra `wireOnSurf`/`refinedWireOnSurf` and `wire`/`refinedWire` reprs are
carried through `HdEmbreePrototypeContext`. `renderer/geometry/wireframe.*`
computes screen-space edge coverage: coarse hits use triangle barycentrics;
refined hits decode quad or n-gon sub-patch UVs and the live subdivision level
buffer to reconstruct the final diced grid. Restore the one-pixel derivative
footprint after texture filtering's sample-count scaling, and use continuous
coverage rather than a binary sample discard. Derive coverage in Embree's
geometric hit parameterization, never from MaterialX `st` derivatives. Final
mesh diagnostics must evaluate every regular diced U/V edge and cell diagonal;
do not replace subpixel topology with a coarser display LOD. Subpixel cells
should contribute filtered dense coverage and require zoom or higher output
resolution to resolve individually. Compose the wire before adaptive
variance/AOV accumulation. Wire-on-surface runs after the selected integrator
so the overlay follows final shading and displacement. Edge-only returns after
the primary camera hit, before material, light, volume, AO, or secondary-ray
evaluation, then composites opaque black coverage over the clear color.
Embree does not expose internal transition-fan primitive IDs, so do not claim
exact stitch diagonals where opposing edge levels differ. Edge-only mode
ignores Hydra's wire color, blends nearest-surface interiors to the clear
color, and does not reveal rear edges; wire-on-surface is the
authoritative diagnostic path. `HdEmbreeRenderPass::_MarkCollectionDirty()`
must mark rprims `DirtyRepr` whenever the collection repr selector or its
forced-repr state changes. Hydra's dirty list only rebuilds automatically the
first time it encounters a selector, so returning to a previously used mode
otherwise skips `_InitRepr()` and `Sync()`. `_InitRepr()` must then set
`HdChangeTracker::NewRepr` whenever the resolved requested repr changes,
including a return to an already registered repr, and mesh `Sync()` must clear
`NewRepr` after publishing the new mode to the prototype context.

Sync methods may run in parallel. Only pull data whose dirty bit is set, and
keep Embree context/object lifetimes valid until corresponding geometry is
released in `Finalize()`.

## Materials And Textures

`HdEmbreeMaterial::Sync()` pulls `HdMaterial::GetMaterialResource()` from the
scene delegate. It accepts modern `HdMaterialNetwork2` and legacy material
network maps, converts them in `renderer/materials/mxcppAdapter.*`, and compiles an
`mxcpp::EvalGraph`. Surface and optional `displacement` terminals are compiled separately into the stable material handle. `HdEmbreeMesh` registers an Embree subdivision displacement callback; scene commit evaluates `ND_displacement_float` at generated vertices and offsets positions by `displacement * scale` along the normalized object-space geometric normal. Bind material state before committing the prototype scene, and force a recommit after displacement material changes. The callback currently supplies object-space position/normal, `st`, and constant string/filename geomprops. Triangle geometry, including meshes with `subdivisionScheme = "none"`, is not displaced.

`EvalGraph::Compile()` returns an explicit valid, invalid, or absent-terminal
result. Valid results alone own a graph and may carry one recoverable authoring
diagnostic; invalid results carry one fatal diagnostic; absent optional
terminals carry neither. The material delegate
warns for invalid surfaces, warns for an absent surface only when neither a
volume nor displacement terminal is authored, warns for malformed
displacement, and keeps absent displacement silent. A malformed
displacement-only material emits the actionable displacement warning without a
redundant missing-surface warning. Volume-only
materials synthesize a transparent medium boundary; displacement-only
materials retain the renderer's default display-color surface. Authored graph
failures must not escape compilation or hit-time/displacement evaluation as
exceptions. Renderer callbacks must translate recoverable backend failures
into their documented fallback values; texture failures are caught and
reported at the OIIO boundary where the filename is available. Unexpected
exceptions reaching Embree's displacement C callback boundary emit a
`TF_RUNTIME_ERROR` once per prototype commit rather than escaping through
Embree or failing silently; the affected callback lane remains undisplaced.

`SurfaceClosure::isVolumeBoundary` identifies volume-only boundaries
independently of `hasInteriorMedium`. Preserve it for vacuum or locally
zero-density volume closures so they remain transparent transport boundaries;
`hasInteriorMedium` continues to mean that there is actual medium state to
enter. `MixSurfaceClosures` preserves the flag only when every input with
nonzero weight is a volume boundary; endpoint mixes preserve the selected
input and must not retain the zero-weight branch's BSDF tree or medium state.

MaterialXCpp supports EDF-only materials authored as `ND_uniform_edf`
connected to the `edf` input of `ND_surface`. The uniform EDF is carried
through graph evaluation as a typed `mxcpp::UniformEdf` closure; `ND_surface`
writes it to `SurfaceClosure::emissiveColor`, applies its opacity as cutout
presence, and clears the legacy BSDF summary when no scattering closure is
present. `ND_surface` may be either the terminal material model or an ordinary
surfaceshader-valued node feeding another surfaceshader node. Surface-shader
values are carried through `mxcpp::Value` as `SurfaceClosure`; for example,
`ND_mix_surfaceshader` blends its `bg` and `fg` `SurfaceClosure` inputs,
including emissive summaries and BSDF closure trees. `ND_surface.bsdf` carries
typed `mxcpp::BsdfClosure`
values from MaterialX PBR BSDF nodes into `SurfaceClosure::bsdfTree`; keep the
legacy summary cleared so missing or empty BSDF inputs do not fall back to the
old diffuse/specular defaults. When the connected BSDF tree contains a
`SubsurfaceData` node, `ND_surface` must also copy its color/radius/anisotropy
into the `SurfaceClosure` subsurface summary fields because the renderer's
random-walk SSS path uses those fields after `Bsdf::SampleSurface()` marks a
subsurface event. `ND_dielectric_bsdf` maps to `Bsdf::DielectricData`, including
MaterialX `scatter_mode` values `R`, `T`, and `RT`. `ND_layer_bsdf` combines
typed `BsdfClosure` inputs into one closure tree; when merging trees, remap all
child node ids in nested mix/layer/add/multiply nodes before appending the new
layer root. MaterialX VDF nodes are carried as typed `mxcpp::VdfClosure` medium
data: `ND_absorption_vdf` fills absorption, `ND_anisotropic_vdf` fills
absorption/scattering/Henyey-Greenstein anisotropy, and `ND_layer_vdf` attaches
that medium to the top BSDF closure. `ND_surface` copies the optional medium
into `SurfaceClosure::interiorMedium`; keep `thin_walled` suppressing that
interior medium so thin surfaces do not enter participating media.

MaterialX `ND_geomcolor_*` nodes read only the USD/Hydra `displayColor`
primvar stream (`primvars:displayColor`) through `ShadingContext::displayColor`.
The color4 variant uses `displayOpacity` as alpha. Do not read arbitrary
`geomColor` or `geomColorN` primvars here; use `ND_geompropvalue_*` for named
geometry primvars.

MaterialX `ND_geompropvalueuniform_string` and
`ND_geompropvalueuniform_filename` read `HdInterpolationConstant` primvars
from the prototype's material-handle-indexed uniform-value vector. String-like
values are accepted from string, token, asset path, and their array forms;
asset paths should resolve to the resolved path when available and otherwise
fall back to the authored asset path.

MaterialXCpp implements `ND_tiledcircles_color3`,
`ND_tiledcloverleafs_color3`, and `ND_tiledhexagons_color3` directly from the
MaterialX stdlib nodegraph formulas. Keep the regular branch based on
`mod(texcoord * uvtiling - uvoffset) * 2 - 1`, and keep the staggered branch
constants aligned with stdlib. `ND_cloverleaf_float` doubles both texcoord and
center before evaluating its four circle lobes. `ND_hexagon_float` follows the
stdlib folded-coordinate SDF, including the swapped absolute delta vector and
both reflection folds before the final inside/outside test.

MaterialXCpp implements `ND_blur_float`, `ND_blur_color3`, `ND_blur_color4`,
`ND_blur_vector2`, `ND_blur_vector3`, and `ND_blur_vector4` by propagating a
subtree-local texture preblur amount through `ShadingContext::textureBlur` to
`Texture2DRequest::blur` and OIIO `TextureOpt::sblur/tblur`. Do not map them
back to MaterialX's stdlib nodegraph, which is documented as a pass-through
placeholder, and do not implement blur by arbitrary repeated UV resampling.
Constant/unconnected inputs should remain stable; image-backed inputs should
receive OIIO texture preblur based on the MaterialX `size` input.

The active material render context is controlled by `ty:materialRenderContext`.
`HdEmbreeRenderDelegate::GetMaterialRenderContexts()` returns context priority
for Hydra material network selection. When that setting changes,
`HdEmbreeRenderPass::_ResyncMaterialNetworksForRenderContextChange()` asks
materials to recompile.

Textures go through `HdEmbreeOiioTextureSystem`, which implements the
MaterialXCpp texture system over OpenImageIO.
PNG image nodes expect source alpha to remain straight/unassociated. PNG
textures use a dedicated OIIO texture system with `unassociatedalpha` enabled
so alpha is not premultiplied into RGB before hdEmbree applies texture
color-space conversion to RGB only. Do not enable this globally; non-PNG
formats should retain OIIO's default alpha handling.

Surface hits use one central interaction contract. `normalGeomWldExt` remains
outward and immutable; it alone owns boundary classification, medium
transitions, and offsets. `normalGeomWldOut` is faced toward `omegaOutWld`.
Smooth/displaced `normalSrfWldExt` is aligned outward once, then transformed
to `normalSrfWldOut`; material resolution produces `normalShdWldOut`.
Material normals outside the `normalGeomWldOut` hemisphere fall back to
`normalSrfWldOut`. Never infer topology from a shading normal or reintroduce
closure-dependent normal orientation.

`ShadingContext::texcoord` preserves authored USD `st` values in MaterialX's
lower-left UV convention. Do not pre-flip V when building the shading context.
The OpenImageIO texture backend is the boundary that converts MaterialX UVs to
OIIO/image-space coordinates by flipping T together with its derivatives.
For UDIM filenames, tile selection must use the unflipped MaterialX UVs; only
the local per-tile T coordinate is flipped before sampling the concrete image.
glTF image and normal-map nodes use this same graph-facing MaterialX UV
convention; do not add an additional glTF-only V flip around texture transforms.

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
- Hydra mesh repr wireframe composition on the retained primary hit;
- writing resolved AOV values into `HdEmbreeRenderBuffer`.

The render pass handles Hydra-facing output concerns: camera/data-window/AOV
state, fallback color/depth buffers, convergence checks, and offline
RenderProduct file writing. It treats an unset `enableInteractive` setting as
true and writes products only when the client explicitly sets it to false.
`usdrender` is the stage-authored output command;
its C++ implementation is under `pxr/usdImaging/bin/usdrender/` and drives
`UsdImagingGLEngine` directly. It owns output roots and frame-placeholder
expansion; hdEmbree must not expand placeholders. Its repeatable `-s` /
`--set` option validates attribute targets against the unmodified composed
stage, parses each USDA value in an isolated scratch layer, and authors only
the resulting value into an anonymous session layer. `{settings}` resolves
once before overrides to the active RenderSettings prim. `UsdAppUtilsFrameRecorder`
remains the legacy `usdrecord` path and is not used by `usdrender`.

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

- `pixi run cmake --build build --target testHdEmbreeRenderSetup`
- `pixi run ctest --test-dir build -R testHdEmbreeRenderSetup --output-on-failure`
- `pixi run cmake --build build --target testHdEmbreeRenderSettings`
- `pixi run ctest --test-dir build -R testHdEmbreeRenderSettings --output-on-failure`
- `pixi run cmake --build build --target testHdEmbreeSampling`
- `pixi run cmake --build build --target testHdEmbreeWireframe`
- `pixi run ctest --test-dir build -R testHdEmbreeWireframe --output-on-failure`
- `pixi run cmake --build build --target testHdEmbreeLightSamplers`
- `pixi run cmake --build build --target testMaterialXCpp`
- `pixi run build/pxr/imaging/plugin/hdEmbree/testMaterialXCpp`
- `pixi run cmake --build build --target testHdEmbreeSubdivision`
- `pixi run ctest --test-dir build -R testHdEmbreeSubdivision --output-on-failure`
- Collect the AOUSD displacement fixture: `cd /home/anders/code/aousd-materials-test-suite && pixi run pytest test-suite/surfaces/open_pbr_surface/displacement.usda --collect-only -q`
- Render that fixture with this checkout’s installed `usdrender` when validating displacement or complexity; the test-suite Pixi environment may resolve a separately packaged renderer.

The external `/home/anders/code/typhoon-test-suite` Goldeneye repository has
two renderer regression suites:

- `powerprofilesctl launch --profile performance -- pixi run pytest materials`
  runs 67 renderer-focused material,
  transport, primvar, geometry, and texture fixtures imported from the AOUSD
  materials suite.
- `powerprofilesctl launch --profile performance -- pixi run pytest test-suite`
  runs 41 broader renderer integration fixtures.
- `powerprofilesctl launch --profile performance -- pixi run pytest usdlux`
  runs 328 active direct-lighting frames across the USD Lux light types,
  shaping/IES controls, and visible light geometry.

The mandatory complete-suite command is:

```sh
powerprofilesctl launch --profile performance -- \
    pixi run pytest --renderer typhoon-local
```

Its expected laptop baseline is approximately 235 seconds. If any test fails
or runtime is 250 seconds or above, stop and check with Anders before
continuing or landing the change.
The complete suite currently contains 436 cases.

Initialize its shaderball assets with
`git submodule update --init --depth 1`. Use the material subtree for transport
or shading changes and `usdlux` for light-adapter/sampling changes. Broad
MaterialX node evaluation remains covered by `testMaterialXCpp`, not by the
render suite.

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
- Product output requires both valid renderer setup and convergence. If setup
  fails or an image never reaches convergence, `_WriteActiveRenderProducts()`
  will not run.
- Embree geometry user data must outlive geometry that can be hit by in-flight
  rays. Release contexts and geometry in `Finalize()` paths.
- Generated schema files are part of the plugin contract. Keep `schema.usda`,
  `generatedSchema.usda`, and tests in sync when changing
  `TyphoonRenderSettingsAPI`.
