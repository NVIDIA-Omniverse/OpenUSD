//
// Copyright 2017 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_CONFIG_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_CONFIG_H

#include "pxr/pxr.h"
#include "pxr/base/tf/singleton.h"

#include <string>

PXR_NAMESPACE_OPEN_SCOPE

// NOTE: types here restricted to bool/int/string, as also used for
// TF_DEFINE_ENV_SETTING
constexpr int HdEmbreeDefaultSamplesToConvergence = 256;
constexpr int HdEmbreeDefaultTileSize = 8;
constexpr int HdEmbreeDefaultAmbientOcclusionSamples = 0;
constexpr bool HdEmbreeDefaultEnableAmbientOcclusion =
    HdEmbreeDefaultAmbientOcclusionSamples > 0;
constexpr bool HdEmbreeDefaultJitterCamera = true;
constexpr bool HdEmbreeDefaultEnableSceneColors = true;
constexpr int HdEmbreeDefaultCameraLightIntensity = 300;
constexpr int HdEmbreeDefaultRandomNumberSeed = -1;
constexpr bool HdEmbreeDefaultEnableLighting = true;
constexpr int HdEmbreeDefaultMaxBounces = 16;
constexpr int HdEmbreeDefaultMinBouncesBeforeRR = 2;
constexpr bool HdEmbreeDefaultDomeLightCameraVisibility = true;
constexpr bool HdEmbreeDefaultEnableAdaptiveSampling = true;
constexpr float HdEmbreeDefaultAdaptiveThreshold = 0.01f;
constexpr int HdEmbreeDefaultMinSamplesBeforeAdaptive = 64;
constexpr int HdEmbreeDefaultLightSamplesPerHit = 1;
constexpr bool HdEmbreeDefaultStratifyLightSamples = true;
constexpr bool HdEmbreeDefaultShowAdaptiveHeatmap = false;
constexpr float HdEmbreeDefaultFireflyClampThreshold = 20.0f;
constexpr bool HdEmbreeDefaultEnableCaustics = false;
constexpr float HdEmbreeDefaultCausticsClampThreshold = 5.0f;
constexpr bool HdEmbreeDefaultApproxTransparentShadows = true;
constexpr bool HdEmbreeDefaultDisableShadows = false;
constexpr bool HdEmbreeDefaultEnableGgxMicrofacetMultipleScattering = true;
constexpr char HdEmbreeDefaultMaterialRenderContext[] = "mtlx";
constexpr bool HdEmbreeDefaultUseAdobeOpenPBR = false;
constexpr char HdEmbreeDefaultDielectricLayerThroughputMode[] = "bsdl";
constexpr int HdEmbreeDefaultTextureCacheSizeMB = 16384;

/// \class HdEmbreeConfig
///
/// This class is a singleton, holding configuration parameters for HdEmbree.
/// Everything is provided with a default, but can be overridden using
/// environment variables before launching a hydra process.
///
/// Many of the parameters can be used to control quality/performance
/// tradeoffs, or to alter how HdEmbree takes advantage of parallelism.
///
/// At startup, this class will print config parameters if
/// *HDEMBREE_PRINT_CONFIGURATION* is true. Integer values greater than zero
/// are considered "true".
///
class HdEmbreeConfig {
public:

    /// \brief Return the configuration singleton.
    static const HdEmbreeConfig &GetInstance();

    /// How many samples do we need before a pixel is considered
    /// converged?
    ///
    /// Override with *HDEMBREE_SAMPLES_TO_CONVERGENCE*.
    unsigned int samplesToConvergence = HdEmbreeDefaultSamplesToConvergence;

    /// How many pixels are in an atomic unit of parallel work?
    /// A work item is a square of size [tileSize x tileSize] pixels.
    ///
    /// Override with *HDEMBREE_TILE_SIZE*.
    unsigned int tileSize = HdEmbreeDefaultTileSize;

    /// How many ambient occlusion rays should we generate per
    /// camera ray?
    ///
    /// Override with *HDEMBREE_AMBIENT_OCCLUSION_SAMPLES*.
    unsigned int ambientOcclusionSamples = HdEmbreeDefaultAmbientOcclusionSamples;

    /// Should the renderpass use ambient occlusion when scene lighting is
    /// disabled?
    ///
    /// Override with *HDEMBREE_ENABLE_AMBIENT_OCCLUSION*.
    bool enableAmbientOcclusion = HdEmbreeDefaultEnableAmbientOcclusion;

    /// Should the renderpass jitter camera rays for antialiasing?
    ///
    /// Override with *HDEMBREE_JITTER_CAMERA*. The case-insensitive strings
    /// "true", "yes", "on", and "1" are considered true; an empty value uses
    /// the default, and all other values are false.
    bool jitterCamera = HdEmbreeDefaultJitterCamera;

    /// Should the renderpass use the color primvar, or flat white colors?
    /// (Flat white shows off ambient occlusion better).
    ///
    /// Override with *HDEMBREE_ENABLE_SCENE_COLORS*.
    bool enableSceneColors = HdEmbreeDefaultEnableSceneColors;

    /// What should the intensity of the camera light be, specified as a
    /// percent of <1, 1, 1>.  For example, 300 would be <3, 3, 3>.
    ///
    /// Override with *HDEMBREE_CAMERA_LIGHT_INTENSITY*.
    float cameraLightIntensity = HdEmbreeDefaultCameraLightIntensity;

    /// OpenQMC frame seed. A value of -1 (the default) uses the current
    /// scene frame.
    ///
    /// Override with *HDEMBREE_RANDOM_NUMBER_SEED*.
    int randomNumberSeed = HdEmbreeDefaultRandomNumberSeed;

    /// Should the renderpass use scene lights (in particular, UsdLux-compliant
    /// area lights)?  Note that if scene lights and ambient occlusion are both
    /// enabled, the renderer will choose scene lights rather than ambient
    /// occlusion.
    ///
    /// Override with *HDEMBREE_ENABLE_LIGHTING*.
    bool enableLighting = HdEmbreeDefaultEnableLighting;

    /// Sampler sequence token name. If *HDEMBREE_SAMPLER_SEQUENCE* is empty,
    /// this defaults to openqmc_sobolbn.
    std::string samplerSequence;

    /// Whether dome lights are directly visible to camera rays.
    ///
    /// Override with *HDEMBREE_DOME_LIGHT_CAMERA_VISIBILITY*.
    bool domeLightCameraVisibility =
        HdEmbreeDefaultDomeLightCameraVisibility;

    /// Should we use adaptive sampling to skip converged pixels?
    ///
    /// Override with *HDEMBREE_ENABLE_ADAPTIVE_SAMPLING*.
    bool enableAdaptiveSampling = HdEmbreeDefaultEnableAdaptiveSampling;

    /// Variance threshold below which a pixel is considered converged.
    ///
    /// Override with *HDEMBREE_ADAPTIVE_THRESHOLD*.
    float adaptiveThreshold = HdEmbreeDefaultAdaptiveThreshold;

    /// Minimum samples per pixel before adaptive convergence checks begin.
    ///
    /// Override with *HDEMBREE_MIN_SAMPLES_BEFORE_ADAPTIVE*.
    int minSamplesBeforeAdaptive = HdEmbreeDefaultMinSamplesBeforeAdaptive;

    /// Number of light samples per hit point per light.
    ///
    /// Override with *HDEMBREE_LIGHT_SAMPLES_PER_HIT*.
    int lightSamplesPerHit = HdEmbreeDefaultLightSamplesPerHit;

    /// Should light samples be stratified across the light surface?
    ///
    /// Override with *HDEMBREE_STRATIFY_LIGHT_SAMPLES*.
    bool stratifyLightSamples = HdEmbreeDefaultStratifyLightSamples;

    /// Maximum number of indirect light bounces.
    ///
    /// Override with *HDEMBREE_MAX_BOUNCES*.
    int maxBounces = HdEmbreeDefaultMaxBounces;

    /// Minimum number of bounces before Russian Roulette termination.
    ///
    /// Override with *HDEMBREE_MIN_BOUNCES_BEFORE_RR*.
    int minBouncesBeforeRR = HdEmbreeDefaultMinBouncesBeforeRR;

    /// Visualize adaptive sampling convergence as a heatmap.
    ///
    /// Override with *HDEMBREE_SHOW_ADAPTIVE_HEATMAP*.
    bool showAdaptiveHeatmap = HdEmbreeDefaultShowAdaptiveHeatmap;

    /// Clamp threshold for bright non-caustic path contributions.
    ///
    /// Override with *HDEMBREE_FIREFLY_CLAMP_THRESHOLD*.
    float fireflyClampThreshold = HdEmbreeDefaultFireflyClampThreshold;

    /// Whether indirect caustic paths are enabled.
    ///
    /// Override with *HDEMBREE_ENABLE_CAUSTICS*.
    bool enableCaustics = HdEmbreeDefaultEnableCaustics;

    /// Clamp threshold for caustic path contributions.
    ///
    /// Override with *HDEMBREE_CAUSTICS_CLAMP_THRESHOLD*.
    float causticsClampThreshold = HdEmbreeDefaultCausticsClampThreshold;

    /// Whether transparent shadows are approximated.
    ///
    /// Override with *HDEMBREE_APPROX_TRANSPARENT_SHADOWS*.
    bool approxTransparentShadows =
        HdEmbreeDefaultApproxTransparentShadows;

    /// Whether shadow visibility rays are disabled.
    ///
    /// Override with *HDEMBREE_DISABLE_SHADOWS*.
    bool disableShadows = HdEmbreeDefaultDisableShadows;

    /// Whether rough dielectric GGX uses multiple scattering compensation.
    ///
    /// Override with *HDEMBREE_ENABLE_GGX_MICROFACET_MULTIPLE_SCATTERING*.
    bool enableGgxMicrofacetMultipleScattering =
        HdEmbreeDefaultEnableGgxMicrofacetMultipleScattering;

    /// Material render context priority.
    ///
    /// Override with *HDEMBREE_MATERIAL_RENDER_CONTEXT*.
    std::string materialRenderContext =
        HdEmbreeDefaultMaterialRenderContext;

    /// Whether the Adobe OpenPBR BSDF is used for OpenPBR Surface.
    ///
    /// Override with *HDEMBREE_USE_ADOBE_OPENPBR*.
    bool useAdobeOpenPBR = HdEmbreeDefaultUseAdobeOpenPBR;

    /// Throughput implementation for dielectric layer evaluation.
    ///
    /// Override with *HDEMBREE_DIELECTRIC_LAYER_THROUGHPUT_MODE*.
    std::string dielectricLayerThroughputMode =
        HdEmbreeDefaultDielectricLayerThroughputMode;

    /// Size (in MB) of the OpenImageIO texture/tile cache. Larger values keep
    /// more texture tiles resident, avoiding the cache thrashing and global
    /// cache-lock contention that otherwise serialize render threads on
    /// texture-heavy scenes. Exposed to applications as the "ty:textureCacheSize"
    /// render setting.
    ///
    /// Override with *HDEMBREE_TEXTURE_CACHE_SIZE*.
    int textureCacheSizeMB = HdEmbreeDefaultTextureCacheSizeMB;

private:
    // The constructor initializes the config variables with their
    // default or environment-provided override, and optionally prints
    // them.
    HdEmbreeConfig();
    ~HdEmbreeConfig() = default;

    HdEmbreeConfig(const HdEmbreeConfig&) = delete;
    HdEmbreeConfig& operator=(const HdEmbreeConfig&) = delete;

    friend class TfSingleton<HdEmbreeConfig>;
};

PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_IMAGING_PLUGIN_HD_EMBREE_CONFIG_H
