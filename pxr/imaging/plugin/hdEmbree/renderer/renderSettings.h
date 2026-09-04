//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_RENDER_SETTINGS_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_RENDER_SETTINGS_H

#include "pxr/base/tf/token.h"
#include "pxr/pxr.h"

PXR_NAMESPACE_OPEN_SCOPE
namespace ty {

constexpr float DefaultMinimumCurveWidth = 0.001f;

/// Rough dielectric top-layer throughput estimator.
enum class DielectricLayerThroughputMode {
    Bsdl,
    MaterialXGlsl
};

/// Return the throughput mode named by \p token.
///
/// Unknown tokens return the Bsdl default. Callers that accept authored tokens
/// must round-trip the result through
/// GetDielectricLayerThroughputModeToken to diagnose invalid input.
inline DielectricLayerThroughputMode
GetDielectricLayerThroughputModeFromToken(TfToken const& token)
{
    static const TfToken materialXGlsl("materialxGlsl", TfToken::Immortal);
    return token == materialXGlsl
        ? DielectricLayerThroughputMode::MaterialXGlsl
        : DielectricLayerThroughputMode::Bsdl;
}

/// Return the canonical token for \p mode.
inline TfToken
GetDielectricLayerThroughputModeToken(
    DielectricLayerThroughputMode mode)
{
    static const TfToken bsdl("bsdl", TfToken::Immortal);
    static const TfToken materialXGlsl("materialxGlsl", TfToken::Immortal);
    return mode == DielectricLayerThroughputMode::MaterialXGlsl
        ? materialXGlsl
        : bsdl;
}

/// Renderer-consumed render settings after Hydra policy resolution.
///
/// The render pass converts authored tokens and resolves cross-setting policy.
/// Renderer normalizes value invariants before storing this value.
struct RenderSettings {
    // Progressive and sampling policy.
    int samplesToConvergence = 256;
    int randomNumberSeed = -1;
    int tileSize = 8;
    float adaptiveThreshold = 0.01f;
    int minSamplesBeforeAdaptive = 64;

    // Lighting.
    bool domeLightCameraVisibility = true;
    int lightSamplesPerHit = 1;

    // Path depth and contribution policy.
    int maxBounces = 16;
    int minBouncesBeforeRR = 2;
    float fireflyClampThreshold = 20.0f;
    bool enableCaustics = false;
    float causticsClampThreshold = 5.0f;

    // Visibility policy.
    bool disableShadows = false;

    // Geometry policy. The render delegate normalizes this object-space
    // diameter before the value reaches the renderer or curve Rprims.
    float minimumCurveWidth = DefaultMinimumCurveWidth;

    // Material and texture policy.
    DielectricLayerThroughputMode dielectricLayerThroughputMode =
        DielectricLayerThroughputMode::Bsdl;
    bool useAdobeOpenPBR = false;
    int textureCacheSizeMB = 16384;
};

// Render-pass-owned defaults not represented in renderer state.
constexpr bool DefaultDynamicSubdvTesselation = false;
constexpr char DefaultMaterialRenderContext[] = "mtlx";

} // namespace ty
PXR_NAMESPACE_CLOSE_SCOPE

#endif  // PXR_IMAGING_PLUGIN_HD_EMBREE_RENDER_SETTINGS_H
