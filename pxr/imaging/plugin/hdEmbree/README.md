# HdEmbree Render Settings

hdEmbree uses OpenQMC for all renderer sampling. The `build_usd.py --embree`
path installs OpenQMC `v0.7.1` automatically, and the hdEmbree CMake target
requires `OpenQMC::OpenQMC`.

Renderer implementation is organized by responsibility under `renderer/`:
frame and pixel-sample orchestration remains in `renderer.cpp`, while camera
sampling, lit and unlit integrators, AOVs, lights, materials, geometry, and
sampling live in dedicated subdirectories. See `ARCHITECTURE.md` for the file map and render-flow guide.

The following settings can be configured via `renderSettings` (Hydra render delegate settings API) and/or environment variables. USD `RenderSettings` prim attributes use the `ty:` namespace. Precedence is:
built-in default < environment variable < USD `RenderSettings` prim < Hydra renderer setting UI.

| UI Name | Token | Type | Default | Environment Variable |
|---------|-------|------|---------|---------------------|
| Enable Scene Colors | `ty:enableSceneColors` | `bool` | `true` | `HDEMBREE_ENABLE_SCENE_COLORS` |
| Enable Scene Lighting | `ty:enableLighting` | `bool` | `true` | `HDEMBREE_ENABLE_LIGHTING` |
| Enable Ambient Occlusion | `ty:enableAmbientOcclusion` | `bool` | `false` | `HDEMBREE_ENABLE_AMBIENT_OCCLUSION` |
| Ambient Occlusion Samples | `ty:ambientOcclusionSamples` | `int` | `0` | `HDEMBREE_AMBIENT_OCCLUSION_SAMPLES` |
| Samples To Convergence | `ty:convergedSamplesPerPixel` | `int` | `256` | `HDEMBREE_SAMPLES_TO_CONVERGENCE` |
| Random Number Seed | `ty:randomNumberSeed` | `int` | `-1` | `HDEMBREE_RANDOM_NUMBER_SEED` |
| Sampler Sequence | `ty:samplerSequence` | `token` | `openqmc_sobolbn` | `HDEMBREE_SAMPLER_SEQUENCE` |
| Dome Light Camera Visibility | `ty:domeLightCameraVisibility` | `bool` | `true` | `HDEMBREE_DOME_LIGHT_CAMERA_VISIBILITY` |
| Enable Exposure Compensation | `ty:enableExposureCompensation` | `bool` | `true` | - |
| Enable Adaptive Sampling | `ty:enableAdaptiveSampling` | `bool` | `true` | `HDEMBREE_ENABLE_ADAPTIVE_SAMPLING` |
| Adaptive Threshold | `ty:adaptiveThreshold` | `float` | `0.01` | `HDEMBREE_ADAPTIVE_THRESHOLD` |
| Min Samples Before Adaptive | `ty:minSamplesBeforeAdaptive` | `int` | `64` | `HDEMBREE_MIN_SAMPLES_BEFORE_ADAPTIVE` |
| Max Bounces | `ty:maxBounces` | `int` | `16` | `HDEMBREE_MAX_BOUNCES` |
| Min Bounces Before Russian Roulette | `ty:minBouncesBeforeRR` | `int` | `2` | `HDEMBREE_MIN_BOUNCES_BEFORE_RR` |
| Light Samples Per Hit | `ty:lightSamplesPerHit` | `int` | `1` | `HDEMBREE_LIGHT_SAMPLES_PER_HIT` |
| Stratify Light Samples | `ty:stratifyLightSamples` | `bool` | `true` | `HDEMBREE_STRATIFY_LIGHT_SAMPLES` |
| Show Adaptive Heatmap | `ty:showAdaptiveHeatmap` | `bool` | `false` | `HDEMBREE_SHOW_ADAPTIVE_HEATMAP` |
| Firefly Clamp Threshold | `ty:fireflyClampThreshold` | `float` | `20.0` | `HDEMBREE_FIREFLY_CLAMP_THRESHOLD` |
| Enable Caustics | `ty:enableCaustics` | `bool` | `false` | `HDEMBREE_ENABLE_CAUSTICS` |
| Caustics Clamp Threshold | `ty:causticsClampThreshold` | `float` | `5.0` | `HDEMBREE_CAUSTICS_CLAMP_THRESHOLD` |
| Approximate Transparent Shadows | `ty:approxTransparentShadows` | `bool` | `true` | `HDEMBREE_APPROX_TRANSPARENT_SHADOWS` |
| Disable Shadows | `ty:disableShadows` | `bool` | `false` | `HDEMBREE_DISABLE_SHADOWS` |
| Enable GGX Microfacet Multiple Scattering | `ty:enableGgxMicrofacetMultipleScattering` | `bool` | `true` | `HDEMBREE_ENABLE_GGX_MICROFACET_MULTIPLE_SCATTERING` |
| Material Render Context | `ty:materialRenderContext` | `token` | `mtlx` | `HDEMBREE_MATERIAL_RENDER_CONTEXT` |
| Use Adobe OpenPBR | `ty:useAdobeOpenPBR` | `bool` | `false` | `HDEMBREE_USE_ADOBE_OPENPBR` |
| Dielectric Layer Throughput Mode | `ty:dielectricLayerThroughputMode` | `token` | `bsdl` | `HDEMBREE_DIELECTRIC_LAYER_THROUGHPUT_MODE` |

## Setting Descriptions

### Enable Scene Lighting (`ty:enableLighting`)
When enabled, the renderer evaluates direct lighting from scene lights (UsdLux-compliant area lights) using MIS-based path tracing. When disabled, falls back to ambient occlusion if that is enabled.

### Enable Ambient Occlusion (`ty:enableAmbientOcclusion` / `ty:ambientOcclusionSamples`)
When scene lighting is disabled, ambient occlusion can be used instead. The number of AO rays per camera ray is controlled by `ty:ambientOcclusionSamples`. For compatibility with existing launches, `HDEMBREE_AMBIENT_OCCLUSION_SAMPLES` greater than `0` also enables ambient occlusion at startup. Set both `ty:enableAmbientOcclusion` to `false` and `ty:ambientOcclusionSamples` to `0` to disable.

### Sampler Sequence (`ty:samplerSequence`)
Selects the per-pixel sampler implementation. Supported values are:

- `openqmc_sobol`
- `openqmc_sobolbn`
- `openqmc_pmj`
- `openqmc_pmjbn`
- `openqmc_lattice`
- `openqmc_latticebn`

If `ty:samplerSequence` is not authored and `HDEMBREE_SAMPLER_SEQUENCE` is empty,
hdEmbree chooses `openqmc_sobolbn`. Unknown sampler tokens fall back to that
default and emit a warning.

Internally, sampling is domain-aware rather than a single mutable 1D stream.
Each sampling decision, such as camera jitter, BSDF sampling, direct-light
samples, medium free-flight, and SSS random-walk bounces, derives a stable
sample domain from a fixed integer key. OpenQMC sequences map those domains to
`newDomain*()` and draw the requested dimensions with one `drawSample<N>()`
call.

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

The approximation applies RGB attenuation from surface opacity, dielectric transmission, transmission tint, and active interior-medium transmittance. Smooth and unsupported interfaces use the legacy Schlick Fresnel transmission. Coupled rough dielectric interfaces use baked front/back directional-hemispherical transmission albedo, including their multiple-scattering transmission share when compensation is enabled, so roughness loss is applied independently at every crossed interface. For regular thick transmission with an interior medium, the surface tint is skipped so `transmission_color` is not applied once by the surface and again by Beer or Adobe OpenPBR volume transmittance.

Set `ty:approxTransparentShadows` to `false` to keep the conservative thick-surface behavior: current-medium exits are treated as scalar visibility, while thick transparent entry boundaries block the straight shadow ray. Thin-walled transmissive surfaces still use straight RGB attenuation.

### Disable Shadows (`ty:disableShadows`)
When `ty:disableShadows` is `true`, shadow visibility rays return fully visible. Direct light sampling, emitted-light hits, dome evaluation, and camera visibility still run, so this removes occlusion along direct light paths without hiding lights or geometry from the camera. The default is `false`.

### Random Number Seed (`ty:randomNumberSeed`)
A value of `-1` (default) chooses a non-deterministic OpenQMC frame seed for each render. Any other value produces deterministic/repeatable sampler sequences.

## Custom AOVs

| AOV Name | Token | Format | Description |
|----------|-------|--------|-------------|
| Adaptive Heatmap | `adaptiveHeatmap` | `Float32Vec4` | Per-pixel sample count heatmap for adaptive sampling diagnostics |

### Adaptive Heatmap (`adaptiveHeatmap`)
When this AOV is bound (and `ty:enableAdaptiveSampling` is active), it outputs a heatmap visualizing per-pixel sample counts. The color ramp maps the ratio `sampleCount / convergedSamplesPerPixel`: blue (few samples) -> cyan -> green -> yellow -> red (many samples). In usdview, select "adaptiveHeatmap" from the AOV dropdown to display it. The color AOV continues to render normally — the heatmap is written to its own separate buffer.

## Material Interpretation Notes

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
