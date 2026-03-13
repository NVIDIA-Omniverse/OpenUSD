//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "standardSurface.h"

namespace mxcpp {

static const std::string _kBase = "base";
static const std::string _kBaseColor = "base_color";
static const std::string _kDiffuseRoughness = "diffuse_roughness";
static const std::string _kMetalness = "metalness";
static const std::string _kSpecular = "specular";
static const std::string _kSpecularColor = "specular_color";
static const std::string _kSpecularRoughness = "specular_roughness";
static const std::string _kSpecularIOR = "specular_IOR";
static const std::string _kSpecularAnisotropy = "specular_anisotropy";
static const std::string _kSpecularRotation = "specular_rotation";
static const std::string _kTransmission = "transmission";
static const std::string _kTransmissionColor = "transmission_color";
static const std::string _kTransmissionDepth = "transmission_depth";
static const std::string _kSubsurface = "subsurface";
static const std::string _kSubsurfaceColor = "subsurface_color";
static const std::string _kSubsurfaceRadius = "subsurface_radius";
static const std::string _kSubsurfaceScale = "subsurface_scale";
static const std::string _kSheen = "sheen";
static const std::string _kSheenColor = "sheen_color";
static const std::string _kSheenRoughness = "sheen_roughness";
static const std::string _kCoat = "coat";
static const std::string _kCoatColor = "coat_color";
static const std::string _kCoatRoughness = "coat_roughness";
static const std::string _kCoatIOR = "coat_IOR";
static const std::string _kCoatNormal = "coat_normal";
static const std::string _kEmission = "emission";
static const std::string _kEmissionColor = "emission_color";
static const std::string _kOpacity = "opacity";
static const std::string _kThinWalled = "thin_walled";
static const std::string _kNormal = "normal";
static const std::string _kTangent = "tangent";

SurfaceClosure
EvalStandardSurface(const ParamMap& params)
{
    SurfaceClosure c;

    float base     = Get<float>(params, _kBase, 1.0f);
    Vec3f baseCol = Get<Vec3f>(params, _kBaseColor,
                                          Vec3f(0.8f));
    c.baseColor = baseCol * base;

    c.roughness = Get<float>(params, _kSpecularRoughness, 0.2f);
    c.metallic  = Get<float>(params, _kMetalness, 0.0f);

    float spec  = Get<float>(params, _kSpecular, 1.0f);
    c.specular  = spec;
    c.specularColor = Get<Vec3f>(params, _kSpecularColor,
                                          Vec3f(1.0f));
    c.specularIor   = Get<float>(params, _kSpecularIOR, 1.5f);

    c.transmission  = Get<float>(params, _kTransmission, 0.0f);
    c.transmissionColor = Get<Vec3f>(params, _kTransmissionColor,
                                              Vec3f(1.0f));

    c.coat          = Get<float>(params, _kCoat, 0.0f);
    c.coatRoughness = Get<float>(params, _kCoatRoughness, 0.1f);
    c.coatIor       = Get<float>(params, _kCoatIOR, 1.5f);

    c.sheen          = Get<float>(params, _kSheen, 0.0f);
    c.sheenColor     = Get<Vec3f>(params, _kSheenColor,
                                           Vec3f(1.0f));
    c.sheenRoughness = Get<float>(params, _kSheenRoughness, 0.3f);

    float emissionWeight = Get<float>(params, _kEmission, 0.0f);
    Vec3f emissionCol = Get<Vec3f>(params, _kEmissionColor,
                                              Vec3f(1.0f));
    c.emissiveColor = emissionCol * emissionWeight;

    Vec3f opacityVec = Get<Vec3f>(params, _kOpacity,
                                             Vec3f(1.0f));
    c.opacity = (opacityVec[0] + opacityVec[1] + opacityVec[2]) / 3.0f;

    c.thinWalled = Get<bool>(params, _kThinWalled, false);
    // If opacity was authored as a float, pick it up.
    if (c.opacity == 1.0f) {
        c.opacity = Get<float>(params, _kOpacity, 1.0f);
    }

    c.normal = Get<Vec3f>(params, _kNormal,
                                   Vec3f(0.0f, 0.0f, 1.0f));

    return c;
}

} // namespace mxcpp
