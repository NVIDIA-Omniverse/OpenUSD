# HdEmbree Render Settings

When hdEmbree is built with `PXR_ENABLE_OPENQMC_SUPPORT=ON`, the plugin also
links against OpenQMC. The `build_usd.py --embree` path installs OpenQMC
`v0.7.1` automatically and enables this CMake option for the final USD build.

The following settings can be configured via `renderSettings` (Hydra render delegate settings API) and/or environment variables.

| UI Name | Token | Type | Default | Environment Variable |
|---------|-------|------|---------|---------------------|
| Enable Scene Colors | `enableSceneColors` | `bool` | `true` | `HDEMBREE_USE_FACE_COLORS` |
| Enable Scene Lighting | `enableLighting` | `bool` | `true` | `HDEMBREE_USE_LIGHTING` |
| Enable Ambient Occlusion | `enableAmbientOcclusion` | `bool` | `false` | `HDEMBREE_AMBIENT_OCCLUSION_SAMPLES` |
| Ambient Occlusion Samples | `ambientOcclusionSamples` | `int` | `0` | `HDEMBREE_AMBIENT_OCCLUSION_SAMPLES` |
| Samples To Convergence | `convergedSamplesPerPixel` | `int` | `256` | `HDEMBREE_SAMPLES_TO_CONVERGENCE` |
| Random Number Seed | `randomNumberSeed` | `int` | `-1` | `HDEMBREE_RANDOM_NUMBER_SEED` |
| Sampler Sequence | `samplerSequence` | `token` | effective default: `openqmc_sobolbn` with OpenQMC, otherwise `sobol` | `HDEMBREE_USE_SOBOL` (default selection only) |
| Enable Adaptive Sampling | `enableAdaptiveSampling` | `bool` | `true` | `HDEMBREE_ENABLE_ADAPTIVE_SAMPLING` |
| Adaptive Threshold | `adaptiveThreshold` | `float` | `0.01` | — |
| Min Samples Before Adaptive | `minSamplesBeforeAdaptive` | `int` | `16` | — |
| Max Bounces | `maxBounces` | `int` | `16` | — |
| Min Bounces Before Russian Roulette | `minBouncesBeforeRR` | `int` | `2` | — |
| Light Samples Per Hit | `lightSamplesPerHit` | `int` | `8` | `HDEMBREE_LIGHT_SAMPLES_PER_HIT` |
| Stratify Light Samples | `stratifyLightSamples` | `bool` | `true` | `HDEMBREE_STRATIFY_LIGHT_SAMPLES` |
| Use Per-Channel Variance | `usePerChannelVariance` | `bool` | `false` | — |
| Firefly Clamp Threshold | `fireflyClampThreshold` | `float` | `20.0` | — |
| Enable Caustics | `enableCaustics` | `bool` | `true` | — |
| Caustics Clamp Threshold | `causticsClampThreshold` | `float` | `5.0` | — |
| Approximate Transparent Shadows | `approxTransparentShadows` | `bool` | `true` | — |

In addition, the following Hydra built-in setting is forwarded:

| Token | Type | Default | Notes |
|-------|------|---------|-------|
| `domeLightCameraVisibility` | `bool` | `true` | From `HdRenderSettingsTokens`; controls whether dome lights are directly visible to camera rays |

## Setting Descriptions

### Enable Scene Lighting (`enableLighting`)
When enabled, the renderer evaluates direct lighting from scene lights (UsdLux-compliant area lights) using MIS-based path tracing. When disabled, falls back to ambient occlusion if that is enabled.

### Enable Ambient Occlusion (`enableAmbientOcclusion` / `ambientOcclusionSamples`)
When scene lighting is disabled, ambient occlusion can be used instead. The number of AO rays per camera ray is controlled by `ambientOcclusionSamples`. Set to `0` to disable.

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

If `samplerSequence` is not authored, hdEmbree chooses `openqmc_sobolbn`
when OpenQMC support is compiled in and `HDEMBREE_USE_SOBOL` remains enabled.
Builds without OpenQMC support choose `sobol`; setting `HDEMBREE_USE_SOBOL=0`
chooses `random`. If an `openqmc_*` sequence is requested without OpenQMC
support compiled in, hdEmbree falls back to `sobol` and emits a warning.

Internally, sampling is domain-aware rather than a single mutable 1D stream.
Each sampling decision, such as camera jitter, BSDF sampling, direct-light
samples, medium free-flight, and SSS random-walk bounces, derives a stable
sample domain from a fixed integer key. OpenQMC-backed sequences map those
domains to `newDomain*()` and draw the requested dimensions with one
`drawSample<N>()` call; the built-in `sobol` and `random` sequences use the
same domain framework with deterministic seed mixing.

### Adaptive Sampling (`enableAdaptiveSampling`, `adaptiveThreshold`, `minSamplesBeforeAdaptive`)
When enabled, per-pixel variance is tracked using Welford's online algorithm. Pixels whose variance falls below `adaptiveThreshold` after at least `minSamplesBeforeAdaptive` samples are marked as converged and skipped in subsequent passes. This can yield significant speedups (2-4x) for scenes with non-uniform complexity.

### Path Tracing Depth (`maxBounces`, `minBouncesBeforeRR`)
`maxBounces` controls the maximum number of indirect light bounces (default `16`). Higher values capture more global illumination but increase render time. An SSS closure (entry + random walk + exit) counts as a single bounce, matching a plain diffuse surface hit. `minBouncesBeforeRR` sets the minimum number of bounces before Russian Roulette path termination kicks in (default `2`). Paths shorter than this threshold are never randomly terminated, ensuring basic indirect illumination is always captured.

### Light Samples Per Hit (`lightSamplesPerHit`)
Number of shadow/light samples taken per hit point per light source. Higher values reduce noise in direct lighting at the cost of render time. Must be >= 1.

### Stratify Light Samples (`stratifyLightSamples`)
When enabled, light samples are stratified across the light surface, providing more uniform coverage and reducing variance compared to purely random sampling.

### Use Per-Channel Variance (`usePerChannelVariance`)
Controls the convergence metric for adaptive sampling. When `false` (default), luminance-based relative variance (`varOfMean / luminance²`) is used — channels are weighted by perceptual brightness, which can cause dark or red-heavy surfaces to require more samples. When `true`, per-channel relative variance (`varOfMean[c] / mean[c]`) is used instead (similar to pbrt-v4), treating R, G, B independently so that surface color does not bias convergence speed.

### Caustics (`enableCaustics`, `causticsClampThreshold`)
When `enableCaustics` is `true` (default), hdEmbree keeps indirect caustic paths but regularizes sharp lobes after the first non-specular bounce. Contributions on paths that have entered this caustic class are clamped by `causticsClampThreshold`; set the threshold to `0` or below to disable this extra caustic-only clamp.

When `enableCaustics` is `false`, hdEmbree suppresses caustic-class paths by terminating BSDF samples that become specular or cross a dielectric boundary after a non-specular bounce. This removes high-variance reflective/refractive caustics such as bright floor sparkles under glass objects, at the cost of omitting those caustic contributions.

### Transparent Shadows (`approxTransparentShadows`)
When `approxTransparentShadows` is `true` (default), shadow rays through transmissive surfaces continue along the original straight line instead of being refracted. This is a biased direct-shadow approximation, not a Snell-refraction caustic solver. It keeps direct lighting usable under thin-walled transparent cards and thick glass when caustic-class paths are suppressed.

The approximation applies RGB attenuation from surface opacity, dielectric Fresnel transmission, transmission tint, and active interior-medium transmittance. For regular thick transmission with an interior medium, the surface tint is skipped so `transmission_color` is not applied once by the surface and again by Beer or Adobe OpenPBR volume transmittance.

Set `approxTransparentShadows` to `false` to use the older conservative shadow behavior: thin-walled transmission and current-medium exits are treated as scalar visibility, while thick transparent entry boundaries block the straight shadow ray.

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
