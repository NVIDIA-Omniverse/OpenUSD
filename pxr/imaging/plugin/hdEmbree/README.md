# Typhoon / hdEmbree

Typhoon is a reference path tracer built into OpenUSD. It is intended to be a readable, community-developed, shared reference for how to implement standard USD features, such as UsdLux lighting, and UsdShade-based MaterialX materials.

Typhoon is NOT intended to be a production renderer, nor a replacement for a viewport renderer such as Storm. It is not heavily optimized, but should be fast enough that image regression suites using it can run in a reasonable amount of time.

# Getting Started

After cloning as normal, the quickest and easiest way to build is with [Pixi](https://pixi.prefix.dev/latest/installation/):
```bash
# from repo root, NOT pxr/imaging/plugin/hdEmbree
pixi run configure
pixi run build
```

Pixi will handle all dependencies and install the built OpenUSD distribution in its default environment. Once the build has completed, run:

```bash
# from repo root, NOT pxr/imaging/plugin/hdEmbree
pixi run usdview /path/to/scene.usd
```

to run `usdview` with Typhoon already selected as the default renderer. In `usdview` you can use the `RenderLab` plugin to edit certain scene properties at runtime.

## usdrender

This branch also adds a new executable called `usdrender`. This, as its name suggests, renders a USD layer, writing `RenderProduct`s connected to the selected `RenderSettings` to disk.  

### Per-invocation attribute overrides

`usdrender -s` / `--set` authors repeatable attribute overrides into an
anonymous session layer without modifying the stage:

```sh
pixi run usdrender scene.usda -r Embree \
    -s "{settings}.ty:randomNumberSeed = 1" \
    -s "{settings}.ty:maxBounces = 8" \
    -s "/Camera.focalLength = 35"
```

# Navigating the code
Internal design and file ownership are documented in [`ARCHITECTURE.md`](ARCHITECTURE.md).

## Limitations

- Render-pass collection include/exclude paths and render tags are unsupported;
  every pass traces the whole synchronized scene.
- Multiple simultaneous hdEmbree render passes are unsupported. They share one
  delegate renderer and overwrite common outputs instead of compositing.

## Supported lighting

Typhoon supports USD Lux cylinder, disk, distant, dome, rect, and sphere
lights, including common LightAPI controls, color temperature, normalization,
shaping, IES profiles, dome/rect textures, and light/shadow linking. Finite
light shapes can appear to the camera when `visibleInPrimaryRay` is enabled.
`domeLightCameraVisibility` independently controls dome backgrounds.

The following settings can be configured through the Hydra render-delegate
settings API. USD `RenderSettings` prim attributes use the `ty:` namespace,
except the generic `domeLightCameraVisibility` setting. For `usdrender`,
precedence is built-in default < USD `RenderSettings` prim < command-line
`--set`. Interactive applications can place direct Hydra renderer settings,
including UI changes, above authored USD values.

## Render-product output

hdEmbree writes active stage-authored `RenderProduct` files only for offline
clients that set Hydra's `enableInteractive` render setting to `false`. An
unset value is treated as interactive, so viewers such as usdview render the
products into their viewport without writing their `productName` paths.
`usdrender` explicitly selects offline mode. hdEmbree writes products only
after the frame both passes renderer setup and converges; a failed setup leaves
the expected product absent so `usdrender` reports an error.

## Subdivision complexity and MaterialX displacement

At `low` complexity, hdEmbree triangulates the authored subdivision control
cage without evaluating subdivision displacement. Higher complexities use
screen-space adaptive subdivision:

| Complexity | Geometry / target edge length |
|------------|-------------------------------|
| `veryhigh` | subdivision, 0.5 pixel |
| `high` | subdivision, 1 pixel |
| `medium` | subdivision, 4 pixels |
| `low` | triangulated control cage |

The target is measured after instance transforms. Displaced quadrilateral faces
may refine to at most twice the camera-derived level to follow screen-space
curvature without opening shared edges. See
[scene synchronization](ARCHITECTURE.md#scene-synchronization) for the
authoritative level, displacement, and commit invariants.

Subdivision requires an attached `HdCamera`. By default, the first camera and
valid viewport determine tessellation for the render pass lifetime. Set
`ty:dynamicSubdvTesselation = true` to recompute after camera or viewport
changes; expensive displacement graphs can make those updates costly.
Instance, topology, and display-style changes still update affected geometry.
Meshes with `subdivisionScheme = "none"` remain triangles at every complexity.

Vertex, varying, uniform, and indexed face-varying primvars retain their Hydra
interpolation and seam behavior. Embree cannot distinguish OpenSubdiv's
`cornersOnly`, `cornersPlus1`, and `cornersPlus2` face-varying rules, so those
three produce the same closest-supported corner behavior.

A material can connect `ND_displacement_float` to its `displacement` terminal.
Displacement applies only at medium or higher complexity to a subdivision
scheme such as `catmullClark`. The graph receives object-space position and
normal, `st`, numeric geomprops at supported Hydra interpolations, and constant
string/filename geomprops. Embree limits interpolated subdivision attributes
to float-based scalar/vector types. Point-instancer transforms and per-instance
primvars cannot vary a shared prototype's displacement.

MaterialX `geompropvalue` names must be constant strings. Connected, absent, or
non-string names produce one recoverable diagnostic and make only that node
evaluate its authored default.

Material terminals are validated when the material is synchronized. A
malformed surface/displacement graph emits one warning identifying the
material, terminal, and first failing node; unauthored optional terminals
remain silent. Volume-only materials create a transparent participating-medium
boundary, including where all evaluated medium coefficients are zero.
Mixing surface-shader closures preserves that boundary identity only when
every input with nonzero weight is itself a volume boundary.
Displacement-only materials use display-color fallback shading on the displaced
surface. A material with none of these usable terminals warns about its missing
surface. A malformed displacement-only material emits its actionable
displacement warning without also treating the intentionally absent surface as
an error. Missing or rejected surface graphs use authored `displayColor` for
the diffuse fallback, or neutral gray `(0.5, 0.5, 0.5)` with opacity 1 when it
is not authored. Rejected displacement graphs leave the surface undisplaced.
Unexpected displacement backend failures produce a runtime error and leave the
affected generated vertex undisplaced.

## Hydra wireframe display

hdEmbree honors the standard mesh `wireOnSurf`, `refinedWireOnSurf`, `wire`,
and `refinedWire` reprs selected by clients such as usdview. Wire-on-surface is
the recommended mode: it composites a screen-space line over the final shaded
camera result, using `HdRenderPassState`'s wire color, alpha, and line width. An
unset zero wire color follows Storm's convention and dims the shaded surface
along edges. Changing usdview's render mode resynchronizes existing meshes, so
switching between smooth, wire, and wire-on-surface takes effect immediately.
For color output, wire-only mode performs only the camera intersection and
geometric wire coverage evaluation. It skips material evaluation, lighting,
volumes, and secondary bounces, and draws opaque black lines over the clear
color regardless of the render-pass wire color. Independently binding the
`ambocc` diagnostic still requests its visibility ray.

At low complexity, the overlay shows the rendered control-cage triangles,
including diagonals. At higher complexity it follows the final adaptive,
displaced diced grid. Subpixel cells appear as filtered dense coverage; zoom or
increase output resolution to resolve them. Line width stays in framebuffer
pixels and is stable during progressive rendering.

Embree does not expose its private transition-fan triangle IDs, so stitch
diagonals where opposing subdivision levels differ are approximate. Wire-only
mode shows the nearest surface and does not reveal rear edges. Use
wire-on-surface for final-render diagnostics. The reconstruction and repr-sync
invariants are documented under
[scene synchronization](ARCHITECTURE.md#scene-synchronization).

# Settings

| UI Name | Token | Type | Default |
|---------|-------|------|---------|
| Rendering Color Space | `renderingColorSpace` | `token` | `lin_rec709_scene` |
| Samples To Convergence | `ty:convergedSamplesPerPixel` | `int` | `256` |
| Random Number Seed | `ty:randomNumberSeed` | `int` | `-1` |
| Tile Size | `ty:tileSize` | `int` | `8` |
| Dome Light Camera Visibility | `domeLightCameraVisibility` | `bool` | `true` |
| Enable Exposure Compensation | `enableExposureCompensation` | `bool` | `true` |
| Dynamic Subdivision Tessellation | `ty:dynamicSubdvTesselation` | `bool` | `false` |
| Adaptive Threshold | `ty:adaptiveThreshold` | `float` | `0.01` |
| Min Samples Before Adaptive | `ty:minSamplesBeforeAdaptive` | `int` | `64` |
| Max Bounces | `ty:maxBounces` | `int` | `16` |
| Min Bounces Before Russian Roulette | `ty:minBouncesBeforeRR` | `int` | `2` |
| Light Samples Per Hit | `ty:lightSamplesPerHit` | `int` | `1` |
| Firefly Clamp Threshold | `ty:fireflyClampThreshold` | `float` | `20.0` |
| Enable Caustics | `ty:enableCaustics` | `bool` | `false` |
| Caustics Clamp Threshold | `ty:causticsClampThreshold` | `float` | `5.0` |
| Disable Shadows | `ty:disableShadows` | `bool` | `false` |
| Material Render Context | `ty:materialRenderContext` | `token` | `mtlx` |
| Use Adobe OpenPBR | `ty:useAdobeOpenPBR` | `bool` | `false` |
| Dielectric Layer Throughput Mode | `ty:dielectricLayerThroughputMode` | `token` | `bsdl` |
| Texture Cache Size (MB) | `ty:textureCacheSize` | `int` | `16384` |

## Rendering Behavior and Setting Descriptions

### Rendering Color Space (`renderingColorSpace`)
Selects the renderer working color space. RenderLab exposes
`lin_rec709_scene`, `lin_ap1_scene`, and `data`; `data` bypasses source color
transforms. This is the standard `UsdRenderSettings` attribute rather than a
Typhoon-namespaced setting.

### Hydra Lighting State

Lighting presentation follows `HdRenderPassState::GetLightingEnabled()` rather
than a Typhoon render setting. Enabled color passes always use path tracing and
evaluate only the lights supplied by Hydra. If scene-light pruning leaves no
light or emissive source, non-emissive surfaces remain black; hdEmbree does not
add a headlight or ambient fallback.

When Hydra disables lighting, color passes display authored `displayColor`
directly, or neutral gray `(0.5, 0.5, 0.5)` when it is unauthored. This unlit
presentation does not evaluate material closures, surface orientation, scene
lights, emission transport, indirect bounces, or ambient occlusion. Request the
independent `ambocc` AOV when that diagnostic is needed.

### Adaptive Sampling (`ty:adaptiveThreshold`, `ty:minSamplesBeforeAdaptive`)
Adaptive sampling is always active during progressive rendering. Per-pixel
variance is tracked using Welford's online algorithm. Pixels whose variance
metric falls below `ty:adaptiveThreshold` after at least
`ty:minSamplesBeforeAdaptive` samples are marked as converged and skipped in
subsequent passes. `ty:convergedSamplesPerPixel` remains the maximum sample
count for pixels that do not converge early. The default minimum sample count
is intentionally conservative enough to avoid stopping too early on rare
bright events such as sharp finite-light reflections, while still preserving
useful speedups for scenes with non-uniform complexity.
Each RGB channel is tested independently with a mixed absolute/relative variance-of-the-mean limit:

```text
varOfMean[c] <= absFloor + adaptiveThreshold * mean[c]^2
```

The absolute floor keeps near-black pixels from being judged only by relative error; the relative term makes `adaptiveThreshold=0.01` roughly mean that a channel has reached a 10% standard error of its mean. This avoids the hue bias of the legacy luminance metric, where red or blue surfaces can require more samples only because their luminance coefficients are small.

### Path Tracing Depth (`ty:maxBounces`, `ty:minBouncesBeforeRR`)
`ty:maxBounces` controls the maximum number of indirect light bounces (default `16`). Higher values capture more global illumination but increase render time. An SSS closure (entry + random walk + exit) counts as a single bounce, matching a plain diffuse surface hit. `ty:minBouncesBeforeRR` sets the minimum number of bounces before Russian Roulette path termination kicks in (default `2`). Paths shorter than this threshold are never randomly terminated, ensuring basic indirect illumination is always captured.

### Light Samples Per Hit (`ty:lightSamplesPerHit`)
Number of shadow/light samples taken per hit point per light source. Higher values reduce noise in direct lighting at the cost of render time. Samples are always stratified across the light surface. Must be >= 1.

### Caustics and Transparent Shadows (`ty:enableCaustics`, `ty:causticsClampThreshold`)
When `ty:enableCaustics` is `true`, hdEmbree keeps indirect caustic paths but regularizes sharp lobes after the first non-specular bounce. Contributions on paths that have entered this caustic class are clamped by `ty:causticsClampThreshold`; set the threshold to `0` or below to disable this extra caustic-only clamp. Thick transmissive entry boundaries block straight shadow rays in this mode instead of receiving a separate transparent-shadow approximation.

When `ty:enableCaustics` is `false` (default), hdEmbree treats specular or dielectric-boundary events after a diffuse-like surface, subsurface, or medium scatter as a caustic-class heuristic rather than a strict full-path caustics proof. It prunes those lobes from BSDF continuation sampling and direct-light BSDF evaluation when the native closure tree exposes them, then keeps a post-sample guard for backend-specific or geometry-dependent cases that can only be classified after sampling. Glossy dielectric traversal seen directly by the camera is not treated as a diffuse caustic ancestor merely because it has a finite PDF. This removes high-variance reflective/refractive caustics such as bright floor sparkles under glass objects, at the cost of omitting those caustic contributions. To keep direct lighting usable under thick glass in this mode, shadow rays use a biased straight-through transparent-shadow approximation instead of solving a Snell-refraction caustic path.

Thin-walled transmissive surfaces always use straight RGB shadow attenuation in both caustics modes because there is no thickness or refractive path to solve. For thick transmissive surfaces, the straight-through approximation is enabled only when caustics are disabled. Direct-light shadow rays originating at a coupled thick dielectric still use conservative visibility when they intersect that same object, because straight traversal cannot participate consistently in MIS with that dielectric's refracted BSDF path. Other objects on the segment use approximate traversal, as does coupled thick glass intersected by a shadow ray from a diffuse surface or medium. When caustics are enabled, current-medium exits retain conservative visibility, and thick entry boundaries do not continue along the unrefracted shadow ray.

The approximation applies RGB attenuation from surface opacity, dielectric transmission, transmission tint, and active interior-medium transmittance. Legacy and unsupported interfaces use Schlick Fresnel transmission. Near-smooth coupled dielectric interfaces use exact dielectric Fresnel through alpha 0.002 and transition smoothly to baked front/back directional-hemispherical transmission albedo through alpha 0.07, including their multiple-scattering transmission share when compensation is enabled, so roughness loss is applied independently at every crossed interface. The transmission LUT has an analytic Fresnel smooth row and is generated from the same bounded BSDL sampler used for rough coupled transport. For regular thick transmission with an interior medium, the surface tint is skipped so `transmission_color` is not applied once by the surface and again by Beer or Adobe OpenPBR volume transmittance.

### Disable Shadows (`ty:disableShadows`)
When `ty:disableShadows` is `true`, shadow visibility rays return fully visible. Direct light sampling, emitted-light hits, dome evaluation, and camera visibility still run, so this removes occlusion along direct light paths without hiding lights or geometry from the camera. The default is `false`.

### Random Number Seed (`ty:randomNumberSeed`)
A value of `-1` (default) derives the OpenQMC seed from the scene frame. Any
other value selects an explicit deterministic/repeatable sampler sequence. Use
`usdrender -s "{settings}.ty:randomNumberSeed = 1"` for fixed-seed comparisons
without editing the stage.

## AOVs

| AOV | Required format |
|-----|-----------------|
| `color` | `Float32Vec3/4`, `UNorm8Vec3/4`, or `SNorm8Vec3/4` |
| `depth`, `cameraDepth` | `Float32` |
| `primId`, `instanceId`, `elementId` | `Int32` |
| `normal`, `Neye` | `Float32Vec3` |
| `primvars:<name>` | `Float32Vec3` |
| `adaptiveHeatmap` | `Float32Vec4` |
| `ambocc` | `Float32Vec3` |

### Adaptive heatmap
When this AOV is bound, it outputs a heatmap visualizing per-pixel sample
counts. The color ramp maps the ratio
`sampleCount / convergedSamplesPerPixel`: blue (few samples) -> cyan -> green
-> yellow -> red (many samples). If it is the only bound AOV, hdEmbree still
evaluates scene radiance so convergence reflects the same signal as beauty
rendering. In usdview, choose `Renderer > Hydra AOVs > Other...` and enter
`adaptiveHeatmap` to display it. The heatmap is computed only while that AOV is
bound and never replaces the color AOV.

### Ambient occlusion
When `ambocc` is bound, hdEmbree traces one cosine-weighted ambient-visibility
ray for every progressive pixel sample that hits ordinary geometry. The
unoccluded fraction is stored as the same linear value in all three RGB
channels: zero is fully occluded and one is fully open. Misses issue no AO ray
and contribute zero so camera or scene restarts cannot retain stale resolved
pixels.

`ty:convergedSamplesPerPixel` is both the progressive sample limit and the
maximum number of AO rays per pixel; there is no separate enable flag or AO
sample-count setting. Adaptive sampling may stop a converged pixel earlier.
When `ambocc` is the only bound AOV, its scalar visibility samples drive the
convergence statistics without evaluating material closures or path lighting.
When color is also bound, AO remains independent and never changes the beauty
sample. In usdview, choose `Renderer > Hydra AOVs > Other...` and enter
`ambocc` to display it.

Before rendering, hdEmbree requires at least one hdEmbree-owned AOV buffer,
supported AOV formats, matching non-zero buffer dimensions, and a non-empty
data window contained by every buffer. Invalid setup emits a specific warning,
performs no sampling or buffer mapping, and terminates that render invocation.
Legacy viewport clients that provide no AOV bindings receive anonymous color
and depth buffers; render-pass convergence follows those internal buffers.
Valid camera framing without AOV bindings remains unsupported and settles as a
failed frame, so it cannot write a stale `RenderProduct`.

## Material Interpretation Notes

### Normal orientation and double-sided shading

hdEmbree keeps the authored-outside facet normal separate from smooth,
displaced, and material-mapped shading normals. Boundary crossings, media, and
ray offsets use only the facet normal. Materials evaluate normal and bump maps
in a view-independent exterior frame. Results are validated against the
exterior smooth/displaced base normal; invalid or inverted results fall back to
that base. The complete result is then faced to the incident side, preserving
one physical relief field across the entry and exit sides of a dielectric.
Glossy lobes and subsurface entry normals are raised toward the geometric
surface when their ideal reflection would otherwise point below it, matching
Cycles for both delta and finite-roughness microfacet closures. A corrected
lobe uses the same normal for Fresnel, reflection,
refraction, TIR, evaluation, and PDF.
Generated reflection directions below the geometric or lobe surface and
transmission directions above either surface are discarded without resampling.
Direct evaluation and PDF do not apply that geometric rejection, matching
Cycles; strongly mapped grazing facets can therefore become darker than true
displacement. Smooth-base/material-normal agreement is evaluated per lobe, and
diffuse-family values receive continuous bump-terminator softening. Layered
materials project each lobe by its own exact-normal cosine before combining
the response, avoiding grazing energy spikes from one graph-normal cosine. On
coarse smooth triangles, direct-light shadow origins are lifted
toward the interpolated surface near a facet terminator using the triangle's
actual positions; authored texture-coordinate scale does not affect that lift.
Thick dielectric side
selection happens before material evaluation; thin-walled transmission never
changes persistent medium state.

### Rough and thin-walled transmission

Rough-transmission transport is selected by the material model. OpenPBR and
metalness-workflow UsdPreviewSurface use the compensated coupled dielectric
interface. Rough glossy events use BSDL's bounded reflection-VNDF sampler and
select reflection or refraction using exact Fresnel. BSDL's directional
missing-energy table scales both evaluations together; it does not add a
separate compensation lobe to sampling. Direct-light evaluation is truncated
to the same bounded-normal support, preventing rough glass from gaining energy
under next-event estimation. Standard Surface keeps
its established separate reflection layer and transmission lobe for
compatibility, including its layered Fresnel attenuation and support for
`transmission_extra_roughness`.

Standard Surface `thin_walled` transmission uses a separate IOR-1 dielectric
lobe, so transmitted rays remain straight through even when the authored
specular IOR or roughness is nonzero. OpenPBR `geometry_thin_walled` instead
uses its coupled thin-sheet interface: the authored IOR still controls Fresnel,
and nonzero roughness may blur transmission as defined by that model.

### Subsurface radius vs. Cycles / Blender Principled BSDF

hdEmbree's random-walk SSS treats the final per-channel radius (`subsurface_radius * subsurface_radius_scale` in OpenPBR, `Subsurface Radius * Subsurface Scale` in Principled BSDF) as the **physical mean free path** fed directly into the Chiang 2016 remap. No additional scaling is applied.

Cycles, in contrast, multiplies the incoming radius by `1 / (4π) ≈ 0.0796` inside `bssrdf_setup_radius` ([cycles/src/kernel/closure/bssrdf.h](https://projects.blender.org/blender/cycles/src/branch/main/src/kernel/closure/bssrdf.h)). Cycles' own comment describes this as a perceptual compatibility shim "so it gives similar looking result to older Cubic, Gaussian and Burley models" — it is not physically motivated, and hdEmbree intentionally does not replicate it because hdEmbree does not support those legacy BSSRDF models.

**Practical consequence**: a radius value that looks right in Cycles will scatter roughly `4π ≈ 12.6×` more densely in hdEmbree. To port material values:

- **Cycles radius → hdEmbree radius**: divide by `4π` (~12.6).
- **hdEmbree radius → Cycles radius**: multiply by `4π` (~12.6).

Everything else (the Chiang albedo → α polynomial remap, the random-walk step cap of 256, Dwivedi guided sampling, MIS channel selection) matches Cycles directly.
