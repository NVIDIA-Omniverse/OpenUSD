# hdEmbree Architecture

This guide explains where hdEmbree code lives, how Hydra data reaches the path tracer, how a frame is rendered, and where new functionality belongs. See `README.md` for user-facing features and settings, and `AGENTS.md` for build and test workflow.

## Design boundary

hdEmbree is one plugin target with three source submodules:

- `delegate/`: Hydra-facing adapter. Creates Hydra primitives, pulls dirty scene data, translates render-pass state and settings, owns the render thread, and publishes Hydra render buffers.
- `renderer/`: CPU path tracer and support code. Owns progressive state, Embree traversal, path integration, sampling, shading, lighting, textures, media, and subsurface scattering.
- `schema/`: authored and generated `TyphoonRenderSettingsAPI` schema.

The intended dependency direction is `Hydra -> delegate -> renderer`. Renderer code depends only on renderer-owned runtime records and interfaces; delegate adapters populate or implement those contracts.

hdEmbree exposes no supported hand-written C++ API and installs no
hand-written headers. Hydra loads the plugin through `plugInfo.json`; the
supported external contract is:

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
| The renderer identifiers RenderLab matches on (`"HdEmbreeRendererPlugin"`, `"Embree"`) | `extras/usd/examples/usdviewPlugins/renderLab/renderSettingsMetadata.py` |
| RenderLab setting keys, a subset of the delegate descriptors | `extras/usd/examples/usdviewPlugins/renderLab/renderSettingsMetadata.py` |

The USD `TyphoonRenderSettingsAPI` schema is hdEmbree's supported external
settings interface. Hydra's direct delegate-settings path is an internal
application-control path, required by usdview and RenderLab, and is not a
consumer-facing C++ API. RenderLab uses `StageView.SetRendererSetting()` rather
than authoring USD. Its explicit metadata covers 25 of the 27 `ty:` attributes;
`ty:disableShadows` and `ty:textureCacheSize` use the default category. Its key
set must remain a subset of the delegate descriptors.

`usdrender -s` / `--set` is the per-invocation authoring path. It lives outside
hdEmbree in `pxr/usdImaging/bin/usdrender/`: the tool resolves `{settings}`
before applying overrides, validates targets against the unmodified composed
stage, parses each value in an isolated USDA scratch layer, and authors only
the resulting typed value into an anonymous session layer. This makes the
command-line opinion stronger than the root stage and a supplied
`--sessionLayer` without modifying either file.

## Common-quantity naming

First-party identifiers compose semantic suffixes in this order:

`<quantity><kind><transport/event role><coordinate space><orientation><representation or measure><reciprocal>`

Use full quantity roots: `normal`, `omega`, `radiance`, `position`, `distance`,
`anisotropy`, and role-specific `index`. The exceptions are uniform random
samples `u1`/`u2` and derivatives such as `dPdu`, `dPdv`, `dPdx`, and `dPdy`.
Use the fixed abbreviations `Geom`, `Srf`, `Shd`, `Wld`, `Obj`, and `Ext`;
other suffixes remain unabbreviated. `pdf`, `bsdf`, `rgb`, and `ior` are the
standard multi-letter acronym roots. The quantity always comes first:
`positionHitWld`, not `hitPositionWorld`; an output parameter appends
`Output` or `Result` rather than using an `Out` prefix.

A spatial value in shared state or a function interface states its coordinate
space. Transforms use `<from>To<to>`, such as `objToWld`. Omit a suffix only
when that semantic dimension does not apply, not because a function currently
uses only one space or normal kind.

For directional quantities, `In` and `Out` identify transport sides:
`omegaIn` points toward the next
vertex or light and `omegaOut` points toward the previous vertex or camera.
They do not mean an object's interior/exterior or an outward normal. Normal
orientation uses `Ext` for the authored exterior and `Out`/`In` only when
faced toward the corresponding transport direction. IOR names are the explicit
optics exception: `iorIn` is the incident medium before a crossing and
`iorOut` is the transmitted medium after it, independent of `omegaIn` and
`omegaOut`. `eta` is reserved for the ratio `iorIn / iorOut`.
Probability-density names state their measure and append `Inverse` for a
reciprocal.

External, generated, and authored names are boundaries. Embree fields such as
`RTCHit::Ng`, MaterialX port tokens, and vendored BSDL identifiers retain their
external spelling; first-party code copies them immediately into a name below.
Types below are renderer-native; first-party MaterialXCpp uses its
corresponding type without changing the semantic name.

| Name | Type | Meaning and invariants |
| --- | --- | --- |
| `positionWld` | `GfVec3f` | World-space point. Add event/usage suffixes before space, such as `positionHitWld`, `positionEntryWld`, or `positionRayOriginWld`, when multiple positions coexist. |
| `positionObj` | `GfVec3f` | Object-space point belonging to the current prototype. |
| `normalGeomWldExt` | `GfVec3f` | Normalized geometric normal transformed from Embree `RTCHit::Ng`, corrected for authored orientation, and pointing toward the authored exterior. Immutable topology and boundary authority. |
| `normalGeomWldOut` | `GfVec3f` | Geometric exterior normal faced toward `omegaOutWld`. Never material-resolved. |
| `normalGeomObjExt` | `GfVec3f` | Object-space counterpart used only where object and world geometric normals coexist. |
| `normalSrfWldExt` | `GfVec3f` | Normalized smooth/displaced shading normal, view independent, aligned with `normalGeomWldExt`, and containing no material normal-map result. |
| `normalSrfWldOut` | `GfVec3f` | Surface normal faced toward `omegaOutWld`; material-normal fallback and differential source. |
| `normalShdWldOut` | `GfVec3f` | Material-resolved shading normal faced toward `omegaOutWld`; falls back to `normalSrfWldOut` and never owns topology or medium transitions. |
| `tangentWld`, `bitangentWld` | `GfVec3f` | World-space material frame paired with `normalShdWldOut`; use `Obj` or `Tangent` suffixes for other spaces. |
| `omegaInWld` | `GfVec3f` | Normalized incident direction from the interaction toward the sampled next vertex or light. Replaces `wi`, `wI`, and ambiguous `direction` when this meaning applies. |
| `omegaOutWld` | `GfVec3f` | Normalized exitant direction from the interaction toward the previous path vertex or camera. Replaces `wo` and `wO`. |
| `positionRayOriginWld`, `directionRayWld` | `GfVec3f` | Origin and forward travel direction of a generic ray segment. Use `directionShadowWld`, `directionEntryWld`, etc. when it is not a local scattering omega. |
| `iorIn`, `iorOut` | `float` | Absolute IORs in the optics convention: incident medium before a crossing and transmitted medium after it. They do not name the `omegaIn`/`omegaOut` sides. `1.0f` represents vacuum/air and glass is commonly about `1.5f`. |
| `eta` | `float` | Explicit ratio `iorIn / iorOut`. `1.0f` means no IOR change; special sentinel behavior such as zero must be documented by the owning API. |
| `radianceIn` | `GfVec3f` | Incident RGB radiance carried from a sampled/evaluated light. Replaces `Li`. Use `radianceInSpectral` for a hero-wavelength scalar. |
| `radianceEmitted` | `GfVec3f` | RGB radiance emitted by a light or surface. Replaces `Le`. |
| `radianceAccumulated` | `GfVec3f` | RGB radiance accumulated for the current camera path/sample. Qualify direct, indirect, or spectral variants when they coexist. |
| `throughputRgb` | `GfVec3f` | RGB path-throughput multiplier accumulated from the camera to the current segment. |
| `throughputSpectral` | `float` | Hero-wavelength counterpart to `throughputRgb`. Use `throughputWeight` only for an unapplied local returned multiplier. |
| `bsdfValue` | `GfVec3f` or `mxcpp::Vec3f` | Evaluated or sampled BSDF value. Replaces bare `f`; qualify spectral/scalar forms when required. |
| `pdf` | `float` | Non-negative probability density whose measure is explicit in its suffix or declaration contract. |
| `pdfSolidAngle`, `pdfSolidAngleInverse` | `float` | Density and reciprocal density with respect to solid angle. Replace `pdfW` / `invPdfW`. |
| `pdfArea`, `pdfAreaInverse` | `float` | Density and reciprocal density with respect to surface area. Replace `pdfA` / `invPdfA`. |
| `distanceWld` | `float` | World-space distance. Add roles such as `distanceRemainingWld`, `distanceScatterWld`, or `distanceOppositeWld`. Replace `dist` and ray-parameter `t` where they represent distance. |
| `u1`, `u2` | `float` | Independent uniform random samples in `[0, 1)`. These are the explicit short-name exception. Use `coordinateParametricU` / `coordinateParametricV` or `coordinateTextureU` / `coordinateTextureV` for coordinates rather than random samples. |
| `absorption` | `GfVec3f` | Per-channel absorption coefficient in inverse world units. Replaces `sigmaA`. |
| `scattering` | `GfVec3f` | Per-channel scattering coefficient in inverse world units. Replaces `sigmaS`. |
| `extinction` | `GfVec3f` | Per-channel extinction coefficient: `absorption + scattering`. Replaces `sigmaT`; do not call this transmission. |
| `absorptionIndex` | `GfVec3f` | Dimensionless imaginary part of a conductor's complex IOR. Replaces the optics symbol `kappa`; it is not a medium absorption or extinction coefficient. |
| `anisotropy` | `float` | Henyey-Greenstein anisotropy, clamped to the owning model's documented range. Replaces bare `g`. |
| `albedo` | `GfVec3f` | Unitless per-channel scattering/reflectance ratio, normally in `[0, 1]`; qualify surface or volume variants when both coexist. |
| `cosTheta` | `float` | Cosine of the relevant angle. Add a role suffix such as `cosThetaIn`, `cosThetaOut`, or `cosThetaLight` when multiple angles coexist. Keep bare `cosTheta` only when the role is unambiguous. |
| `index` | `int` or unsigned index type | Generic collection index. Prefer `indexBounce`, `indexChannel`, `indexLight`, etc.; replace loop variables `i`, `j`, and `k`. |

## Repository map

### Plugin root

- `CMakeLists.txt`: plugin sources, dependencies, installed resources, compile definitions, and C++ tests.
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
- `renderParam.h`: synchronization bridge. Scene edits stop rendering, acquire the Embree scene for mutation, and increment the scene version used to restart accumulation. Edits that can alter generated displacement also increment a narrower displacement version used to schedule prototype retessellation.
- `renderPass.h/.cpp`: converts `HdRenderPassState`, camera, framing, AOVs, wire color/width, scene-index render settings/products, and delegate settings into renderer setters. Starts/restarts rendering, reports convergence, and writes active render products.
  It requires an attached `HdCamera` and snapshots the first valid camera/data window for screen-space subdivision. Scene edits reuse that snapshot; `ty:dynamicSubdvTesselation` enables resnapshotting and recomputation after projection or data-window changes.
- `renderBuffer.h/.cpp`: CPU-backed `HdRenderBuffer` storage, mapping, format conversion, convergence, and renderer write access.
- `mesh.h/.cpp`: `HdMesh` adapter. Pulls topology, points, transforms, subdivision data, primvars, materials, categories, active repr, and instancing; builds/updates Embree prototypes and instances. The mesh uniquely owns one prototype context and one context in each instance record; Embree borrows their stable addresses as geometry user data. It applies levels computed by `adaptiveSubdivision.*`, stores wireframe topology/display state in the prototype context, and supplies the Embree subdivision displacement callback declared in `displacement.h`.
- `adaptiveSubdivision.h/.cpp`: deterministic screen-space edge projection, guarded homogeneous view-volume clipping, shared-edge/instance maximum selection, complexity targets, fixed 3x3 displaced-quad chord probes, Embree level clamping, and quad transition balancing.
- `instancer.h/.cpp`: `HdInstancer` adapter; computes instance transforms and per-instance category/light-linking context.
- `material.h/.cpp`: `HdMaterial` adapter; pulls Hydra networks, normalizes them through `mxcppAdapter`, owns separate compiled surface and optional displacement `mxcpp::EvalGraph` objects, and updates a stable renderer material-data handle.
- `light.h/.cpp`: `HdLight` adapter. Pulls USD Lux parameters, transforms, textures, IES data, shaping, linking, and visible finite-light geometry into renderer-owned light data.
- `implicitSurfaceSceneIndexPlugin.h/.cpp`: scene-index registration used to convert supported implicit primitives before they reach the mesh adapter.

### `renderer/`: path tracing

- `renderer.h/.cpp`: `HdEmbreeRenderer` central façade, persistent frame state,
  settings, and the progressive preview/full-resolution render loop.
- `rendererImpl.h`: private inline math, ray, closure, spectral, and shading
  helpers shared by focused renderer translation units.
- `camera/camera.cpp`: camera and lens sampling, primary-ray construction,
  and ray differentials. Tile/pixel traversal remains in `renderer.cpp`.
- `aov/aovOutput.cpp`: AOV binding validation, clear/reset behavior, adaptive
  variance tracking, hit AOV evaluation, and format-specific buffer writes.
- `integrator/pathIntegrator.cpp`: the lit multi-bounce control loop. It owns
  the primary hit, surface-event ordering, path state, throughput, BSDF
  continuation, bounce limits, and Russian roulette.
- `integrator/volumeTransport.cpp`: active-medium segment attenuation,
  free-flight scattering, medium roulette, and medium-boundary ownership.
- `integrator/unlitIntegrator.cpp`: the single-hit unlit integrator. It owns its
  camera intersection, MaterialX base color, camera-light shading, and AO.
- `integrator/surfaceShading.cpp`: shared hit-normal, MaterialX shading-context,
  visibility-closure, display-wire composition, and ray-differential
  propagation helpers.
- `integrator/lighting.cpp`: surface and participating-medium direct-light MIS,
  plus camera-background and indirect-environment evaluation.
- `integrator/visibility.cpp`: linked and transparent shadow traversal plus
  finite-light hit evaluation.
- `integrator/medium.h/.cpp`: participating-medium properties, phase functions,
  and free-flight utilities.
- `integrator/sss.h/.cpp`: subsurface entry/exit orchestration and the
  self-contained random walk.
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
- `materials/materialEvalContext.h`: stable renderer-owned texture, frame, and
  time services borrowed by geometry-build and hit-time material evaluation.
- `materials/MaterialXCpp/`: CPU material graph compiler/evaluator, nodes,
  terminal models, closures, spectral support, and focused tests. Compilation
  returns explicit valid, invalid-with-diagnostic, or absent-terminal results;
  only valid results own an evaluation graph. A volume terminal without a
  surface is explicitly wrapped in the internal transparent surface-volume
  model; this is not arbitrary terminal substitution. Volume-only closures
  retain explicit boundary identity even when their evaluated coefficients are
  vacuum, while `hasInteriorMedium` remains reserved for active medium state.
  Surface-shader mixes retain that identity only when every contributing input
  is a volume boundary. Zero-weight endpoint branches contribute neither their
  BSDF tree nor their medium state. Terminal support validation uses the same
  dispatch function as evaluation so the accepted model set cannot drift
  separately.
- `materials/BSDL/`: BSDF support library and generated lookup tables.
- `geometry/context.h`: Embree prototype and instance hit data: identities,
  properties, primvars, materials, derivatives, transforms, and categories.
- `geometry/displacementEvaluation.h/.cpp`: shared build-time and hit-time
  displacement evaluation, transform-correct object-space offsets, final
  displaced-position probes, smooth displaced subdivision-frame
  reconstruction, and lazy displaced-normal curvature evaluation. Recoverable
  authored failures return false; unexpected exceptions at Embree's C callback
  boundary leave the affected lane undisplaced, emit one runtime error per
  prototype commit, and do not unwind through Embree.
- `geometry/primvarSampler.h/.cpp`: generic Hydra buffer and primvar sampling.
- `geometry/meshSamplers.h/.cpp`: constant, uniform, triangle,
  face-varying, and subdivision interpolation.
- `geometry/wireframe.h/.cpp`: screen-space triangle-edge coverage and
  reconstruction of Embree's final diced subdivision grid from coarse-face
  layout, hit UVs, ray differentials, and live edge levels.
- `sampling/sampling.h`: OpenQMC sequence selection, stable sample-domain keys,
  and `Fork`, `Split`, `Distrib`, and `Chain` operations.
- `renderBuffer.h`: narrow AOV output interface implemented by the delegate.
- `renderSettings.h`: the renderer-consumed settings value, hard-coded
  defaults, token conversions, and pass-owned setting defaults.
- `debugCodes.h/.cpp`: hdEmbree `TF_DEBUG` symbols.

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
5. Mesh prototypes/instances are attached to the top-level `RTCScene`.
   `HdEmbreePrototypeContext` and `HdEmbreeInstanceContext` make synchronized
   renderer data available at hits without retaining Hydra adapter objects.
6. At low complexity, subdivision meshes render as triangulated control cages.
   For medium and higher, the pass projects authored control edges through the
   stored subdivision view/projection and all instance transforms. Projection
   uses a 10% X/Y view guard for displaced patches, converts complexity to a
   pixel-edge target, and clamps candidate Embree levels to `[4, 4096]`. A
   monotone fixed-point pass raises levels until shared authored edges agree
   and opposite edges of every authored quad differ by at most 2:1. Displaced
   quads then evaluate final object-space positions at the 3x3 product of
   `u,v={0,0.5,1}`. For every instance, screen-space midpoint-to-chord error is
   measured along three rows and three columns. Errors above 0.5 pixel at
   medium or 0.25 pixel at high/very-high double the corresponding u- or
   v-direction levels, capped at twice the freshly computed camera baseline.
   The boost factor first propagates through shared edges and quad-opposite
   pairs along the complete edge strip. This uniformly scales any pre-existing
   camera transition pattern and prevents a local boost from creating Embree
   stitch-fan triangles that may fold on the displaced surface. Shared-edge
   and opposite-edge balancing then run again. Failed probes and general
   n-gons retain their camera baseline except on a shared boosted edge. The
   mesh adapter updates persistent level buffers and recommits only the
   affected prototype and instances.

The active Hydra mesh repr is copied into each prototype context. After the
selected lit or unlit integrator returns, `_ApplyWireframe()` interprets the
retained camera hit, reconstructs parametric pixel derivatives, and composites
`wireOnSurf`/`refinedWireOnSurf` before adaptive variance and color AOV
accumulation. Coarse geometry uses hit-triangle barycentrics. Refined geometry
uses the live edge-level buffer, including n-gon's encoded subdivision
sub-patches, to reproduce the diced grid on the displaced surface. The
wireframe path converts the texture-filter derivatives back to a one-pixel
footprint and uses continuous analytic coverage. Measured derivatives determine
line coverage, and use Embree's geometric hit parameterization directly rather
than the material shading context's authored `st` domain. Every regular diced
U/V edge and cell diagonal participates in coverage, including subpixel cells;
there is no coarser display LOD. Embree's public hit record does not identify a
private
transition-fan micro-triangle, so unequal-edge stitch diagonals remain an
approximation. Edge-only reprs return immediately after the primary camera hit,
skipping material evaluation, lighting, volumes, ambient occlusion, and path
bounces. They composite opaque black coverage over the clear color, blend
non-edge camera samples back to that clear color, and intentionally do not
perform a second traversal for rear edges.
`HdEmbreeRenderPass::_MarkCollectionDirty()` marks rprims `DirtyRepr` whenever
the collection repr selector or forced-repr state changes. This is required
because Hydra only rebuilds its dirty list the first time it encounters a
selector; a later return to that selector would otherwise skip the mesh.
`HdEmbreeMesh::_InitRepr()` then marks every resolved repr transition with
`NewRepr`, including a return to a previously registered repr. `Sync()` clears
that bit after the prototype context receives its new wireframe mode.

Displacement is active only for refined geometry whose display style permits
it and whose material has a displacement terminal. The Embree callback and
hit-time frame reconstruction share one evaluator. Renderer-owned texture,
frame/time, transform, authored `st`, geomprop, and uniform-property state is
borrowed read-only by the prototype context. A displacement value is a
world-space distance along the semantic world normal; it is converted back to
a prototype-object-space offset so direction and magnitude remain correct
under non-uniform scale and reflection. Graph failures and non-finite results
leave the generated vertex unchanged.

Material displacement edits and frame/time changes use a dedicated version
path so displaced prototypes are recommitted even when their level buffers are
unchanged. Instance-only edits do not force displacement reevaluation when the
adaptive levels remain unchanged. A shared point-instancer prototype is
displaced at prototype-scene commit time, so its callback can see the rprim
transform and prototype primvars but not point-instancer transforms or
per-instance primvars; the mesh adapter warns when this limitation applies.

Embree interpolation at a hit intentionally returns the undisplaced limit
surface. For a displaced hit, `displacementEvaluation.*` samples the center and
neighboring u/v locations, finite-differences the object-space offsets, and
reconstructs smooth displaced `dPdu`, `dPdv`, and their cross-product normal.
This frame feeds material shading, while the actual displaced-facet
`RTCHit::Ng` remains the geometric authority for visibility, medium-boundary
classification, and ray bias.

The hit-local displaced frame retains its center/U/V probes. If a surviving
BSDF sample is delta-specular and still carries ray differentials, the path
integrator evaluates only the outer UU/UV/VV ring and finite-differences the
final normalized smooth normals to obtain `dNdu` and `dNdv`. It remaps those
patch derivatives through the authored `st` inverse Jacobian and transforms
the normalized-normal derivative with the inverse transpose, including the
normalization derivative required by non-uniform instance scale. Diffuse,
glossy, differential-free, and undisplaced hits do not evaluate the outer
ring. A failed or degenerate ring conservatively keeps zero curvature.
These derivatives describe the smooth displaced geometry. Their lifecycle is
tracked separately from resolved-normal derivative provenance: smooth base
normals may use the ready base approximation, displaced curvature remains
lazy, and a materially changed normal has no derivatives until the material
normal graph is differentiated. Base dN is never silently paired with a mapped
normal.

Embree position buffers are Embree-owned `FLOAT3` data with a 16-byte stride,
and triangle index buffers are also Embree-owned so `VtArray` copy-on-write
cannot invalidate borrowed storage. Refined primvars use Embree vertex
attributes. Topology 0 samples smooth vertex data, topology 1 reuses mesh
indices with `PIN_ALL` for varying data, and every face-varying primvar owns
another topology carrying its authored index array. This per-primvar topology
is necessary because UV/color seams need not agree. Embree lacks separate
modes for OpenSubdiv's three corner variants; they share `PIN_CORNERS`, while
`none`, `boundaries`, and `all` remain distinct. Attribute buffers and
interpolation outputs are 16-byte padded because Embree may use SIMD-width
loads/stores for scalar and short-vector values.

Adaptive levels and displacement alter generated primitives, so mesh geometry
uses low-quality rebuilds rather than vertex-only refits. Prototype mutation
follows Embree's required order: commit prototype geometry, commit the
prototype scene, recommit every referencing instance geometry, then commit the
root scene before rendering. Both prototype and root scenes enable robust
traversal to reduce ray leaks at dense displaced patch boundaries.

### Frame execution

1. Hydra calls `HdEmbreeRenderPass::_Execute()`.
2. The pass compares scene/settings versions, frame/time, camera/framing, data window, and AOV bindings with the previous execution.
3. The pass resolves delegate and scene-index `HdRenderSettingsSchema` values,
   then applies renderer-consumed settings through one
   `HdEmbreeRenderer::SetRenderSettings()` call. Camera, framing, AOV, scene,
   and wireframe state retain their dedicated setters.
4. If accumulation-relevant state changed, the pass stops the thread, resets as needed, and starts `HdEmbreeRenderer::Render()` on `HdRenderThread`.
5. Before scene commit or buffer mapping, the renderer validates that the
   scene exists, every AOV is an hdEmbree buffer with a supported format and
   matching non-zero dimensions, and the data window is non-empty and
   contained. Failure maps no buffers, traces no tiles, marks usable buffers
   converged to park Hydra, and returns from `Render()`.
6. Successful setup commits the scene, builds frame/AOV state, and maps every
   buffer exactly once. Rendering resolves the buffers, then unmaps each one
   exactly once and marks it converged.
7. The pass exposes convergence and, for offline clients with
   `enableInteractive = false`, writes active `RenderProduct` files after
   convergence. Setup failure is not yet distinguishable from genuine
   convergence at this boundary, so suppressing stale offline output requires
   a separate renderer-to-pass failure signal.

## How a pixel sample becomes a path

### Pixel and camera sampling

1. `Render()` divides the active data window into tiles and schedules
   `_RenderTiles()` with `WorkParallelForN`. Coarse preview passes use a pixel
   stride greater than one; full-resolution passes use stride one. Pixels
   already converged under adaptive sampling are skipped.
2. Each selected pixel gets one `HdEmbreeSampler`, keyed by the frame seed,
   pixel coordinates, sample number, and configured OpenQMC sequence. Every
   later stochastic decision derives a named domain from this root; it must not
   consume unrelated domains opportunistically.
3. `_SampleCameraRay()` converts the pixel to NDC, optionally draws camera
   jitter, unprojects through the projection matrix, and constructs either a
   perspective or orthographic camera ray. When depth of field is enabled it
   draws a lens point, focuses the ray, and applies the same lens point to the
   x/y differential rays. The origin, normalized direction, and scaled ray
   differentials are transformed to world space.
4. `_EvaluatePixelSample()` chooses exactly one radiance integrator:
   `_IntegratePath()` when scene lighting is enabled or `_IntegrateUnlit()`
   otherwise. Both integrators trace their own camera ray and return a
   `_PixelSampleResult` containing linear RGBA radiance and the unchanged first
   Embree result in `primaryHit`.
5. If neither a color AOV nor adaptive sampling needs radiance,
   `_EvaluatePixelSample()` takes the geometric-AOV fast path: it intersects the
   primary ray once without invoking either radiance integrator.

The retained `primaryHit` is deliberately distinct from the final path event.
For example, a stochastic-presence surface may be retained for depth, ID,
normal, and primvar AOVs even though the radiance path passes through it and
continues to another surface or the environment.

### Lit path state

`_IntegratePath()` creates one `_PathState` shared by the main loop and its
volume, environment, and SSS handlers. It contains:

- accumulated RGB `radiance`;
- RGB or hero-wavelength `throughput`;
- current ray and ray differentials;
- current participating medium;
- the previous BSDF PDF and light-sampling mode used for MIS;
- receiver categories used for light and shadow linking;
- diffuse/specular ancestry used by caustic policy;
- first-segment state, bounce count, and a separate path-event index.

The bounce count is incremented explicitly only after a surface or medium
scattering event. Null-presence pass-throughs, volume-only boundaries, and a
synthetic SSS exit continue without changing it. The path-event index advances
on every loop iteration, so each receives a distinct `PathBounce` sample domain
even when it does not consume the user's bounce budget.

### Lit segment loop

For each segment, `_IntegratePath()` performs these stages in order:

1. **Intersect the segment.** A normal segment is populated with the camera ray
   mask, zero `tnear` for the initial camera ray, and a small positive `tnear`
   for later segments. The extra iteration after the configured maximum bounce
   uses the light-only mask: it may collect an emitter reached by the final BSDF
   sample but may not shade another surface. A successful SSS random walk can
   instead supply a synthetic exit hit.
2. **Retain the primary result.** The first intersection or miss is copied to
   `_PixelSampleResult::primaryHit` exactly once. It is never overwritten by
   later surfaces, volume events, emitters, or environment misses.
3. **Resolve candidate events.** The renderer identifies finite-light geometry
   represented in Embree. On non-camera segments it also tests analytic finite
   lights and compares their distance with the nearest ordinary surface.
4. **Transport through the active medium.** `_TraceVolumeTransmission()` in
   `volumeTransport.cpp` runs before surface shading. It applies absorption/transmittance and compares a
   sampled free-flight distance with both surface and finite-light distances.
   It can:

   - terminate at an attenuated finite-light hit;
   - create a volume-scattering event, add direct medium lighting, sample the
     phase function, and continue with a new ray;
   - attenuate throughput to the pending surface and allow surface processing
     to continue; or
   - terminate when throughput, sampling, bounce, or roulette conditions fail.

5. **Handle a finite emitter.** If a finite light is closer than the ordinary
   surface, its emitted radiance is accumulated and the path ends. Secondary
   hits must match the previous receiver's light-link categories. When the
   previous direction came from a finite-PDF BSDF sample, the contribution is
   MIS-weighted against the light-sampling technique; a primary camera hit has
   neither previous-link filtering nor MIS weighting.
6. **Handle a miss.** `_AccumulateEnvironment()` in `lighting.cpp` applies
   segment-specific camera or indirect policy:

   - A camera miss returns the clear color when no dome is registered or dome
     camera visibility is disabled. Otherwise it evaluates visible domes in the
     camera direction. Distant lights are not camera backgrounds.
   - An indirect miss evaluates visible distant and dome lights. It applies
     light linking from the previous scattering surface and MIS against the
     previous BSDF PDF. Dome camera visibility does not suppress indirect dome
     illumination.

7. **Reject an ordinary surface on the emitter-only iteration.** Once the
   surface-bounce budget is exhausted, the extra iteration exists only to see
   an emitter. Hitting another non-emissive surface ends the path.
8. **Construct surface state.** Renderer-owned instance and prototype context
   records provide the material, transforms, primvars, derivatives, categories,
   and geometry flags without querying Hydra. The integrator computes the hit
   position and constructs one central surface interaction with distinct
   topology and shading state:

   - `normalGeomWldExt` is the normalized, orientation-correct Embree facet
     normal. It points toward the authored exterior and alone controls boundary
     crossings, medium ownership, ray offsets, and geometric
     reflection/transmission;
   - `normalGeomWldOut` is its copy faced toward `omegaOutWld`;
   - `normalSrfWldExt` is the view-independent smooth/displaced pre-material
     normal aligned with `normalGeomWldExt`;
   - `normalSrfWldOut` is its exitant-facing copy and material-normal fallback;
   - `normalShdWldOut` is the resolved material normal, or
     `normalSrfWldOut` when resolution fails;
   - `frontFacing` is computed once from
     `dot(normalGeomWldExt, omegaOutWld)`.

   The side transform flips the surface normal and its derivatives on back
   faces, preserves `dP` and the
   authored tangent orientation, and reconstructs the bitangent from recorded
   outward-frame handedness. All normal vectors use inverse-transpose
   transforms under non-uniform instance transforms. `_BuildShadingContext()`
   supplies texture coordinates,
   display color, the reconstructed displaced tangent frame when available,
   geomprop lookup, uniform primvars, and surface/ray derivatives.
9. **Evaluate the material.** The bound `mxcpp::EvalGraph` produces a
   `SurfaceClosure`. Malformed graphs are rejected during compilation, so
   hit-time evaluation does not use exceptions for authored-value, missing
   input, or type-mismatch failures. Missing or rejected surface graphs leave a
   synthetic diffuse fallback available for direct lighting; displacement-only
   materials intentionally use that fallback, while volume-only materials use
   a synthesized transparent medium boundary. Exceptions from OIIO handle
   resolution, sampling, and color conversion are caught around those backend
   calls, reported once with the filename, and return the authored texture
   default. A synthetic SSS exit replaces the material with a
   unit Lambertian closure so subsurface albedo is not counted twice. Material
   normal inputs are resolved before BSDF work. Coupled
   dielectric closures carry an explicit combined reflection/refraction
   compensation policy enabled by OpenPBR and metalness-workflow
   UsdPreviewSurface. Missing energy is restored with an additive cosine
   multiple-scattering lobe rather than scaling the glossy lobes. Rough coupled
   glossy events select reflection or refraction from exact Fresnel after
   sampling the visible microfacet. Straight-shadow attenuation for these
   coupled interfaces uses exact Fresnel through alpha 0.002, blends
   smoothly to a front/back directional transmission LUT through alpha 0.07,
   and uses the LUT directly above that band. Its analytic smooth row,
   quadratic near-zero roughness spacing, and uniform/visible-normal MIS bake
   preserve the narrow transmission lobe and critical-angle transition near
   that limit.
   Standard Surface retains separate reflection and transmission lobes; its
   thin-walled transmission uses IOR 1 so the transmitted direction remains
   undeflected.
10. **Apply stochastic presence.** Presence is treated as the probability of a
    real interaction. A rejected interaction advances the ray beyond the hit,
    preserves first-bounce and MIS state, consumes a new path-event domain, and
    does not consume a bounce.
11. **Prepare transport policy and sample the closure.** Caustic-class lobes may
    be pruned after a diffuse-like ancestor when caustics are disabled. A
    dispersive closure initializes hero-wavelength transport. The integrator
    prepares the native or Adobe OpenPBR surface and draws the BSDF sample that
    would produce the next segment.
12. **Handle subsurface scattering.** `_TraceSubsurface()` in `sss.cpp` applies
    the selected entry direction and weight, then `HdEmbreeRandomWalkSSS()` walks inside
    the owning mesh. A successful exit becomes a synthetic Lambertian hit on
    the next loop iteration. The complete entry, random walk, and exit consume
    one surface bounce; failure terminates the path.
13. **Accumulate local radiance.** Material emission is added through current
    throughput. `_ComputeDirectLightingMIS()` performs next-event estimation
    for BSDF surfaces, including light selection, linking, colored visibility,
    active-medium attenuation, and MIS against BSDF sampling. A surface without
    a usable material closure receives the renderer's synthetic diffuse direct
    lighting fallback.
14. **Cross volume-only boundaries.** A closure that only defines an interior
    medium updates the active medium on entry or exit, offsets the unchanged ray
    across the boundary, and continues without consuming a surface bounce.
15. **Enforce the bounce budget.** At the last allowed surface, a valid BSDF
    sample may be followed by one emitter-only segment. Otherwise integration
    stops after the local emission and direct-light contributions.
16. **Advance path throughput.** For a valid non-SSS BSDF sample, the integrator
    applies `f * abs(cos(theta)) / pdf`; delta events use their direct throughput
    coefficient. It records the BSDF PDF, receiver categories, dome-sampling
    hemisphere, diffuse/specular ancestry, and any medium-boundary crossing for
    the next segment.
17. **Apply roulette and differentials.** After the configured minimum bounce,
    Russian roulette terminates low-throughput paths and compensates survivors.
    A surviving displaced delta event with an active ray footprint lazily
    completes its smooth-normal curvature from three additional displacement
    graph probes; all other events skip that work.
    `_PropagateRayDifferential()` in `surfaceShading.cpp` propagates specular
    reflection/refraction differentials; non-specular events discard them. The
    next origin is biased to the appropriate side of the oriented Embree facet
    normal, and the loop continues.

When the loop terminates, accumulated radiance is clamped non-negative, packed
with alpha one, and returned with the retained primary hit. Firefly and caustic
clamps are applied when contributions are added, before this final packing.

### Unlit integration

`_IntegrateUnlit()` is an independent single-hit integrator, not a special
branch inside the lit path:

1. It intersects the camera ray and stores that result as `primaryHit`.
2. A miss returns the configured clear color. Finite-light geometry returns
   black because scene lighting is explicitly disabled.
3. For an ordinary surface it resolves instance/prototype state, hit position,
   smooth normal, shading context, tangent frame, and MaterialX closure.
4. The output color is the closure's base color, or display color/default gray
   when no closure is available, multiplied by the camera-facing headlight.
5. When enabled, `_ComputeAmbientOcclusion()` stratifies cosine-weighted
   hemisphere samples and traces Embree occlusion rays; its visibility average
   attenuates the headlight result.
6. The integrator returns linear RGBA and the same primary hit used for shading.

It performs no scene-light sampling, emissive transport, indirect bounces, MIS,
participating-medium transport, or Russian roulette.

### Accumulation and AOV output

`_PreRenderSetup()` rebuilds AOV validation and output classification every render;
it does not cache validation because a bound buffer can change format or
dimensions without changing its pointer.
The renderer publishes synchronized `Pending`, `Valid`, or `Failed` frame
status across the render and client threads. Offline RenderProducts are written
only after the current frame is both valid and converged; setup failure still
marks usable AOVs converged to park the render thread without writing output.

After the selected integrator returns, `_EvaluatePixelSample()` applies any
active mesh wireframe repr to the retained camera hit, then updates the
per-pixel mean and variance used by adaptive convergence. It then passes each
classified AOV through `_WriteAov()`'s direct switch:

- color output consumes the returned radiance and applies camera exposure only at
  output;
- depth, normal, ID, and primvar output interprets the retained `primaryHit`;
- heatmap output consumes adaptive sample counts rather than scene radiance.

The render buffer accumulates samples until resolve/convergence. Thus transport
is owned by one selected integrator, while accumulation, format conversion, and
Hydra buffer writes remain renderer/AOV responsibilities.

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
   integrator, then updates adaptive variance and writes classified AOVs using
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
7. `_WriteAov()` in `aov/aovOutput.cpp`: direct conversion of the returned
   color and retained primary hit into Hydra AOV storage.

Read `renderer.h` for persistent state and function contracts,
`geometry/context.h` for hit data, `sampling/sampling.h` for random domains,
`lights/lightSamplers.*` for light PDFs, and
`materials/MaterialXCpp/materials/bsdf.*` for closure evaluation and sampling.

## Changing the plugin

### Materials and MaterialX nodes

- New terminal model: add a builder under `renderer/materials/MaterialXCpp/materials/`, add it to CMake, and register its node type in `nodeRegistry.cpp` or the appropriate node-family registrar.
- New MaterialX node: add an evaluator to the matching `nodes/*Nodes.cpp` and register every supported type signature in `Register*Nodes()`.
- Extend `renderer/materials/mxcppAdapter.cpp` when Hydra network normalization is needed.
- Transport changes must update value, PDF, sampling, lobe classification, MIS, and delta behavior consistently.
- Add focused graph/node/material tests and a rendered fixture when integration is significant. Renderer-level material and transport regressions live under the root-level `materials/` suite in the external `typhoon-test-suite` repository; broad node-value coverage stays in `testMaterialXCpp`.

Run rendered regression suites through
`powerprofilesctl launch --profile performance --` to reduce laptop power-state
variance. The complete gate is `pixi run pytest --renderer typhoon-local`
under that launcher, with an expected baseline of approximately 235 seconds;
failures or runtimes of 250 seconds or above require stopping and checking with
Anders.

### Lights

- Add Hydra support/creation tokens in `delegate/renderDelegate.cpp`.
- Pull authored data in `HdEmbree_Light::Sync()`; populate the runtime representation declared in `renderer/lights/light.h`.
- Add sampling and directional PDF logic in `renderer/lights/lightSamplers.*`.
- Add visible Embree geometry for finite camera-visible lights.
- Keep radiance evaluation, sampling PDF, normalization, shaping, IES, texture orientation, and linking consistent.
- Extend light-sampler tests and add a render fixture for visibility/synchronization. Use the external `typhoon-test-suite/usdlux` frame sweeps for LightAPI and sampler regressions.

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

hdEmbree's supported external render-settings interface is the USD
`TyphoonRenderSettingsAPI` schema. Direct Hydra delegate settings remain an
internal application-control path used by clients such as usdview's renderLab.
`HdEmbreeRenderer` and `HdEmbreeRenderSettings` are implementation details, not
a supported C++ API.

The MaterialXCpp GGX multiple-scattering and dielectric-layer throughput
policies remain process-wide globals. OIIO's primary texture system and its
cache-size attribute are also process-wide because it is created in shared
mode. `SetRenderSettings()` therefore reapplies all three values
unconditionally; applying them for one stopped renderer can affect another
renderer that is shading concurrently. Per-evaluation ownership is separate
work.

Update all relevant surfaces:

- default and typed runtime field in `renderer/renderSettings.h`, where
  renderer-consumed;
- token, descriptor, and label in `delegate/renderDelegate.*`;
- authored and generated files under `schema/`;
- bridge logic in `delegate/renderPass.cpp`;
- unified renderer application/state in `renderer/renderer.*` and behavior in
  the owning `aov/`, `camera/`, or `integrator/` file;
- user documentation in `README.md`;
- coverage in `testenv/testHdEmbreeRenderSettings.cpp`.

Keep `README.md`, this document, generated schema data, and tests synchronized with implementation.
