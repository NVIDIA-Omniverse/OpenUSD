//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "openPbr.h"

#include "../paramMap.h"

#include <algorithm>
#include <cmath>
#include <utility>

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

namespace {

float
_Clamp01(float x)
{
    return std::clamp(x, 0.0f, 1.0f);
}

float
_ClampRoughness(float roughness)
{
    return std::clamp(roughness, 0.001f, 1.0f);
}

Vec3f
_Saturate(const Vec3f& value)
{
    return Vec3f(
        _Clamp01(value[0]),
        _Clamp01(value[1]),
        _Clamp01(value[2]));
}

Vec2f
_ComputeAnisotropicRoughness(float roughness, float anisotropy)
{
    const float clampedRoughness = _ClampRoughness(roughness);
    const float clampedAnisotropy = std::clamp(anisotropy, -0.95f, 0.95f);
    const float aspect =
        std::sqrt(std::max(0.01f, 1.0f - 0.9f * clampedAnisotropy));
    return Vec2f(
        _ClampRoughness(clampedRoughness / aspect),
        _ClampRoughness(clampedRoughness * aspect));
}

Bsdf::NodeId
_AppendAdd(Bsdf::ClosureTree* tree, Bsdf::NodeId lhs, Bsdf::NodeId rhs)
{
    if (!tree->IsValid(lhs)) {
        return rhs;
    }
    if (!tree->IsValid(rhs)) {
        return lhs;
    }

    Bsdf::AddData add;
    add.in1 = lhs;
    add.in2 = rhs;
    return tree->Add(add);
}

Bsdf::NodeId
_AppendLayer(Bsdf::ClosureTree* tree, Bsdf::NodeId top, Bsdf::NodeId base)
{
    if (!tree->IsValid(top)) {
        return base;
    }
    if (!tree->IsValid(base)) {
        return top;
    }

    Bsdf::LayerData layer;
    layer.top = top;
    layer.base = base;
    return tree->Add(layer);
}

}  // namespace

SurfaceClosure
EvalOpenPbr(const ParamMap& params)
{
    SurfaceClosure c;

    const float baseWeight = Get<float>(params, _kBaseWeight, 1.0f);
    const Vec3f baseColor = Get<Vec3f>(params, _kBaseColor, Vec3f(0.8f));
    const float baseRoughness = Get<float>(params, _kBaseRoughness, 0.0f);

    c.baseColor = baseColor * baseWeight;
    c.roughness = Get<float>(params, _kSpecularRoughness, 0.3f);
    c.metallic = Get<float>(params, _kBaseMetalness, 0.0f);

    c.specular = Get<float>(params, _kSpecularWeight, 1.0f);
    c.specularColor = Get<Vec3f>(params, _kSpecularColor, Vec3f(1.0f));
    c.specularIor = Get<float>(params, _kSpecularIor, 1.5f);

    const float specularAnisotropy =
        Get<float>(params, _kSpecularAnisotropy, 0.0f);
    const Vec2f specularRoughness =
        _ComputeAnisotropicRoughness(c.roughness, specularAnisotropy);

    c.transmission = Get<float>(params, _kTransmissionWeight, 0.0f);
    c.transmissionColor =
        Get<Vec3f>(params, _kTransmissionColor, Vec3f(1.0f));

    c.coat = Get<float>(params, _kCoatWeight, 0.0f);
    const Vec3f coatColor = Get<Vec3f>(params, _kCoatColor, Vec3f(1.0f));
    c.coatRoughness = Get<float>(params, _kCoatRoughness, 0.0f);
    c.coatIor = Get<float>(params, _kCoatIor, 1.6f);

    c.sheen = Get<float>(params, _kFuzzWeight, 0.0f);
    c.sheenColor = Get<Vec3f>(params, _kFuzzColor, Vec3f(1.0f));
    c.sheenRoughness = Get<float>(params, _kFuzzRoughness, 0.5f);

    const float emissionLum = Get<float>(params, _kEmissionLuminance, 0.0f);
    const Vec3f emissionColor = Get<Vec3f>(params, _kEmissionColor, Vec3f(1.0f));
    c.emissiveColor = emissionColor * emissionLum;

    const Vec3f opacityVec = Get<Vec3f>(params, _kGeometryOpacity, Vec3f(1.0f));
    c.opacity = (opacityVec[0] + opacityVec[1] + opacityVec[2]) / 3.0f;
    if (c.opacity == 1.0f) {
        c.opacity = Get<float>(params, _kGeometryOpacity, 1.0f);
    }
    c.presence = c.opacity;

    c.thinWalled = Get<bool>(params, _kGeometryThinWalled, false);
    c.normal = Get<Vec3f>(params, _kNormal, Vec3f(0.0f, 0.0f, 1.0f));
    const Vec3f tangent =
        Get<Vec3f>(params, _kTangent, Vec3f(1.0f, 0.0f, 0.0f));

    Bsdf::ClosureTree tree;
    Bsdf::NodeId root = Bsdf::InvalidNodeId;

    const float diffuseWeight = _Clamp01(
        baseWeight * (1.0f - c.metallic) * (1.0f - c.transmission));
    if (diffuseWeight > 0.0f) {
        Bsdf::OrenNayarDiffuseData diffuse;
        diffuse.weight = diffuseWeight;
        diffuse.color = baseColor;
        diffuse.roughness = baseRoughness;
        diffuse.energyCompensation = true;
        root = _AppendAdd(&tree, root, tree.Add(diffuse));
    }

    const float metalMix = _Clamp01(c.metallic);
    const float specularWeight = _Clamp01(c.specular);
    if (specularWeight > 0.0f) {
        Bsdf::DielectricData dielectric;
        dielectric.weight = specularWeight;
        dielectric.tint = _Saturate(c.specularColor);
        dielectric.ior = std::max(c.specularIor, 1.0f);
        dielectric.roughness = specularRoughness;
        dielectric.tangent = tangent;
        dielectric.scatterMode = Bsdf::ScatterMode::Reflection;

        Bsdf::GeneralizedSchlickData metal;
        metal.weight = specularWeight;
        metal.color0 = _Saturate(CompMul(baseColor, c.specularColor));
        metal.color82 = metal.color0;
        metal.color90 = Vec3f(1.0f);
        metal.exponent = 5.0f;
        metal.roughness = specularRoughness;
        metal.tangent = tangent;
        metal.scatterMode = Bsdf::ScatterMode::Reflection;

        const Bsdf::NodeId dielectricId = tree.Add(dielectric);
        const Bsdf::NodeId metalId = tree.Add(metal);
        if (metalMix <= 0.0f) {
            root = _AppendAdd(&tree, root, dielectricId);
        } else if (metalMix >= 1.0f) {
            root = _AppendAdd(&tree, root, metalId);
        } else {
            Bsdf::MixData specMix;
            specMix.bg = dielectricId;
            specMix.fg = metalId;
            specMix.mix = metalMix;
            root = _AppendAdd(&tree, root, tree.Add(specMix));
        }
    }

    const float transmissionWeight =
        _Clamp01(c.transmission * (1.0f - c.metallic));
    if (transmissionWeight > 0.0f) {
        if (c.thinWalled) {
            Bsdf::TranslucentData translucent;
            translucent.weight = transmissionWeight;
            translucent.color = _Saturate(c.transmissionColor);
            root = _AppendAdd(&tree, root, tree.Add(translucent));
        } else {
            Bsdf::DielectricData transmission;
            transmission.weight = transmissionWeight;
            transmission.tint = _Saturate(c.transmissionColor);
            transmission.ior = std::max(c.specularIor, 1.0f);
            transmission.roughness = specularRoughness;
            transmission.tangent = tangent;
            transmission.scatterMode = Bsdf::ScatterMode::Transmission;
            root = _AppendAdd(&tree, root, tree.Add(transmission));
        }
    }

    if (_Clamp01(c.sheen) > 0.0f) {
        Bsdf::SheenData fuzz;
        fuzz.weight = _Clamp01(c.sheen);
        fuzz.color = _Saturate(c.sheenColor);
        fuzz.roughness = _ClampRoughness(c.sheenRoughness);
        fuzz.mode = Bsdf::SheenMode::Zeltner;
        root = _AppendAdd(&tree, root, tree.Add(fuzz));
    }

    if (_Clamp01(c.coat) > 0.0f) {
        Bsdf::DielectricData coat;
        coat.weight = _Clamp01(c.coat);
        coat.tint = _Saturate(coatColor);
        coat.ior = std::max(c.coatIor, 1.0f);
        coat.roughness = Vec2f(
            _ClampRoughness(c.coatRoughness),
            _ClampRoughness(c.coatRoughness));
        coat.tangent = tangent;
        coat.scatterMode = Bsdf::ScatterMode::Reflection;
        root = _AppendLayer(&tree, tree.Add(coat), root);
    }

    tree.root = root;
    c.bsdfTree = std::move(tree);

    return c;
}

}  // namespace mxcpp
