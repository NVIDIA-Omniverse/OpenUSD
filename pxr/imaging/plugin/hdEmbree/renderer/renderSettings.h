//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_RENDER_SETTINGS_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_RENDER_SETTINGS_H

#include <renderer/sampling/sampling.h>

#include "pxr/base/tf/token.h"
#include "pxr/pxr.h"

PXR_NAMESPACE_OPEN_SCOPE

/// Rough dielectric top-layer throughput estimator.
enum class HdEmbreeDielectricLayerThroughputMode {
    Bsdl,
    MaterialXGlsl
};

/// Return the throughput mode named by \p token.
///
/// Unknown tokens return the Bsdl default. Callers that accept authored tokens
/// must round-trip the result through
/// HdEmbreeGetDielectricLayerThroughputModeToken to diagnose invalid input.
inline HdEmbreeDielectricLayerThroughputMode
HdEmbreeGetDielectricLayerThroughputModeFromToken(TfToken const& token)
{
    static const TfToken materialXGlsl("materialxGlsl", TfToken::Immortal);
    return token == materialXGlsl
        ? HdEmbreeDielectricLayerThroughputMode::MaterialXGlsl
        : HdEmbreeDielectricLayerThroughputMode::Bsdl;
}

/// Return the canonical token for \p mode.
inline TfToken
HdEmbreeGetDielectricLayerThroughputModeToken(
    HdEmbreeDielectricLayerThroughputMode mode)
{
    static const TfToken bsdl("bsdl", TfToken::Immortal);
    static const TfToken materialXGlsl("materialxGlsl", TfToken::Immortal);
    return mode == HdEmbreeDielectricLayerThroughputMode::MaterialXGlsl
        ? materialXGlsl
        : bsdl;
}

/// Renderer-consumed render settings after Hydra policy resolution.
///
/// The render pass converts authored tokens and resolves cross-setting policy.
/// HdEmbreeRenderer normalizes value invariants before storing this value.
struct HdEmbreeRenderSettings {
    // Progressive and sampling policy.
    int samplesToConvergence = 256;
    int randomNumberSeed = -1;
    int tileSize = 8;
    bool jitterCamera = true;
    HdEmbreeSamplerSequence samplerSequence =
        HdEmbreeSamplerSequence::OpenQMCSobolBN;
    bool enableAdaptiveSampling = true;
    float adaptiveThreshold = 0.01f;
    int minSamplesBeforeAdaptive = 64;
    bool showAdaptiveHeatmap = false;

    // Lighting and ambient occlusion.
    bool enableLighting = true;
    bool domeLightCameraVisibility = true;
    bool enableSceneColors = true;
    int ambientOcclusionSamples = 0;
    int lightSamplesPerHit = 1;
    bool stratifyLightSamples = true;

    // Path depth and contribution policy.
    int maxBounces = 16;
    int minBouncesBeforeRR = 2;
    float fireflyClampThreshold = 20.0f;
    bool enableCaustics = false;
    float causticsClampThreshold = 5.0f;

    // Visibility policy.
    bool approxTransparentShadows = true;
    bool disableShadows = false;

    // Material and texture policy.
    bool enableGgxMicrofacetMultipleScattering = true;
    HdEmbreeDielectricLayerThroughputMode dielectricLayerThroughputMode =
        HdEmbreeDielectricLayerThroughputMode::Bsdl;
    bool useAdobeOpenPBR = false;
    int textureCacheSizeMB = 16384;
};

// Render-pass-owned defaults not represented in renderer state.
constexpr bool HdEmbreeDefaultEnableAmbientOcclusion = false;
constexpr bool HdEmbreeDefaultEnableExposureCompensation = true;
constexpr bool HdEmbreeDefaultDynamicSubdvTesselation = false;
constexpr char HdEmbreeDefaultMaterialRenderContext[] = "mtlx";

// Linear multiplier on the fallback headlight. Not a percentage.
constexpr float HdEmbreeCameraLightIntensity = 3.0f;

PXR_NAMESPACE_CLOSE_SCOPE

#endif  // PXR_IMAGING_PLUGIN_HD_EMBREE_RENDER_SETTINGS_H
