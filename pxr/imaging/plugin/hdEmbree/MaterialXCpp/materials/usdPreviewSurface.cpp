//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "usdPreviewSurface.h"

#include <algorithm>
#include <cmath>

namespace mxcpp {

static const SlotName _kDiffuseColor("diffuseColor");
static const SlotName _kEmissiveColor("emissiveColor");
static const SlotName _kUseSpecularWorkflow("useSpecularWorkflow");
static const SlotName _kSpecularColor("specularColor");
static const SlotName _kMetallic("metallic");
static const SlotName _kRoughness("roughness");
static const SlotName _kClearcoat("clearcoat");
static const SlotName _kClearcoatRoughness("clearcoatRoughness");
static const SlotName _kOpacity("opacity");
static const SlotName _kOpacityMode("opacityMode");
static const SlotName _kOpacityThreshold("opacityThreshold");
static const SlotName _kIor("ior");
static const SlotName _kNormal("normal");
static const SlotName _kDisplacement("displacement");
static const SlotName _kOcclusion("occlusion");

namespace {

enum class _OpacityMode {
    Transparent,
    Presence
};

_OpacityMode
_GetOpacityMode(const ParamMap& params)
{
    const Value* const value = params.Find(_kOpacityMode);
    if (!value) {
        return _OpacityMode::Transparent;
    }

    if (ValueHolds<int>(*value)) {
        return ValueGet<int>(*value) == 1
            ? _OpacityMode::Presence
            : _OpacityMode::Transparent;
    }

    if (ValueHolds<std::string>(*value)) {
        return ValueGet<std::string>(*value) == "presence"
            ? _OpacityMode::Presence
            : _OpacityMode::Transparent;
    }

    return _OpacityMode::Transparent;
}

}  // namespace

SurfaceClosure
EvalUsdPreviewSurface(const ParamMap& params)
{
    SurfaceClosure c;

    c.baseColor = Get<Vec3f>(params, _kDiffuseColor,
                                      Vec3f(0.18f));
    c.emissiveColor = Get<Vec3f>(params, _kEmissiveColor,
                                          Vec3f(0.0f));

    int useSpecWf = Get<int>(params, _kUseSpecularWorkflow, 0);
    if (useSpecWf) {
        c.specularColor = Get<Vec3f>(
            params, _kSpecularColor, Vec3f(0.0f));
        c.metallic = 0.0f;
        c.specular = 1.0f;
    } else {
        c.metallic = Get<float>(params, _kMetallic, 0.0f);
        c.specularColor = Vec3f(1.0f);
        c.specular = 1.0f;
    }

    c.roughness = Get<float>(params, _kRoughness, 0.5f);

    c.coat = Get<float>(params, _kClearcoat, 0.0f);
    c.coatRoughness = Get<float>(
        params, _kClearcoatRoughness, 0.01f);
    c.coatIor = 1.5f;

    const float authoredOpacity = std::clamp(
        Get<float>(params, _kOpacity, 1.0f), 0.0f, 1.0f);
    const float opacityThreshold = Get<float>(
        params, _kOpacityThreshold, 0.0f);
    const bool hasCutoutThreshold = opacityThreshold > 0.0f;
    const float cutoutOpacity =
        authoredOpacity >= opacityThreshold ? 1.0f : 0.0f;
    const _OpacityMode opacityMode = _GetOpacityMode(params);

    if (hasCutoutThreshold) {
        c.opacity = cutoutOpacity;
        c.presence = cutoutOpacity;
        c.transmission = 0.0f;
    } else if (opacityMode == _OpacityMode::Presence) {
        c.opacity = authoredOpacity;
        c.presence = authoredOpacity;
        c.transmission = 0.0f;
    } else {
        c.opacity = authoredOpacity;
        c.presence = 1.0f;
        c.transmission = 1.0f - authoredOpacity;
    }

    c.specularIor = Get<float>(params, _kIor, 1.5f);

    c.normal = Get<Vec3f>(params, _kNormal,
                                   Vec3f(0.0f, 0.0f, 1.0f));

    float occlusion = Get<float>(params, _kOcclusion, 1.0f);
    c.baseColor = c.baseColor * occlusion;

    c.transmissionColor = Vec3f(1.0f);
    c.sheen = 0.0f;
    c.thinWalled = false;

    return c;
}

}  // namespace mxcpp
