# Typhoon / hdEmbree

This is the user guide for Typhoon, the `hdEmbree` CPU path-tracing Hydra render
delegate. It is authoritative for supported workflows, authored settings and
AOVs, visible behavior, limitations, and examples. Internal design and file
ownership are documented in [`ARCHITECTURE.md`](ARCHITECTURE.md).

Typhoon uses Embree for ray traversal, MaterialX/OpenPBR for shading, USD Lux
lights, and OpenQMC sampling. It renders interactively in Hydra applications
and writes stage-authored `UsdRender` products through `usdrender`.

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

### Per-invocation attribute overrides

`usdrender -s` / `--set` authors repeatable attribute overrides into an
anonymous session layer without modifying the stage:

```sh
pixi run usdrender scene.usda -r Embree \
    -s "{settings}.ty:randomNumberSeed = 1" \
    -s "{settings}.ty:maxBounces = 8" \
    -s "/Camera.focalLength = 35"
```

The grammar is `[uniform|varying] [type] /Prim.attribute = USDA-value`.
Existing schema or authored attributes infer their type and variability;
explicit declarations must agree. A new custom attribute requires a type, for
example `-s "bool {settings}.domeLightCameraVisibility = false"`.
`{settings}` is case-insensitive and resolves once, before overrides, to the
RenderSettings prim selected by `--renderSettingsPrimPath`,
`--renderPassPrimPath`, or stage metadata. Every target prim must already be
defined and included by `--mask`; typos, type mismatches, malformed values,
undefined or masked-out prims, and native instance proxies fail before any
override is authored.

Values use USDA syntax: booleans are `true` / `false`, strings are quoted,
vectors use tuples, and arrays use brackets. Relative asset paths are
unanchored and resolve through the active resolver context with the current
working directory first; use an absolute asset path when resolution must be
unambiguous. With at least one `--set`, `--printOverrides` prints the generated
session layer and continues.

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
an error. Rejected surface graphs retain the display-color fallback, while
rejected displacement graphs leave the surface undisplaced.
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
Wire-only mode performs only the camera intersection and geometric wire
coverage evaluation. It skips material evaluation, lighting, ambient
occlusion, volumes, and secondary bounces, and draws opaque black lines over
the clear color regardless of the render-pass wire color.

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

| UI Name | Token | Type | Default |
|---------|-------|------|---------|
| Enable Scene Colors | `ty:enableSceneColors` | `bool` | `true` |
| Enable Scene Lighting | `ty:enableLighting` | `bool` | `true` |
| Enable Ambient Occlusion | `ty:enableAmbientOcclusion` | `bool` | `false` |
| Ambient Occlusion Samples | `ty:ambientOcclusionSamples` | `int` | `0` |
| Samples To Convergence | `ty:convergedSamplesPerPixel` | `int` | `256` |
| Random Number Seed | `ty:randomNumberSeed` | `int` | `-1` |
| Tile Size | `ty:tileSize` | `int` | `8` |
| Jitter Camera Rays | `ty:jitterCamera` | `bool` | `true` |
| Sampler Sequence | `ty:samplerSequence` | `token` | `openqmc_sobolbn` |
| Dome Light Camera Visibility | `domeLightCameraVisibility` | `bool` | `true` |
| Enable Exposure Compensation | `ty:enableExposureCompensation` | `bool` | `true` |
| Dynamic Subdivision Tessellation | `ty:dynamicSubdvTesselation` | `bool` | `false` |
| Enable Adaptive Sampling | `ty:enableAdaptiveSampling` | `bool` | `true` |
| Adaptive Threshold | `ty:adaptiveThreshold` | `float` | `0.01` |
| Min Samples Before Adaptive | `ty:minSamplesBeforeAdaptive` | `int` | `64` |
| Max Bounces | `ty:maxBounces` | `int` | `16` |
| Min Bounces Before Russian Roulette | `ty:minBouncesBeforeRR` | `int` | `2` |
| Light Samples Per Hit | `ty:lightSamplesPerHit` | `int` | `1` |
| Stratify Light Samples | `ty:stratifyLightSamples` | `bool` | `true` |
| Show Adaptive Heatmap | `ty:showAdaptiveHeatmap` | `bool` | `false` |
| Firefly Clamp Threshold | `ty:fireflyClampThreshold` | `float` | `20.0` |
| Enable Caustics | `ty:enableCaustics` | `bool` | `false` |
| Caustics Clamp Threshold | `ty:causticsClampThreshold` | `float` | `5.0` |
| Approximate Transparent Shadows | `ty:approxTransparentShadows` | `bool` | `true` |
| Disable Shadows | `ty:disableShadows` | `bool` | `false` |
| Enable GGX Microfacet Multiple Scattering | `ty:enableGgxMicrofacetMultipleScattering` | `bool` | `true` |
| Material Render Context | `ty:materialRenderContext` | `token` | `mtlx` |
| Use Adobe OpenPBR | `ty:useAdobeOpenPBR` | `bool` | `false` |
| Dielectric Layer Throughput Mode | `ty:dielectricLayerThroughputMode` | `token` | `bsdl` |
| Texture Cache Size (MB) | `ty:textureCacheSize` | `int` | `16384` |

## Setting Descriptions

### Enable Scene Lighting (`ty:enableLighting`)
When enabled, the renderer evaluates direct lighting from scene lights (UsdLux-compliant area lights) using MIS-based path tracing. When disabled, falls back to ambient occlusion if that is enabled.

### Enable Ambient Occlusion (`ty:enableAmbientOcclusion` / `ty:ambientOcclusionSamples`)
When scene lighting is disabled, ambient occlusion can be used instead. The
number of AO rays per camera ray is controlled by
`ty:ambientOcclusionSamples`. Set `ty:enableAmbientOcclusion` to `false` to
disable AO.

### Sampler Sequence (`ty:samplerSequence`)
Selects the per-pixel sampler implementation. Supported values are:

- `openqmc_sobol`
- `openqmc_sobolbn`
- `openqmc_pmj`
- `openqmc_pmjbn`
- `openqmc_lattice`
- `openqmc_latticebn`

If `ty:samplerSequence` is not authored, hdEmbree chooses
`openqmc_sobolbn`. Unknown sampler tokens fall back to that default and emit a
warning.

### Adaptive Sampling (`ty:enableAdaptiveSampling`, `ty:adaptiveThreshold`, `ty:minSamplesBeforeAdaptive`)
When enabled, per-pixel variance is tracked using Welford's online algorithm. Pixels whose variance metric falls below `ty:adaptiveThreshold` after at least `ty:minSamplesBeforeAdaptive` samples are marked as converged and skipped in subsequent passes. The default minimum sample count is intentionally conservative enough to avoid stopping too early on rare bright events such as sharp finite-light reflections, while still preserving useful speedups for scenes with non-uniform complexity.
Each RGB channel is tested independently with a mixed absolute/relative variance-of-the-mean limit:

```text
varOfMean[c] <= absFloor + adaptiveThreshold * mean[c]^2
```

The absolute floor keeps near-black pixels from being judged only by relative error; the relative term makes `adaptiveThreshold=0.01` roughly mean that a channel has reached a 10% standard error of its mean. This avoids the hue bias of the legacy luminance metric, where red or blue surfaces can require more samples only because their luminance coefficients are small.

### Path Tracing Depth (`ty:maxBounces`, `ty:minBouncesBeforeRR`)
`ty:maxBounces` controls the maximum number of indirect light bounces (default `16`). Higher values capture more global illumination but increase render time. An SSS closure (entry + random walk + exit) counts as a single bounce, matching a plain diffuse surface hit. `ty:minBouncesBeforeRR` sets the minimum number of bounces before Russian Roulette path termination kicks in (default `2`). Paths shorter than this threshold are never randomly terminated, ensuring basic indirect illumination is always captured.

### Light Samples Per Hit (`ty:lightSamplesPerHit`)
Number of shadow/light samples taken per hit point per light source. Higher values reduce noise in direct lighting at the cost of render time. Must be >= 1.

### Stratify Light Samples (`ty:stratifyLightSamples`)
When enabled, light samples are stratified across the light surface, providing more uniform coverage and reducing variance compared to purely random sampling.

### Caustics (`ty:enableCaustics`, `ty:causticsClampThreshold`)
When `ty:enableCaustics` is `true`, hdEmbree keeps indirect caustic paths but regularizes sharp lobes after the first non-specular bounce. Contributions on paths that have entered this caustic class are clamped by `ty:causticsClampThreshold`; set the threshold to `0` or below to disable this extra caustic-only clamp.

When `ty:enableCaustics` is `false` (default), hdEmbree treats specular or dielectric-boundary events after a diffuse-like surface, subsurface, or medium scatter as a caustic-class heuristic rather than a strict full-path caustics proof. It prunes those lobes from BSDF continuation sampling and direct-light BSDF evaluation when the native closure tree exposes them, then keeps a post-sample guard for backend-specific or geometry-dependent cases that can only be classified after sampling. Glossy dielectric traversal seen directly by the camera is not treated as a diffuse caustic ancestor merely because it has a finite PDF. This removes high-variance reflective/refractive caustics such as bright floor sparkles under glass objects, at the cost of omitting those caustic contributions.

### Transparent Shadows (`ty:approxTransparentShadows`)
Thin-walled transmissive surfaces always use straight RGB shadow attenuation because there is no thickness or refractive path to solve. For thick transmissive surfaces, when `ty:approxTransparentShadows` is `true` (default), shadow rays continue along the original straight line instead of being refracted. This thick-surface behavior is a biased direct-shadow approximation, not a Snell-refraction caustic solver. It keeps direct lighting usable under thick glass when caustic-class paths are suppressed.

The approximation applies RGB attenuation from surface opacity, dielectric transmission, transmission tint, and active interior-medium transmittance. Legacy and unsupported interfaces use Schlick Fresnel transmission. Near-smooth coupled dielectric interfaces use exact dielectric Fresnel through alpha 0.002 and transition smoothly to baked front/back directional-hemispherical transmission albedo through alpha 0.07, including their multiple-scattering transmission share when compensation is enabled, so roughness loss is applied independently at every crossed interface. The transmission LUT has an analytic Fresnel smooth row, quadratically spaced perceptual-roughness rows near zero, and uniform/visible-normal MIS so narrow near-smooth lobes and critical-angle behavior are resolved. For regular thick transmission with an interior medium, the surface tint is skipped so `transmission_color` is not applied once by the surface and again by Beer or Adobe OpenPBR volume transmittance.

Set `ty:approxTransparentShadows` to `false` to keep the conservative thick-surface behavior: current-medium exits are treated as scalar visibility, while thick transparent entry boundaries block the straight shadow ray. Thin-walled transmissive surfaces still use straight RGB attenuation.

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

### Adaptive heatmap
When this AOV is bound (and `ty:enableAdaptiveSampling` is active), it outputs a heatmap visualizing per-pixel sample counts. The color ramp maps the ratio `sampleCount / convergedSamplesPerPixel`: blue (few samples) -> cyan -> green -> yellow -> red (many samples). In usdview, select "adaptiveHeatmap" from the AOV dropdown to display it. The color AOV continues to render normally — the heatmap is written to its own separate buffer.

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
Reflective material lobes are raised toward the geometric surface when their
ideal reflection would otherwise point below it. The corrected lobe normal is
also used for Fresnel, reflection, refraction, TIR, evaluation, and PDF.
Generated reflection directions below the geometric or lobe surface and
transmission directions above either surface are discarded without resampling.
Direct evaluation and PDF do not apply that geometric rejection, matching
Cycles; strongly mapped grazing facets can therefore become darker than true
displacement. Smooth-base/material-normal agreement is evaluated per lobe, and
diffuse-family values receive continuous bump-terminator softening. Layered
materials project each lobe by its own corrected-normal cosine before combining
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
interface. Its BSDL compensation adds the missing energy as a cosine-distributed
multiple-scattering lobe split between reflection and refraction; it does not
brighten the existing glossy lobes. Rough glossy events sample a visible
microfacet and select their branch using the exact Fresnel response. Standard Surface keeps
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
