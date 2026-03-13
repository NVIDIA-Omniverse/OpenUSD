//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/imaging/plugin/hdEmbree/MaterialXCpp/materials/usdPreviewSurface.h"

#include "pxr/base/tf/staticTokens.h"

#include <algorithm>
#include <cmath>

PXR_NAMESPACE_OPEN_SCOPE
namespace mxcpp {

TF_DEFINE_PRIVATE_TOKENS(_tokens,
    (diffuseColor)
    (emissiveColor)
    (useSpecularWorkflow)
    (specularColor)
    (metallic)
    (roughness)
    (clearcoat)
    (clearcoatRoughness)
    (opacity)
    (opacityThreshold)
    (ior)
    (normal)
    (displacement)
    (occlusion)
);

SurfaceClosure
EvalUsdPreviewSurface(const ParamMap& params)
{
    SurfaceClosure c;

    c.baseColor = Get<GfVec3f>(params, _tokens->diffuseColor,
                                      GfVec3f(0.18f));
    c.emissiveColor = Get<GfVec3f>(params, _tokens->emissiveColor,
                                          GfVec3f(0.0f));

    int useSpecWf = Get<int>(params, _tokens->useSpecularWorkflow, 0);
    if (useSpecWf) {
        c.specularColor = Get<GfVec3f>(
            params, _tokens->specularColor, GfVec3f(0.0f));
        c.metallic = 0.0f;
        c.specular = 1.0f;
    } else {
        c.metallic = Get<float>(params, _tokens->metallic, 0.0f);
        c.specularColor = GfVec3f(1.0f);
        c.specular = 1.0f;
    }

    c.roughness = Get<float>(params, _tokens->roughness, 0.5f);

    c.coat = Get<float>(params, _tokens->clearcoat, 0.0f);
    c.coatRoughness = Get<float>(
        params, _tokens->clearcoatRoughness, 0.01f);
    c.coatIor = 1.5f;

    c.opacity = Get<float>(params, _tokens->opacity, 1.0f);
    float opacityThreshold = Get<float>(
        params, _tokens->opacityThreshold, 0.0f);
    if (opacityThreshold > 0.0f && c.opacity < opacityThreshold) {
        c.opacity = 0.0f;
    }

    c.specularIor = Get<float>(params, _tokens->ior, 1.5f);

    c.normal = Get<GfVec3f>(params, _tokens->normal,
                                   GfVec3f(0.0f, 0.0f, 1.0f));

    float occlusion = Get<float>(params, _tokens->occlusion, 1.0f);
    c.baseColor = c.baseColor * occlusion;

    c.transmission = 0.0f;
    c.transmissionColor = GfVec3f(1.0f);
    c.sheen = 0.0f;
    c.thinWalled = false;

    return c;
}

} // namespace mxcpp
PXR_NAMESPACE_CLOSE_SCOPE
