//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "openPbr.h"

namespace mxcpp {

static const std::string _kBaseWeight = "base_weight";
static const std::string _kBaseColor = "base_color";
static const std::string _kBaseRoughness = "base_roughness";
static const std::string _kBaseMetalness = "base_metalness";
static const std::string _kSpecularWeight = "specular_weight";
static const std::string _kSpecularColor = "specular_color";
static const std::string _kSpecularRoughness = "specular_roughness";
static const std::string _kSpecularIor = "specular_ior";
static const std::string _kSpecularAnisotropy = "specular_anisotropy";
static const std::string _kTransmissionWeight = "transmission_weight";
static const std::string _kTransmissionColor = "transmission_color";
static const std::string _kTransmissionDepth = "transmission_depth";
static const std::string _kSubsurfaceWeight = "subsurface_weight";
static const std::string _kSubsurfaceColor = "subsurface_color";
static const std::string _kSubsurfaceRadius = "subsurface_radius";
static const std::string _kSubsurfaceRadiusScale = "subsurface_radius_scale";
static const std::string _kCoatWeight = "coat_weight";
static const std::string _kCoatColor = "coat_color";
static const std::string _kCoatRoughness = "coat_roughness";
static const std::string _kCoatIor = "coat_ior";
static const std::string _kCoatNormal = "coat_normal";
static const std::string _kFuzzWeight = "fuzz_weight";
static const std::string _kFuzzColor = "fuzz_color";
static const std::string _kFuzzRoughness = "fuzz_roughness";
static const std::string _kEmissionLuminance = "emission_luminance";
static const std::string _kEmissionColor = "emission_color";
static const std::string _kGeometryOpacity = "geometry_opacity";
static const std::string _kGeometryThinWalled = "geometry_thin_walled";
static const std::string _kNormal = "normal";
static const std::string _kTangent = "tangent";

SurfaceClosure
EvalOpenPbr(const ParamMap& params)
{
    SurfaceClosure c;

    float baseW = Get<float>(params, _kBaseWeight, 1.0f);
    Vec3f baseCol = Get<Vec3f>(params, _kBaseColor,
                                          Vec3f(0.8f));
    c.baseColor = baseCol * baseW;

    c.roughness = Get<float>(params, _kSpecularRoughness, 0.3f);
    c.metallic  = Get<float>(params, _kBaseMetalness, 0.0f);

    c.specular = Get<float>(params, _kSpecularWeight, 1.0f);
    c.specularColor = Get<Vec3f>(params, _kSpecularColor,
                                          Vec3f(1.0f));
    c.specularIor = Get<float>(params, _kSpecularIor, 1.5f);

    c.transmission = Get<float>(params, _kTransmissionWeight, 0.0f);
    c.transmissionColor = Get<Vec3f>(params, _kTransmissionColor,
                                              Vec3f(1.0f));

    c.coat = Get<float>(params, _kCoatWeight, 0.0f);
    c.coatRoughness = Get<float>(params, _kCoatRoughness, 0.0f);
    c.coatIor = Get<float>(params, _kCoatIor, 1.6f);

    // OpenPBR uses "fuzz" for sheen-like lobe.
    c.sheen = Get<float>(params, _kFuzzWeight, 0.0f);
    c.sheenColor = Get<Vec3f>(params, _kFuzzColor,
                                       Vec3f(1.0f));
    c.sheenRoughness = Get<float>(params, _kFuzzRoughness, 0.5f);

    float emissionLum = Get<float>(
        params, _kEmissionLuminance, 0.0f);
    Vec3f emissionCol = Get<Vec3f>(
        params, _kEmissionColor, Vec3f(1.0f));
    c.emissiveColor = emissionCol * emissionLum;

    Vec3f opacityVec = Get<Vec3f>(
        params, _kGeometryOpacity, Vec3f(1.0f));
    c.opacity = (opacityVec[0] + opacityVec[1] + opacityVec[2]) / 3.0f;
    if (c.opacity == 1.0f) {
        c.opacity = Get<float>(params, _kGeometryOpacity, 1.0f);
    }

    c.thinWalled = Get<bool>(
        params, _kGeometryThinWalled, false);

    c.normal = Get<Vec3f>(params, _kNormal,
                                   Vec3f(0.0f, 0.0f, 1.0f));

    return c;
}

} // namespace mxcpp
