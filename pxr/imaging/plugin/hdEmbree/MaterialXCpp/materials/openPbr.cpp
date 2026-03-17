//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "openPbr.h"

namespace mxcpp {

static const SlotName _kBaseWeight("base_weight");
static const SlotName _kBaseColor("base_color");
static const SlotName _kBaseRoughness("base_roughness");
static const SlotName _kBaseMetalness("base_metalness");
static const SlotName _kSpecularWeight("specular_weight");
static const SlotName _kSpecularColor("specular_color");
static const SlotName _kSpecularRoughness("specular_roughness");
static const SlotName _kSpecularIor("specular_ior");
static const SlotName _kSpecularAnisotropy("specular_anisotropy");
static const SlotName _kTransmissionWeight("transmission_weight");
static const SlotName _kTransmissionColor("transmission_color");
static const SlotName _kTransmissionDepth("transmission_depth");
static const SlotName _kSubsurfaceWeight("subsurface_weight");
static const SlotName _kSubsurfaceColor("subsurface_color");
static const SlotName _kSubsurfaceRadius("subsurface_radius");
static const SlotName _kSubsurfaceRadiusScale("subsurface_radius_scale");
static const SlotName _kCoatWeight("coat_weight");
static const SlotName _kCoatColor("coat_color");
static const SlotName _kCoatRoughness("coat_roughness");
static const SlotName _kCoatIor("coat_ior");
static const SlotName _kCoatNormal("coat_normal");
static const SlotName _kFuzzWeight("fuzz_weight");
static const SlotName _kFuzzColor("fuzz_color");
static const SlotName _kFuzzRoughness("fuzz_roughness");
static const SlotName _kEmissionLuminance("emission_luminance");
static const SlotName _kEmissionColor("emission_color");
static const SlotName _kGeometryOpacity("geometry_opacity");
static const SlotName _kGeometryThinWalled("geometry_thin_walled");
static const SlotName _kNormal("normal");
static const SlotName _kTangent("tangent");

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

}  // namespace mxcpp
