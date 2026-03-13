//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/imaging/plugin/hdEmbree/MaterialXCpp/materials/standardSurface.h"

#include "pxr/base/tf/staticTokens.h"

PXR_NAMESPACE_OPEN_SCOPE
namespace mxcpp {

TF_DEFINE_PRIVATE_TOKENS(_tokens,
    (base)
    (base_color)
    (diffuse_roughness)
    (metalness)
    (specular)
    (specular_color)
    (specular_roughness)
    (specular_IOR)
    (specular_anisotropy)
    (specular_rotation)
    (transmission)
    (transmission_color)
    (transmission_depth)
    (subsurface)
    (subsurface_color)
    (subsurface_radius)
    (subsurface_scale)
    (sheen)
    (sheen_color)
    (sheen_roughness)
    (coat)
    (coat_color)
    (coat_roughness)
    (coat_IOR)
    (coat_normal)
    (emission)
    (emission_color)
    (opacity)
    (thin_walled)
    (normal)
    (tangent)
);

SurfaceClosure
EvalStandardSurface(const ParamMap& params)
{
    SurfaceClosure c;

    float base     = Get<float>(params, _tokens->base, 1.0f);
    GfVec3f baseCol = Get<GfVec3f>(params, _tokens->base_color,
                                          GfVec3f(0.8f));
    c.baseColor = baseCol * base;

    c.roughness = Get<float>(params, _tokens->specular_roughness, 0.2f);
    c.metallic  = Get<float>(params, _tokens->metalness, 0.0f);

    float spec  = Get<float>(params, _tokens->specular, 1.0f);
    c.specular  = spec;
    c.specularColor = Get<GfVec3f>(params, _tokens->specular_color,
                                          GfVec3f(1.0f));
    c.specularIor   = Get<float>(params, _tokens->specular_IOR, 1.5f);

    c.transmission  = Get<float>(params, _tokens->transmission, 0.0f);
    c.transmissionColor = Get<GfVec3f>(params, _tokens->transmission_color,
                                              GfVec3f(1.0f));

    c.coat          = Get<float>(params, _tokens->coat, 0.0f);
    c.coatRoughness = Get<float>(params, _tokens->coat_roughness, 0.1f);
    c.coatIor       = Get<float>(params, _tokens->coat_IOR, 1.5f);

    c.sheen          = Get<float>(params, _tokens->sheen, 0.0f);
    c.sheenColor     = Get<GfVec3f>(params, _tokens->sheen_color,
                                           GfVec3f(1.0f));
    c.sheenRoughness = Get<float>(params, _tokens->sheen_roughness, 0.3f);

    float emissionWeight = Get<float>(params, _tokens->emission, 0.0f);
    GfVec3f emissionCol = Get<GfVec3f>(params, _tokens->emission_color,
                                              GfVec3f(1.0f));
    c.emissiveColor = emissionCol * emissionWeight;

    GfVec3f opacityVec = Get<GfVec3f>(params, _tokens->opacity,
                                             GfVec3f(1.0f));
    c.opacity = (opacityVec[0] + opacityVec[1] + opacityVec[2]) / 3.0f;

    c.thinWalled = Get<bool>(params, _tokens->thin_walled, false);
    // If opacity was authored as a float, pick it up.
    if (c.opacity == 1.0f) {
        c.opacity = Get<float>(params, _tokens->opacity, 1.0f);
    }

    c.normal = Get<GfVec3f>(params, _tokens->normal,
                                   GfVec3f(0.0f, 0.0f, 1.0f));

    return c;
}

} // namespace mxcpp
PXR_NAMESPACE_CLOSE_SCOPE
