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

- `renderer.h/.cpp`: `HdEmbreeRenderer`, the progressive path tracer. Holds camera/AOV/settings, lights, accumulation/adaptive buffers, and the main frame, tile, ray, lighting, medium, and path loops.
- `context.h`: Embree hit user data: renderer geometry identity/properties, primitive IDs and parameters, primvar samplers, material-data handle, derivative caches, instance transforms, and light-linking data.
- `config.h/.cpp`: startup defaults from `HDEMBREE_*` environment variables.
- `sampling.h`: OpenQMC sequence selection, stable sample-domain keys, and `Fork`, `Split`, `Distrib`, and `Chain` operations.
- `sampler.h/.cpp`: generic Hydra buffer and primvar type sampling.
- `meshSamplers.h/.cpp`: constant, uniform, triangle vertex/face-varying, and subdivision primvar interpolation.
- `light.h`: renderer-owned immutable-at-render-time light shapes, textures, IES/shaping distributions, transforms, linking tokens, and radiometric parameters.
- `material.h`: stable renderer material handle referencing the currently compiled graph.
- `renderBuffer.h`: narrow AOV output interface implemented by the Hydra render buffer adapter.
- `lightSamplers.h/.cpp`: light selection, analytic/environment sampling, directional PDFs, and MIS-facing sample records.
- `lightLinking.h`: category sets and light/shadow-link matching.
- `medium.h/.cpp`: participating-medium state, phase functions, and free-flight utilities.
- `sss.h/.cpp`: random-walk subsurface scattering.
- `mxcppAdapter.h/.cpp`: converts Hydra/MaterialX networks into canonical MaterialXCpp graphs.
- `oiioTextureSystem.h/.cpp`: OpenImageIO texture implementation for MaterialXCpp.
- `debugCodes.h/.cpp`: hdEmbree `TF_DEBUG` symbols.
- `MaterialXCpp/`: CPU material graph compiler/evaluator and shading library.
  - `graph.*`, `graphTypes.h`, `slots.*`, `paramMap.h`, and `value.h`: graph representation, compilation, connections, parameters, and values.
  - `nodeRegistry.*`: node-type-to-evaluator registration.
  - `nodes/`: math, geometry, texture, procedural, compositing, conditional, NPR, PBR, and helper evaluators.
  - `materials/`: terminal material models and BSDF closure trees for USD Preview Surface, Standard Surface, OpenPBR, Adobe OpenPBR, Disney Principled, and glTF PBR.
  - `shadingContext.h`, `surfaceClosure.h`, `surfaceShaderUtils.h`, and `spectral.h`: shading and spectral data/contracts.
  - `textureSystem.h`: renderer-independent texture interface.
  - `tests/`: MaterialXCpp tests and scene/image fixtures.
- `BSDL/`: BSDF support library and generated lookup tables.
- `pxrIES/`: IES parser and photometric-profile wrapper.
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

Start in `renderer/renderer.cpp` and follow:

1. `HdEmbreeRenderer::Render()`: frame-level progressive loop. Commits the Embree scene, prepares accumulation, schedules preview/full-resolution work, advances samples, resolves AOVs, checks convergence, and handles cancellation.
2. `_RenderTiles()`: parallel image loop. Maps work to pixels, creates sample domains, generates camera/lens samples, calls `_TraceRay()`, accumulates, and writes AOVs.
3. `_TraceRay()`: constructs the camera ray and obtains the first Embree hit. `_ComputeDepth()`, `_ComputeNormal()`, `_ComputeId()`, and `_ComputePrimvar()` interpret non-color AOVs.
4. `_ComputeColor()`: prepares surface state for beauty and enters the integrator.
5. `_TracePath()`: main bounce loop. Track ray, throughput, medium, material closure, linking, direct-light MIS, BSDF sampling, transmission/SSS, and Russian roulette.
6. `_ComputeDirectLightingMIS()`, `_ComputeMediumDirectLighting()`, `_TraceVolumeTransmission()`, and `sss.cpp`: major path-loop branches.
7. `_WriteColor()` and other `_Write*` helpers: convert accumulated results to Hydra AOV formats.

Read `renderer.h` for persistent state, `context.h` for hit data, `sampling.h` for random domains, `lightSamplers.*` for PDFs, and `MaterialXCpp/materials/bsdf.*` for closure evaluation/sampling.

## Extension points

### Materials and MaterialX nodes

- New terminal model: add a builder under `renderer/MaterialXCpp/materials/`, add it to CMake, and register its node type in `nodeRegistry.cpp` or the appropriate node-family registrar.
- New MaterialX node: add an evaluator to the matching `nodes/*Nodes.cpp` and register every supported type signature in `Register*Nodes()`.
- Extend `renderer/mxcppAdapter.cpp` when Hydra network normalization is needed.
- Transport changes must update value, PDF, sampling, lobe classification, MIS, and delta behavior consistently.
- Add focused graph/node/material tests and a rendered fixture when integration is significant.

### Lights

- Add Hydra support/creation tokens in `delegate/renderDelegate.cpp`.
- Pull authored data in `HdEmbree_Light::Sync()`; populate the runtime representation declared in `renderer/light.h`.
- Add sampling and directional PDF logic in `renderer/lightSamplers.*`.
- Add visible Embree geometry for finite camera-visible lights.
- Keep radiance evaluation, sampling PDF, normalization, shaping, IES, texture orientation, and linking consistent.
- Extend light-sampler tests and add a render fixture for visibility/synchronization.

### Sampling

- New user-selectable sequence: add enum/token/OpenQMC draw logic in `renderer/sampling.h`, then expose it through config, delegate settings, schema, README, and renderer setters.
- New stochastic decision: add a stable `HdEmbreeSampleDomainKey`. Never reuse or renumber existing values; they are part of deterministic rendering.
- Use `Fork` for fixed independent domains, `Split` for one of N samples, `Distrib` for distributed work, and `Chain` for indexed variable-length sequences.
- Never draw opportunistically from an unrelated domain.
- Update `testenv/testHdEmbreeSampling.cpp`.

### Geometry and primvars

- Advertise/create Hydra primitives in `delegate/renderDelegate.cpp`.
- Extract dirty data and build Embree state in the delegate adapter, acquiring `HdEmbreeRenderParam` before mutation.
- Put primitive IDs, culling/refinement flags, derivatives, material handles, and other hit-time state in renderer-owned contexts; do not make the renderer query the Hydra Rprim.
- Add interpolation to `renderer/meshSamplers.*` for new primvar representations.

### AOVs

- Accept/validate bindings in `delegate/renderPass.*`.
- Store accumulation state in `renderer/renderer.h`.
- Compute near the appropriate hit/path stage in `renderer.cpp`.
- Add a format-aware `_Write*` path through `HdEmbreeRenderBufferInterface`; implement any new output operation in `delegate/renderBuffer.*`.

### Render settings

Update all relevant surfaces:

- token, descriptor, label, and default in `delegate/renderDelegate.*`;
- environment default in `renderer/config.*`, if appropriate;
- authored and generated files under `schema/`;
- bridge logic in `delegate/renderPass.cpp`;
- renderer setter/state/behavior in `renderer/renderer.*`;
- user documentation in `README.md`;
- coverage in `testenv/testHdEmbreeRenderSettings.cpp`.

Keep `README.md`, this document, generated schema data, and tests synchronized with implementation.
