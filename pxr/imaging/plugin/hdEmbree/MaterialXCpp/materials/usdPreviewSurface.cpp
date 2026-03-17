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
static const SlotName _kOpacityThreshold("opacityThreshold");
static const SlotName _kIor("ior");
static const SlotName _kNormal("normal");
static const SlotName _kDisplacement("displacement");
static const SlotName _kOcclusion("occlusion");

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

    c.opacity = Get<float>(params, _kOpacity, 1.0f);
    float opacityThreshold = Get<float>(
        params, _kOpacityThreshold, 0.0f);
    if (opacityThreshold > 0.0f && c.opacity < opacityThreshold) {
        c.opacity = 0.0f;
    }

    c.specularIor = Get<float>(params, _kIor, 1.5f);

    c.normal = Get<Vec3f>(params, _kNormal,
                                   Vec3f(0.0f, 0.0f, 1.0f));

    float occlusion = Get<float>(params, _kOcclusion, 1.0f);
    c.baseColor = c.baseColor * occlusion;

    c.transmission = 0.0f;
    c.transmissionColor = Vec3f(1.0f);
    c.sheen = 0.0f;
    c.thinWalled = false;

    return c;
}

} // namespace mxcpp
