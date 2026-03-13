//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/imaging/plugin/hdEmbree/MaterialXCpp/materials/openPbr.h"

#include "pxr/base/tf/staticTokens.h"

PXR_NAMESPACE_OPEN_SCOPE
namespace mxcpp {

TF_DEFINE_PRIVATE_TOKENS(_tokens,
    (base_weight)
    (base_color)
    (base_roughness)
    (base_metalness)
    (specular_weight)
    (specular_color)
    (specular_roughness)
    (specular_ior)
    (specular_anisotropy)
    (transmission_weight)
    (transmission_color)
    (transmission_depth)
    (subsurface_weight)
    (subsurface_color)
    (subsurface_radius)
    (subsurface_radius_scale)
    (coat_weight)
    (coat_color)
    (coat_roughness)
    (coat_ior)
    (coat_normal)
    (fuzz_weight)
    (fuzz_color)
    (fuzz_roughness)
    (emission_luminance)
    (emission_color)
    (geometry_opacity)
    (geometry_thin_walled)
    (normal)
    (tangent)
);

SurfaceClosure
EvalOpenPbr(const ParamMap& params)
{
    SurfaceClosure c;

    float baseW = Get<float>(params, _tokens->base_weight, 1.0f);
    GfVec3f baseCol = Get<GfVec3f>(params, _tokens->base_color,
                                          GfVec3f(0.8f));
    c.baseColor = baseCol * baseW;

    c.roughness = Get<float>(params, _tokens->specular_roughness, 0.3f);
    c.metallic  = Get<float>(params, _tokens->base_metalness, 0.0f);

    c.specular = Get<float>(params, _tokens->specular_weight, 1.0f);
    c.specularColor = Get<GfVec3f>(params, _tokens->specular_color,
                                          GfVec3f(1.0f));
    c.specularIor = Get<float>(params, _tokens->specular_ior, 1.5f);

    c.transmission = Get<float>(params, _tokens->transmission_weight, 0.0f);
    c.transmissionColor = Get<GfVec3f>(params, _tokens->transmission_color,
                                              GfVec3f(1.0f));

    c.coat = Get<float>(params, _tokens->coat_weight, 0.0f);
    c.coatRoughness = Get<float>(params, _tokens->coat_roughness, 0.0f);
    c.coatIor = Get<float>(params, _tokens->coat_ior, 1.6f);

    // OpenPBR uses "fuzz" for sheen-like lobe.
    c.sheen = Get<float>(params, _tokens->fuzz_weight, 0.0f);
    c.sheenColor = Get<GfVec3f>(params, _tokens->fuzz_color,
                                       GfVec3f(1.0f));
    c.sheenRoughness = Get<float>(params, _tokens->fuzz_roughness, 0.5f);

    float emissionLum = Get<float>(
        params, _tokens->emission_luminance, 0.0f);
    GfVec3f emissionCol = Get<GfVec3f>(
        params, _tokens->emission_color, GfVec3f(1.0f));
    c.emissiveColor = emissionCol * emissionLum;

    GfVec3f opacityVec = Get<GfVec3f>(
        params, _tokens->geometry_opacity, GfVec3f(1.0f));
    c.opacity = (opacityVec[0] + opacityVec[1] + opacityVec[2]) / 3.0f;
    if (c.opacity == 1.0f) {
        c.opacity = Get<float>(params, _tokens->geometry_opacity, 1.0f);
    }

    c.thinWalled = Get<bool>(
        params, _tokens->geometry_thin_walled, false);

    c.normal = Get<GfVec3f>(params, _tokens->normal,
                                   GfVec3f(0.0f, 0.0f, 1.0f));

    return c;
}

} // namespace mxcpp
PXR_NAMESPACE_CLOSE_SCOPE
