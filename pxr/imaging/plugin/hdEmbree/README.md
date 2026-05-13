# HdEmbree Render Settings

When hdEmbree is built with `PXR_ENABLE_OPENQMC_SUPPORT=ON`, the plugin also
links against OpenQMC. The `build_usd.py --embree` path installs OpenQMC
`v0.7.1` automatically and enables this CMake option for the final USD build.

The following settings can be configured via `renderSettings` (Hydra render delegate settings API) and/or environment variables. Precedence is:
built-in default < environment variable < USD `renderSettings` prim < Hydra renderer setting UI.

| UI Name | Token | Type | Default | Environment Variable |
|---------|-------|------|---------|---------------------|
| Enable Scene Colors | `enableSceneColors` | `bool` | `true` | `HDEMBREE_ENABLE_SCENE_COLORS` |
| Enable Scene Lighting | `enableLighting` | `bool` | `true` | `HDEMBREE_ENABLE_LIGHTING` |
| Enable Ambient Occlusion | `enableAmbientOcclusion` | `bool` | `false` | `HDEMBREE_ENABLE_AMBIENT_OCCLUSION` |
| Ambient Occlusion Samples | `ambientOcclusionSamples` | `int` | `0` | `HDEMBREE_AMBIENT_OCCLUSION_SAMPLES` |
| Samples To Convergence | `convergedSamplesPerPixel` | `int` | `256` | `HDEMBREE_SAMPLES_TO_CONVERGENCE` |
| Random Number Seed | `randomNumberSeed` | `int` | `-1` | `HDEMBREE_RANDOM_NUMBER_SEED` |
| Sampler Sequence | `samplerSequence` | `string` | effective default: `openqmc_sobolbn` with OpenQMC, otherwise `sobol` | `HDEMBREE_SAMPLER_SEQUENCE` |
| Dome Light Camera Visibility | `domeLightCameraVisibility` | `bool` | `true` | `HDEMBREE_DOME_LIGHT_CAMERA_VISIBILITY` |
| Enable Adaptive Sampling | `enableAdaptiveSampling` | `bool` | `true` | `HDEMBREE_ENABLE_ADAPTIVE_SAMPLING` |
| Adaptive Threshold | `adaptiveThreshold` | `float` | `0.01` | `HDEMBREE_ADAPTIVE_THRESHOLD` |
| Min Samples Before Adaptive | `minSamplesBeforeAdaptive` | `int` | `64` | `HDEMBREE_MIN_SAMPLES_BEFORE_ADAPTIVE` |
| Max Bounces | `maxBounces` | `int` | `16` | `HDEMBREE_MAX_BOUNCES` |
| Min Bounces Before Russian Roulette | `minBouncesBeforeRR` | `int` | `2` | `HDEMBREE_MIN_BOUNCES_BEFORE_RR` |
| Light Samples Per Hit | `lightSamplesPerHit` | `int` | `1` | `HDEMBREE_LIGHT_SAMPLES_PER_HIT` |
| Stratify Light Samples | `stratifyLightSamples` | `bool` | `true` | `HDEMBREE_STRATIFY_LIGHT_SAMPLES` |
| Show Adaptive Heatmap | `showAdaptiveHeatmap` | `bool` | `false` | `HDEMBREE_SHOW_ADAPTIVE_HEATMAP` |
| Firefly Clamp Threshold | `fireflyClampThreshold` | `float` | `20.0` | `HDEMBREE_FIREFLY_CLAMP_THRESHOLD` |
| Enable Caustics | `enableCaustics` | `bool` | `false` | `HDEMBREE_ENABLE_CAUSTICS` |
| Caustics Clamp Threshold | `causticsClampThreshold` | `float` | `5.0` | `HDEMBREE_CAUSTICS_CLAMP_THRESHOLD` |
| Approximate Transparent Shadows | `approxTransparentShadows` | `bool` | `true` | `HDEMBREE_APPROX_TRANSPARENT_SHADOWS` |
| Enable GGX Microfacet Multiple Scattering | `enableGgxMicrofacetMultipleScattering` | `bool` | `true` | `HDEMBREE_ENABLE_GGX_MICROFACET_MULTIPLE_SCATTERING` |
| Material Render Context | `materialRenderContext` | `string` | `mtlx` | `HDEMBREE_MATERIAL_RENDER_CONTEXT` |
| Use Adobe OpenPBR | `useAdobeOpenPBR` | `bool` | `false` | `HDEMBREE_USE_ADOBE_OPENPBR` |
| Dielectric Layer Throughput Mode | `dielectricLayerThroughputMode` | `string` | `bsdl` | `HDEMBREE_DIELECTRIC_LAYER_THROUGHPUT_MODE` |

## Setting Descriptions

### Enable Scene Lighting (`enableLighting`)
When enabled, the renderer evaluates direct lighting from scene lights (UsdLux-compliant area lights) using MIS-based path tracing. When disabled, falls back to ambient occlusion if that is enabled.

### Enable Ambient Occlusion (`enableAmbientOcclusion` / `ambientOcclusionSamples`)
When scene lighting is disabled, ambient occlusion can be used instead. The number of AO rays per camera ray is controlled by `ambientOcclusionSamples`. For compatibility with existing launches, `HDEMBREE_AMBIENT_OCCLUSION_SAMPLES` greater than `0` also enables ambient occlusion at startup. Set both `enableAmbientOcclusion` to `false` and `ambientOcclusionSamples` to `0` to disable.

### Sampler Sequence (`samplerSequence`)
Selects the per-pixel sampler implementation. Supported values are:

- `sobol`
- `random`
- `openqmc_sobol`
- `openqmc_sobolbn`
- `openqmc_pmj`
- `openqmc_pmjbn`
- `openqmc_lattice`
- `openqmc_latticebn`

If `samplerSequence` is not authored and `HDEMBREE_SAMPLER_SEQUENCE` is empty,
hdEmbree chooses `openqmc_sobolbn` when OpenQMC support is compiled in. Builds
without OpenQMC support choose `sobol`. If an `openqmc_*` sequence is requested
without OpenQMC support compiled in, hdEmbree falls back to `sobol` and emits a
warning.

Internally, sampling is domain-aware rather than a single mutable 1D stream.
Each sampling decision, such as camera jitter, BSDF sampling, direct-light
samples, medium free-flight, and SSS random-walk bounces, derives a stable
sample domain from a fixed integer key. OpenQMC-backed sequences map those
domains to `newDomain*()` and draw the requested dimensions with one
`drawSample<N>()` call; the built-in `sobol` and `random` sequences use the
same domain framework with deterministic seed mixing.

### Adaptive Sampling (`enableAdaptiveSampling`, `adaptiveThreshold`, `minSamplesBeforeAdaptive`)
When enabled, per-pixel variance is tracked using Welford's online algorithm. Pixels whose variance metric falls below `adaptiveThreshold` after at least `minSamplesBeforeAdaptive` samples are marked as converged and skipped in subsequent passes. The default minimum sample count is intentionally conservative enough to avoid stopping too early on rare bright events such as sharp finite-light reflections, while still preserving useful speedups for scenes with non-uniform complexity.
Each RGB channel is tested independently with a mixed absolute/relative variance-of-the-mean limit:

```text
varOfMean[c] <= absFloor + adaptiveThreshold * mean[c]^2
```

The absolute floor keeps near-black pixels from being judged only by relative error; the relative term makes `adaptiveThreshold=0.01` roughly mean that a channel has reached a 10% standard error of its mean. This avoids the hue bias of the legacy luminance metric, where red or blue surfaces can require more samples only because their luminance coefficients are small.

### Path Tracing Depth (`maxBounces`, `minBouncesBeforeRR`)
`maxBounces` controls the maximum number of indirect light bounces (default `16`). Higher values capture more global illumination but increase render time. An SSS closure (entry + random walk + exit) counts as a single bounce, matching a plain diffuse surface hit. `minBouncesBeforeRR` sets the minimum number of bounces before Russian Roulette path termination kicks in (default `2`). Paths shorter than this threshold are never randomly terminated, ensuring basic indirect illumination is always captured.

### Light Samples Per Hit (`lightSamplesPerHit`)
Number of shadow/light samples taken per hit point per light source. Higher values reduce noise in direct lighting at the cost of render time. Must be >= 1.

### Stratify Light Samples (`stratifyLightSamples`)
When enabled, light samples are stratified across the light surface, providing more uniform coverage and reducing variance compared to purely random sampling.

### Caustics (`enableCaustics`, `causticsClampThreshold`)
When `enableCaustics` is `true`, hdEmbree keeps indirect caustic paths but regularizes sharp lobes after the first non-specular bounce. Contributions on paths that have entered this caustic class are clamped by `causticsClampThreshold`; set the threshold to `0` or below to disable this extra caustic-only clamp.

When `enableCaustics` is `false` (default), hdEmbree treats specular or dielectric-boundary events after a diffuse-like surface, subsurface, or medium scatter as a caustic-class heuristic rather than a strict full-path caustics proof. It prunes those lobes from BSDF continuation sampling and direct-light BSDF evaluation when the native closure tree exposes them, then keeps a post-sample guard for backend-specific or geometry-dependent cases that can only be classified after sampling. Glossy dielectric traversal seen directly by the camera is not treated as a diffuse caustic ancestor merely because it has a finite PDF. This removes high-variance reflective/refractive caustics such as bright floor sparkles under glass objects, at the cost of omitting those caustic contributions.

### Transparent Shadows (`approxTransparentShadows`)
Thin-walled transmissive surfaces always use straight RGB shadow attenuation because there is no thickness or refractive path to solve. For thick transmissive surfaces, when `approxTransparentShadows` is `true` (default), shadow rays continue along the original straight line instead of being refracted. This thick-surface behavior is a biased direct-shadow approximation, not a Snell-refraction caustic solver. It keeps direct lighting usable under thick glass when caustic-class paths are suppressed.

The approximation applies RGB attenuation from surface opacity, dielectric Fresnel transmission, transmission tint, and active interior-medium transmittance. For regular thick transmission with an interior medium, the surface tint is skipped so `transmission_color` is not applied once by the surface and again by Beer or Adobe OpenPBR volume transmittance.

Set `approxTransparentShadows` to `false` to keep the conservative thick-surface behavior: current-medium exits are treated as scalar visibility, while thick transparent entry boundaries block the straight shadow ray. Thin-walled transmissive surfaces still use straight RGB attenuation.

### Random Number Seed (`randomNumberSeed`)
A value of `-1` (default) seeds the RNG non-deterministically. Any other value, combined with `PXR_WORK_THREAD_LIMIT=1`, produces deterministic/repeatable results.

## Custom AOVs

| AOV Name | Token | Format | Description |
|----------|-------|--------|-------------|
| Adaptive Heatmap | `adaptiveHeatmap` | `Float32Vec4` | Per-pixel sample count heatmap for adaptive sampling diagnostics |

### Adaptive Heatmap (`adaptiveHeatmap`)
When this AOV is bound (and `enableAdaptiveSampling` is active), it outputs a heatmap visualizing per-pixel sample counts. The color ramp maps the ratio `sampleCount / convergedSamplesPerPixel`: blue (few samples) -> cyan -> green -> yellow -> red (many samples). In usdview, select "adaptiveHeatmap" from the AOV dropdown to display it. The color AOV continues to render normally — the heatmap is written to its own separate buffer.

## Material Interpretation Notes

### Subsurface radius vs. Cycles / Blender Principled BSDF

hdEmbree's random-walk SSS treats the final per-channel radius (`subsurface_radius * subsurface_radius_scale` in OpenPBR, `Subsurface Radius * Subsurface Scale` in Principled BSDF) as the **physical mean free path** fed directly into the Chiang 2016 remap. No additional scaling is applied.

Cycles, in contrast, multiplies the incoming radius by `1 / (4π) ≈ 0.0796` inside `bssrdf_setup_radius` ([cycles/src/kernel/closure/bssrdf.h](https://projects.blender.org/blender/cycles/src/branch/main/src/kernel/closure/bssrdf.h)). Cycles' own comment describes this as a perceptual compatibility shim "so it gives similar looking result to older Cubic, Gaussian and Burley models" — it is not physically motivated, and hdEmbree intentionally does not replicate it because hdEmbree does not support those legacy BSSRDF models.

**Practical consequence**: a radius value that looks right in Cycles will scatter roughly `4π ≈ 12.6×` more densely in hdEmbree. To port material values:

- **Cycles radius → hdEmbree radius**: divide by `4π` (~12.6).
- **hdEmbree radius → Cycles radius**: multiply by `4π` (~12.6).

Everything else (the Chiang albedo → α polynomial remap, the random-walk step cap of 256, Dwivedi guided sampling, MIS channel selection) matches Cycles directly.
