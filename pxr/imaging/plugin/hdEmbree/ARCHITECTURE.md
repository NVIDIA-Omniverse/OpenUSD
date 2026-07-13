# hdEmbree Architecture

This guide explains where hdEmbree code lives, how Hydra data reaches the path tracer, how a frame is rendered, and where new functionality belongs. See `README.md` for user-facing features and settings, and `AGENTS.md` for build and test workflow.

## Design boundary

hdEmbree is one plugin target with three source submodules:

- `delegate/`: Hydra-facing adapter. Creates Hydra primitives, pulls dirty scene data, translates render-pass state and settings, owns the render thread, and publishes Hydra render buffers.
- `renderer/`: CPU path tracer and support code. Owns progressive state, Embree traversal, path integration, sampling, shading, lighting, textures, media, and subsurface scattering.
- `schema/`: authored and generated `TyphoonRenderSettingsAPI` schema.

The intended dependency direction is `Hydra -> delegate -> renderer`. Renderer code depends only on renderer-owned runtime records and interfaces; delegate adapters populate or implement those contracts.

## Repository map

### Plugin root

- `CMakeLists.txt`: plugin sources, dependencies, installed headers/resources, compile definitions, and C++ tests.
- `plugInfo.json`: Hydra renderer, scene-index plugin, and Typhoon schema registration.
- `pch.h`: precompiled system/standard headers. It remains at root because the OpenUSD build macros discover it there.
- `README.md`: user-facing capabilities and render settings.
- `ARCHITECTURE.md`: this developer guide.
- `AGENTS.md`: build, test, profiling, and maintenance workflow.
- `overview.dox`: Doxygen overview.
- `OPTIMIZATION.md`: profiling results and optimization notes.
- `TODO.md`: known future work.
- `testenv/`: focused integration and unit-style C++ tests.

### `delegate/`: Hydra integration

- `rendererPlugin.h/.cpp`: `HdRendererPlugin` entry point; reports support and creates/deletes `HdEmbreeRenderDelegate`.
- `renderDelegate.h/.cpp`: central factory and lifetime owner. Declares setting tokens/descriptors; advertises supported Rprim, Sprim, and Bprim types; creates scene adapters, buffers, and passes; owns the Embree device/top-level scene, renderer, render thread, and render param.
- `renderParam.h`: synchronization bridge. Scene edits stop rendering, acquire the Embree scene for mutation, and increment the scene version used to restart accumulation.
- `renderPass.h/.cpp`: converts `HdRenderPassState`, camera, framing, AOVs, scene-index render settings/products, and delegate settings into renderer setters. Starts/restarts rendering, reports convergence, and writes active render products.
- `renderBuffer.h/.cpp`: CPU-backed `HdRenderBuffer` storage, mapping, format conversion, convergence, and renderer write access.
- `mesh.h/.cpp`: `HdMesh` adapter. Pulls topology, points, transforms, subdivision data, primvars, materials, categories, and instancing; builds/updates Embree prototypes and instances.
- `instancer.h/.cpp`: `HdInstancer` adapter; computes instance transforms and per-instance category/light-linking context.
- `material.h/.cpp`: `HdMaterial` adapter; pulls Hydra networks, normalizes them through `mxcppAdapter`, owns the compiled `mxcpp::EvalGraph`, and updates a stable renderer material-data handle.
- `light.h/.cpp`: `HdLight` adapter. Pulls USD Lux parameters, transforms, textures, IES data, shaping, linking, and visible finite-light geometry into renderer-owned light data.
- `implicitSurfaceSceneIndexPlugin.h/.cpp`: scene-index registration used to convert supported implicit primitives before they reach the mesh adapter.

### `renderer/`: path tracing

- `renderer.h/.cpp`: `HdEmbreeRenderer` public façade, persistent frame state,
  settings, and the progressive preview/full-resolution render loop.
- `rendererImpl.h`: private inline math, ray, closure, spectral, and shading
  helpers shared by focused renderer translation units. It is not an extension
  point or installed API.
- `camera/camera.cpp`: camera and lens sampling, primary-ray construction,
  and ray differentials. Tile/pixel traversal remains in `renderer.cpp`.
- `aov/aovOutput.cpp`: AOV binding validation, clear/reset behavior, adaptive
  variance tracking, hit AOV evaluation, and format-specific writers.
- `integrator/pathIntegrator.cpp`: the lit multi-bounce integrator. It owns
  the camera intersection, all later surface/volume segments, emitter and
  environment hits, throughput, MIS, SSS, and Russian roulette.
- `integrator/unlitIntegrator.cpp`: the single-hit unlit integrator. It owns its
  camera intersection, MaterialX base color, camera-light shading, and AO.
- `integrator/surfaceShading.cpp`: shared hit-normal, derivative, MaterialX
  shading-context, and visibility-closure helpers.
- `integrator/lighting.cpp`: surface and participating-medium direct-light MIS.
- `integrator/visibility.cpp`: linked and transparent shadow traversal plus
  finite-light hit evaluation.
- `integrator/medium.h/.cpp`: participating-medium properties, phase functions,
  and free-flight utilities.
- `integrator/sss.h/.cpp`: random-walk subsurface scattering.
- `lights/light.h`: immutable-at-render-time light shapes, transforms,
  textures, IES/shaping distributions, links, and radiometric parameters.
- `lights/lightRegistry.h/.cpp`: synchronized ownership of path, dome, and
  finite-geometry lookup containers. The light records themselves are borrowed
  from the delegate.
- `lights/lightSamplers.h/.cpp`: analytic/environment sampling, directional
  PDFs, and MIS-facing light samples.
- `lights/lightLinking.h`: category sets and light/shadow-link matching.
- `lights/pxrIES/`: IES parser and photometric-profile wrapper.
- `materials/material.h`: stable renderer material handle referencing the
  currently compiled graph.
- `materials/mxcppAdapter.h/.cpp`: converts Hydra/MaterialX networks into
  canonical MaterialXCpp graphs.
- `materials/oiioTextureSystem.h/.cpp`: OpenImageIO texture implementation for
  MaterialXCpp.
- `materials/MaterialXCpp/`: CPU material graph compiler/evaluator, nodes,
  terminal models, closures, spectral support, and focused tests.
- `materials/BSDL/`: BSDF support library and generated lookup tables.
- `geometry/context.h`: Embree prototype and instance hit data: identities,
  properties, primvars, materials, derivatives, transforms, and categories.
- `geometry/primvarSampler.h/.cpp`: generic Hydra buffer and primvar sampling.
- `geometry/meshSamplers.h/.cpp`: constant, uniform, triangle,
  face-varying, and subdivision interpolation.
- `sampling/sampling.h`: OpenQMC sequence selection, stable sample-domain keys,
  and `Fork`, `Split`, `Distrib`, and `Chain` operations.
- `renderBuffer.h`: narrow AOV output interface implemented by the delegate.
- `config.h/.cpp`: startup defaults from `HDEMBREE_*` environment variables.
- `debugCodes.h/.cpp`: hdEmbree `TF_DEBUG` symbols.
- `pxrPbrt/pbrtUtils.h`: small utilities derived from PBRT conventions.

### `schema/`: render settings

- `schema.usda`: authored `TyphoonRenderSettingsAPI`.
- `generatedSchema.usda`: generated runtime schema consumed by USD.
- `generatedSchema.classes.txt`: CMake manifest. Source paths point into `schema/`; installed resource names remain unchanged.

## Delegate-to-renderer data flow

### Scene synchronization

1. Hydra creates `HdEmbreeRenderDelegate` through `HdEmbreeRendererPlugin`.
2. `CreateRprim/CreateSprim/CreateBprim` create mesh, material, light, and render-buffer adapters.
3. During `HdRenderIndex::SyncAll()`, Hydra calls each adapter's `Sync()`: meshes pull geometry/primvars/bindings/instances; materials compile networks; lights pull Lux/texture/IES/linking data; instancers update transforms and contexts.
4. Mutating adapters use `HdEmbreeRenderParam::AcquireSceneForEdit()` or `NotifySceneChange()`. This stops background rendering before shared state changes and increments the scene version.
5. Mesh prototypes/instances are attached to the top-level `RTCScene`. `HdEmbreePrototypeContext` and `HdEmbreeInstanceContext` make synchronized renderer data available at hits without retaining Hydra adapter objects.

### Frame execution

1. Hydra calls `HdEmbreeRenderPass::_Execute()`.
2. The pass compares scene/settings versions, frame/time, camera/framing, data window, and AOV bindings with the previous execution.
3. Changes are pushed through `HdEmbreeRenderer::Set*`. Values originate in delegate descriptors, scene-index `HdRenderSettingsSchema`, and `HdEmbreeConfig`.
4. If accumulation-relevant state changed, the pass stops the thread, resets as needed, and starts `HdEmbreeRenderer::Render()` on `HdRenderThread`.
5. The renderer writes bound `HdEmbreeRenderBuffer` objects. The pass exposes convergence and writes active `RenderProduct` files after convergence.

## Reading the renderer

Start in `renderer/renderer.cpp` and follow the implementation files rather
than reading one monolithic translation unit:

1. `HdEmbreeRenderer::Render()` in `renderer.cpp`: frame-level progressive
   loop. It prepares state, schedules preview/full-resolution passes, resolves
   AOVs, checks convergence, and handles cancellation.
2. `_RenderTiles()` in `renderer.cpp`: parallel tile/pixel traversal,
   cancellation, preview stride, and adaptive-pixel filtering. It calls
   `_SampleCameraRay()` in `camera/camera.cpp` for camera/lens sampling, primary
   rays, and ray differentials.
3. `_EvaluatePixelSample()` in `renderer.cpp`: selects exactly one radiance
   integrator, then updates adaptive variance and dispatches AOV writers using
   the selected integrator's retained primary hit. Geometric-AOV-only samples
   take a primary-intersection fast path.
4. `_IntegratePath()` in `integrator/pathIntegrator.cpp`: lit integration. Its
   first loop iteration traces and retains the camera hit; the same loop handles
   primary and secondary finite lights, camera background, indirect environment,
   material closures, direct light, BSDF/volume/SSS transport, and roulette.
5. `_IntegrateUnlit()` in `integrator/unlitIntegrator.cpp`: independent
   single-hit integration for material base color, camera light, and AO.
6. `integrator/surfaceShading.cpp`, `lighting.cpp`, `visibility.cpp`, and
   `sss.cpp`: shared shading and transport branches used by the integrators.
7. `_WriteColor()` and the other writers in `aov/aovOutput.cpp`: conversion of
   the returned color and retained primary hit into Hydra AOV storage.

Read `renderer.h` for persistent state and function contracts,
`geometry/context.h` for hit data, `sampling/sampling.h` for random domains,
`lights/lightSamplers.*` for light PDFs, and
`materials/MaterialXCpp/materials/bsdf.*` for closure evaluation and sampling.

## Extension points

### Materials and MaterialX nodes

- New terminal model: add a builder under `renderer/materials/MaterialXCpp/materials/`, add it to CMake, and register its node type in `nodeRegistry.cpp` or the appropriate node-family registrar.
- New MaterialX node: add an evaluator to the matching `nodes/*Nodes.cpp` and register every supported type signature in `Register*Nodes()`.
- Extend `renderer/materials/mxcppAdapter.cpp` when Hydra network normalization is needed.
- Transport changes must update value, PDF, sampling, lobe classification, MIS, and delta behavior consistently.
- Add focused graph/node/material tests and a rendered fixture when integration is significant.

### Lights

- Add Hydra support/creation tokens in `delegate/renderDelegate.cpp`.
- Pull authored data in `HdEmbree_Light::Sync()`; populate the runtime representation declared in `renderer/lights/light.h`.
- Add sampling and directional PDF logic in `renderer/lights/lightSamplers.*`.
- Add visible Embree geometry for finite camera-visible lights.
- Keep radiance evaluation, sampling PDF, normalization, shaping, IES, texture orientation, and linking consistent.
- Extend light-sampler tests and add a render fixture for visibility/synchronization.

### Sampling

- New user-selectable sequence: add enum/token/OpenQMC draw logic in `renderer/sampling/sampling.h`, then expose it through config, delegate settings, schema, README, and renderer setters.
- New stochastic decision: add a stable `HdEmbreeSampleDomainKey`. Never reuse or renumber existing values; they are part of deterministic rendering.
- Use `Fork` for fixed independent domains, `Split` for one of N samples, `Distrib` for distributed work, and `Chain` for indexed variable-length sequences.
- Never draw opportunistically from an unrelated domain.
- Update `testenv/testHdEmbreeSampling.cpp`.

### Geometry and primvars

- Advertise/create Hydra primitives in `delegate/renderDelegate.cpp`.
- Extract dirty data and build Embree state in the delegate adapter, acquiring `HdEmbreeRenderParam` before mutation.
- Put primitive IDs, culling/refinement flags, derivatives, material handles, and other hit-time state in renderer-owned contexts; do not make the renderer query the Hydra Rprim.
- Add interpolation to `renderer/geometry/meshSamplers.*` for new primvar representations.

### AOVs

- Accept/validate bindings in `delegate/renderPass.*`.
- Store accumulation state in `renderer/renderer.h`.
- Compute in `renderer/aov/aovOutput.cpp` or at the appropriate integrator stage.
- Add a format-aware `_Write*` path through `HdEmbreeRenderBufferInterface`; implement any new output operation in `delegate/renderBuffer.*`.

### Render settings

Update all relevant surfaces:

- token, descriptor, label, and default in `delegate/renderDelegate.*`;
- environment default in `renderer/config.*`, if appropriate;
- authored and generated files under `schema/`;
- bridge logic in `delegate/renderPass.cpp`;
- renderer setter/state in `renderer/renderer.*` and behavior in the owning `aov/`, `camera/`, or `integrator/` file;
- user documentation in `README.md`;
- coverage in `testenv/testHdEmbreeRenderSettings.cpp`.

Keep `README.md`, this document, generated schema data, and tests synchronized with implementation.
