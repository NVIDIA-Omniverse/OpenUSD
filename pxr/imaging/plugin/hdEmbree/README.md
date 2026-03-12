# HdEmbree Render Settings

The following settings can be configured via `renderSettings` (Hydra render delegate settings API) and/or environment variables.

| UI Name | Token | Type | Default | Environment Variable |
|---------|-------|------|---------|---------------------|
| Enable Scene Colors | `enableSceneColors` | `bool` | `true` | `HDEMBREE_USE_FACE_COLORS` |
| Enable Scene Lighting | `enableLighting` | `bool` | `true` | `HDEMBREE_USE_LIGHTING` |
| Enable Ambient Occlusion | `enableAmbientOcclusion` | `bool` | `false` | `HDEMBREE_AMBIENT_OCCLUSION_SAMPLES` |
| Ambient Occlusion Samples | `ambientOcclusionSamples` | `int` | `0` | `HDEMBREE_AMBIENT_OCCLUSION_SAMPLES` |
| Samples To Convergence | `convergedSamplesPerPixel` | `int` | `100` | `HDEMBREE_SAMPLES_TO_CONVERGENCE` |
| Random Number Seed | `randomNumberSeed` | `int` | `-1` | `HDEMBREE_RANDOM_NUMBER_SEED` |
| Use Sobol Sampler | `useSobol` | `bool` | `true` | `HDEMBREE_USE_SOBOL` |
| Enable Adaptive Sampling | `enableAdaptiveSampling` | `bool` | `true` | `HDEMBREE_ENABLE_ADAPTIVE_SAMPLING` |
| Adaptive Threshold | `adaptiveThreshold` | `float` | `0.01` | — |
| Min Samples Before Adaptive | `minSamplesBeforeAdaptive` | `int` | `16` | — |
| Max Bounces | `maxBounces` | `int` | `4` | — |
| Min Bounces Before Russian Roulette | `minBouncesBeforeRR` | `int` | `2` | — |

In addition, the following Hydra built-in setting is forwarded:

| Token | Type | Default | Notes |
|-------|------|---------|-------|
| `domeLightCameraVisibility` | `bool` | `true` | From `HdRenderSettingsTokens`; controls whether dome lights are directly visible to camera rays |

## Setting Descriptions

### Enable Scene Lighting (`enableLighting`)
When enabled, the renderer evaluates direct lighting from scene lights (UsdLux-compliant area lights) using MIS-based path tracing. When disabled, falls back to ambient occlusion if that is enabled.

### Enable Ambient Occlusion (`enableAmbientOcclusion` / `ambientOcclusionSamples`)
When scene lighting is disabled, ambient occlusion can be used instead. The number of AO rays per camera ray is controlled by `ambientOcclusionSamples`. Set to `0` to disable.

### Use Sobol Sampler (`useSobol`)
Controls the sampling strategy. When `true` (default), a Sobol quasi-random sequence with FastOwen scrambling is used, providing better convergence properties. When `false`, a hash-based pseudo-random sampler is used instead.

### Adaptive Sampling (`enableAdaptiveSampling`, `adaptiveThreshold`, `minSamplesBeforeAdaptive`)
When enabled, per-pixel variance is tracked using Welford's online algorithm. Pixels whose variance falls below `adaptiveThreshold` after at least `minSamplesBeforeAdaptive` samples are marked as converged and skipped in subsequent passes. This can yield significant speedups (2-4x) for scenes with non-uniform complexity.

### Path Tracing Depth (`maxBounces`, `minBouncesBeforeRR`)
`maxBounces` controls the maximum number of indirect light bounces (default `4`). Higher values capture more global illumination but increase render time. `minBouncesBeforeRR` sets the minimum number of bounces before Russian Roulette path termination kicks in (default `2`). Paths shorter than this threshold are never randomly terminated, ensuring basic indirect illumination is always captured.

### Random Number Seed (`randomNumberSeed`)
A value of `-1` (default) seeds the RNG non-deterministically. Any other value, combined with `PXR_WORK_THREAD_LIMIT=1`, produces deterministic/repeatable results.
