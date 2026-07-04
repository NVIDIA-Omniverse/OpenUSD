//
// Copyright 2017 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/imaging/plugin/hdEmbree/config.h"

#include "pxr/imaging/plugin/hdEmbree/sampling.h"

#include "pxr/base/tf/envSetting.h"
#include "pxr/base/tf/instantiateSingleton.h"
#include "pxr/base/tf/stringUtils.h"

#include <algorithm>
#include <iostream>

PXR_NAMESPACE_OPEN_SCOPE

// Instantiate the config singleton.
TF_INSTANTIATE_SINGLETON(HdEmbreeConfig);

// Each configuration variable has an associated environment variable.
// The environment variable macro takes the variable name, a default value,
// and a description...
TF_DEFINE_ENV_SETTING(
    HDEMBREE_SAMPLES_TO_CONVERGENCE,
    HdEmbreeDefaultSamplesToConvergence,
    "Samples per pixel before we stop rendering (must be >= 1)");

TF_DEFINE_ENV_SETTING(
    HDEMBREE_TILE_SIZE,
    HdEmbreeDefaultTileSize,
    "Size (per axis) of threading work units (must be >= 1)");

TF_DEFINE_ENV_SETTING(
    HDEMBREE_AMBIENT_OCCLUSION_SAMPLES,
    HdEmbreeDefaultAmbientOcclusionSamples,
    "Ambient occlusion samples per camera ray (must be >= 0;"
    " a value of 0 disables ambient occlusion)");

TF_DEFINE_ENV_SETTING(
    HDEMBREE_ENABLE_AMBIENT_OCCLUSION,
    HdEmbreeDefaultEnableAmbientOcclusion,
    "Should HdEmbree use ambient occlusion when scene lighting is disabled?");

TF_DEFINE_ENV_SETTING(
    HDEMBREE_JITTER_CAMERA,
    HdEmbreeDefaultJitterCamera,
    "Should HdEmbree jitter camera rays while rendering?");

TF_DEFINE_ENV_SETTING(
    HDEMBREE_ENABLE_SCENE_COLORS,
    HdEmbreeDefaultEnableSceneColors,
    "Should HdEmbree use scene colors while rendering?");

TF_DEFINE_ENV_SETTING(
    HDEMBREE_CAMERA_LIGHT_INTENSITY,
    HdEmbreeDefaultCameraLightIntensity,
    "Intensity of the camera light, specified as a percentage of <1,1,1>.");

TF_DEFINE_ENV_SETTING(
    HDEMBREE_RANDOM_NUMBER_SEED,
    HdEmbreeDefaultRandomNumberSeed,
    "OpenQMC frame seed. A value of -1 (the default) chooses a"
        " non-deterministic seed for each render.");

TF_DEFINE_ENV_SETTING(
    HDEMBREE_ENABLE_LIGHTING,
    HdEmbreeDefaultEnableLighting,
    "Should HdEmbree use scene lights while rendering?");

TF_DEFINE_ENV_SETTING(
    HDEMBREE_SAMPLER_SEQUENCE,
    "",
    "Sampler sequence token name. If empty, defaults to openqmc_sobolbn.");

TF_DEFINE_ENV_SETTING(
    HDEMBREE_DOME_LIGHT_CAMERA_VISIBILITY,
    HdEmbreeDefaultDomeLightCameraVisibility,
    "Should dome lights be directly visible to camera rays?");

TF_DEFINE_ENV_SETTING(
    HDEMBREE_ENABLE_ADAPTIVE_SAMPLING,
    HdEmbreeDefaultEnableAdaptiveSampling,
    "Should HdEmbree use adaptive sampling to skip converged pixels?");

TF_DEFINE_ENV_SETTING(
    HDEMBREE_ADAPTIVE_THRESHOLD,
    "0.01",
    "Variance threshold below which a pixel is considered converged.");

TF_DEFINE_ENV_SETTING(
    HDEMBREE_MIN_SAMPLES_BEFORE_ADAPTIVE,
    HdEmbreeDefaultMinSamplesBeforeAdaptive,
    "Minimum samples per pixel before adaptive convergence checks begin"
    " (must be >= 1).");

TF_DEFINE_ENV_SETTING(
    HDEMBREE_MAX_BOUNCES,
    HdEmbreeDefaultMaxBounces,
    "Maximum number of indirect light bounces (must be >= 0).");

TF_DEFINE_ENV_SETTING(
    HDEMBREE_MIN_BOUNCES_BEFORE_RR,
    HdEmbreeDefaultMinBouncesBeforeRR,
    "Minimum number of bounces before Russian Roulette termination"
    " (must be >= 0).");

TF_DEFINE_ENV_SETTING(
    HDEMBREE_LIGHT_SAMPLES_PER_HIT,
    HdEmbreeDefaultLightSamplesPerHit,
    "Number of light samples per hit point per light (must be >= 1)");

TF_DEFINE_ENV_SETTING(
    HDEMBREE_STRATIFY_LIGHT_SAMPLES,
    HdEmbreeDefaultStratifyLightSamples,
    "Should light samples be stratified across the light surface?");

TF_DEFINE_ENV_SETTING(
    HDEMBREE_SHOW_ADAPTIVE_HEATMAP,
    HdEmbreeDefaultShowAdaptiveHeatmap,
    "Should HdEmbree visualize adaptive sampling convergence as a heatmap?");

TF_DEFINE_ENV_SETTING(
    HDEMBREE_FIREFLY_CLAMP_THRESHOLD,
    "20.0",
    "Clamp threshold for bright non-caustic path contributions.");

TF_DEFINE_ENV_SETTING(
    HDEMBREE_ENABLE_CAUSTICS,
    HdEmbreeDefaultEnableCaustics,
    "Should HdEmbree enable indirect caustic paths?");

TF_DEFINE_ENV_SETTING(
    HDEMBREE_CAUSTICS_CLAMP_THRESHOLD,
    "5.0",
    "Clamp threshold for caustic path contributions.");

TF_DEFINE_ENV_SETTING(
    HDEMBREE_APPROX_TRANSPARENT_SHADOWS,
    HdEmbreeDefaultApproxTransparentShadows,
    "Should HdEmbree approximate transparent shadows?");

TF_DEFINE_ENV_SETTING(
    HDEMBREE_DISABLE_SHADOWS,
    HdEmbreeDefaultDisableShadows,
    "Should HdEmbree skip shadow visibility rays?");

TF_DEFINE_ENV_SETTING(
    HDEMBREE_ENABLE_GGX_MICROFACET_MULTIPLE_SCATTERING,
    HdEmbreeDefaultEnableGgxMicrofacetMultipleScattering,
    "Should rough dielectric GGX use multiple scattering compensation?");

TF_DEFINE_ENV_SETTING(
    HDEMBREE_MATERIAL_RENDER_CONTEXT,
    HdEmbreeDefaultMaterialRenderContext,
    "Material render context priority.");

TF_DEFINE_ENV_SETTING(
    HDEMBREE_USE_ADOBE_OPENPBR,
    HdEmbreeDefaultUseAdobeOpenPBR,
    "Should HdEmbree use the Adobe OpenPBR BSDF for OpenPBR Surface?");

TF_DEFINE_ENV_SETTING(
    HDEMBREE_DIELECTRIC_LAYER_THROUGHPUT_MODE,
    HdEmbreeDefaultDielectricLayerThroughputMode,
    "Throughput implementation for dielectric layer evaluation.");

TF_DEFINE_ENV_SETTING(HDEMBREE_PRINT_CONFIGURATION,
    false,
    "Should HdEmbree print configuration on startup?");

static float
_GetFloatEnvSetting(TfEnvSetting<std::string>& setting)
{
    return static_cast<float>(TfStringToDouble(TfGetEnvSetting(setting)));
}

HdEmbreeConfig::HdEmbreeConfig()
{
    // Read in values from the environment, clamping them to valid ranges.
    samplesToConvergence = std::max(1,
            TfGetEnvSetting(HDEMBREE_SAMPLES_TO_CONVERGENCE));
    tileSize = std::max(1,
            TfGetEnvSetting(HDEMBREE_TILE_SIZE));
    ambientOcclusionSamples = std::max(0,
            TfGetEnvSetting(HDEMBREE_AMBIENT_OCCLUSION_SAMPLES));
    enableAmbientOcclusion =
        TfGetEnvSetting(HDEMBREE_ENABLE_AMBIENT_OCCLUSION) ||
        ambientOcclusionSamples > 0;
    jitterCamera = (TfGetEnvSetting(HDEMBREE_JITTER_CAMERA));
    enableSceneColors = (TfGetEnvSetting(HDEMBREE_ENABLE_SCENE_COLORS));
    cameraLightIntensity = (std::max(100,
            TfGetEnvSetting(HDEMBREE_CAMERA_LIGHT_INTENSITY)) / 100.0f);
    randomNumberSeed = TfGetEnvSetting(HDEMBREE_RANDOM_NUMBER_SEED);
    enableLighting = (TfGetEnvSetting(HDEMBREE_ENABLE_LIGHTING));
    samplerSequence = TfGetEnvSetting(HDEMBREE_SAMPLER_SEQUENCE);
    if (samplerSequence.empty()) {
        samplerSequence = HdEmbreeGetSamplerSequenceToken(
            HdEmbreeGetDefaultSamplerSequence()).GetString();
    }
    domeLightCameraVisibility =
        TfGetEnvSetting(HDEMBREE_DOME_LIGHT_CAMERA_VISIBILITY);
    enableAdaptiveSampling = (TfGetEnvSetting(HDEMBREE_ENABLE_ADAPTIVE_SAMPLING));
    adaptiveThreshold = _GetFloatEnvSetting(HDEMBREE_ADAPTIVE_THRESHOLD);
    minSamplesBeforeAdaptive = std::max(1,
            TfGetEnvSetting(HDEMBREE_MIN_SAMPLES_BEFORE_ADAPTIVE));
    maxBounces = std::max(0,
            TfGetEnvSetting(HDEMBREE_MAX_BOUNCES));
    minBouncesBeforeRR = std::max(0,
            TfGetEnvSetting(HDEMBREE_MIN_BOUNCES_BEFORE_RR));
    lightSamplesPerHit = std::max(1,
            TfGetEnvSetting(HDEMBREE_LIGHT_SAMPLES_PER_HIT));
    stratifyLightSamples = (TfGetEnvSetting(HDEMBREE_STRATIFY_LIGHT_SAMPLES));
    showAdaptiveHeatmap = TfGetEnvSetting(HDEMBREE_SHOW_ADAPTIVE_HEATMAP);
    fireflyClampThreshold =
        _GetFloatEnvSetting(HDEMBREE_FIREFLY_CLAMP_THRESHOLD);
    enableCaustics = TfGetEnvSetting(HDEMBREE_ENABLE_CAUSTICS);
    causticsClampThreshold =
        _GetFloatEnvSetting(HDEMBREE_CAUSTICS_CLAMP_THRESHOLD);
    approxTransparentShadows =
        TfGetEnvSetting(HDEMBREE_APPROX_TRANSPARENT_SHADOWS);
    disableShadows = TfGetEnvSetting(HDEMBREE_DISABLE_SHADOWS);
    enableGgxMicrofacetMultipleScattering =
        TfGetEnvSetting(
            HDEMBREE_ENABLE_GGX_MICROFACET_MULTIPLE_SCATTERING);
    materialRenderContext = TfGetEnvSetting(HDEMBREE_MATERIAL_RENDER_CONTEXT);
    useAdobeOpenPBR = TfGetEnvSetting(HDEMBREE_USE_ADOBE_OPENPBR);
    dielectricLayerThroughputMode =
        TfGetEnvSetting(HDEMBREE_DIELECTRIC_LAYER_THROUGHPUT_MODE);

    if (TfGetEnvSetting(HDEMBREE_PRINT_CONFIGURATION)) {
        std::cout
            << "HdEmbree Configuration: \n"
            << "  samplesToConvergence       = "
            <<    samplesToConvergence    << "\n"
            << "  tileSize                   = "
            <<    tileSize                << "\n"
            << "  ambientOcclusionSamples    = "
            <<    ambientOcclusionSamples << "\n"
            << "  enableAmbientOcclusion     = "
            <<    enableAmbientOcclusion  << "\n"
            << "  jitterCamera               = "
            <<    jitterCamera            << "\n"
            << "  enableSceneColors          = "
            <<    enableSceneColors       << "\n"
            << "  cameraLightIntensity      = "
            <<    cameraLightIntensity    << "\n"
            << "  randomNumberSeed          = "
            <<    randomNumberSeed        << "\n"
            << "  enableLighting            = "
            <<    enableLighting          << "\n"
            << "  samplerSequence           = "
            <<    samplerSequence          << "\n"
            << "  domeLightCameraVisibility = "
            <<    domeLightCameraVisibility << "\n"
            << "  enableAdaptiveSampling    = "
            <<    enableAdaptiveSampling   << "\n"
            << "  adaptiveThreshold         = "
            <<    adaptiveThreshold        << "\n"
            << "  minSamplesBeforeAdaptive  = "
            <<    minSamplesBeforeAdaptive << "\n"
            << "  maxBounces                = "
            <<    maxBounces               << "\n"
            << "  minBouncesBeforeRR        = "
            <<    minBouncesBeforeRR       << "\n"
            << "  lightSamplesPerHit       = "
            <<    lightSamplesPerHit       << "\n"
            << "  stratifyLightSamples     = "
            <<    stratifyLightSamples     << "\n"
            << "  showAdaptiveHeatmap       = "
            <<    showAdaptiveHeatmap      << "\n"
            << "  fireflyClampThreshold     = "
            <<    fireflyClampThreshold    << "\n"
            << "  enableCaustics            = "
            <<    enableCaustics           << "\n"
            << "  causticsClampThreshold    = "
            <<    causticsClampThreshold   << "\n"
            << "  approxTransparentShadows  = "
            <<    approxTransparentShadows << "\n"
            << "  disableShadows            = "
            <<    disableShadows           << "\n"
            << "  enableGgxMicrofacetMultipleScattering = "
            <<    enableGgxMicrofacetMultipleScattering << "\n"
            << "  materialRenderContext     = "
            <<    materialRenderContext    << "\n"
            << "  useAdobeOpenPBR           = "
            <<    useAdobeOpenPBR          << "\n"
            << "  dielectricLayerThroughputMode = "
            <<    dielectricLayerThroughputMode << "\n"
            ;
    }
}

/*static*/
const HdEmbreeConfig&
HdEmbreeConfig::GetInstance()
{
    return TfSingleton<HdEmbreeConfig>::GetInstance();
}

PXR_NAMESPACE_CLOSE_SCOPE
