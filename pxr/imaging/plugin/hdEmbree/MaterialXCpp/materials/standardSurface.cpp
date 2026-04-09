//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "standardSurface.h"

#include "../paramMap.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace mxcpp {

static const SlotName _kBase("base");
static const SlotName _kBaseColor("base_color");
static const SlotName _kDiffuseRoughness("diffuse_roughness");
static const SlotName _kMetalness("metalness");
static const SlotName _kSpecular("specular");
static const SlotName _kSpecularColor("specular_color");
static const SlotName _kSpecularRoughness("specular_roughness");
static const SlotName _kSpecularIOR("specular_IOR");
static const SlotName _kSpecularAnisotropy("specular_anisotropy");
static const SlotName _kSpecularRotation("specular_rotation");
static const SlotName _kTransmission("transmission");
static const SlotName _kTransmissionColor("transmission_color");
static const SlotName _kTransmissionDepth("transmission_depth");
static const SlotName _kSubsurface("subsurface");
static const SlotName _kSubsurfaceColor("subsurface_color");
static const SlotName _kSubsurfaceRadius("subsurface_radius");
static const SlotName _kSubsurfaceScale("subsurface_scale");
static const SlotName _kSheen("sheen");
static const SlotName _kSheenColor("sheen_color");
static const SlotName _kSheenRoughness("sheen_roughness");
static const SlotName _kCoat("coat");
static const SlotName _kCoatColor("coat_color");
static const SlotName _kCoatRoughness("coat_roughness");
static const SlotName _kCoatIOR("coat_IOR");
static const SlotName _kCoatNormal("coat_normal");
static const SlotName _kThinFilmThickness("thin_film_thickness");
static const SlotName _kThinFilmIOR("thin_film_IOR");
static const SlotName _kEmission("emission");
static const SlotName _kEmissionColor("emission_color");
static const SlotName _kOpacity("opacity");
static const SlotName _kThinWalled("thin_walled");
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

Vec3f
_ExtinctionFromF0(const Vec3f& f0)
{
    Vec3f extinction(0.0f);
    for (int i = 0; i < 3; ++i) {
        const float clamped = std::clamp(f0[i], 0.0f, 0.999f);
        extinction[i] = 2.0f *
            std::sqrt(clamped / std::max(1.0e-4f, 1.0f - clamped));
    }
    return extinction;
}

Bsdf::ConductorData
_MakeApproxConductor(
    float weight,
    const Vec3f& f0,
    const Vec2f& roughness,
    const Vec3f& tangent)
{
    Bsdf::ConductorData data;
    data.weight = _Clamp01(weight);
    data.ior = Vec3f(1.0f);
    data.extinction = _ExtinctionFromF0(_Saturate(f0));
    data.roughness = roughness;
    data.tangent = tangent;
    return data;
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
EvalStandardSurface(const ParamMap& params)
{
    SurfaceClosure c;

    const float base = Get<float>(params, _kBase, 1.0f);
    const Vec3f baseCol = Get<Vec3f>(params, _kBaseColor, Vec3f(0.8f));
    const float diffuseRoughness = Get<float>(params, _kDiffuseRoughness, 0.0f);

    c.baseColor = baseCol * base;
    c.roughness = Get<float>(params, _kSpecularRoughness, 0.2f);
    c.metallic = Get<float>(params, _kMetalness, 0.0f);

    const float spec = Get<float>(params, _kSpecular, 1.0f);
    c.specular = spec;
    c.specularColor = Get<Vec3f>(params, _kSpecularColor, Vec3f(1.0f));
    c.specularIor = Get<float>(params, _kSpecularIOR, 1.5f);

    const float specularAnisotropy =
        Get<float>(params, _kSpecularAnisotropy, 0.0f);
    const Vec2f specularRoughness =
        _ComputeAnisotropicRoughness(c.roughness, specularAnisotropy);

    c.transmission = Get<float>(params, _kTransmission, 0.0f);
    c.transmissionColor =
        Get<Vec3f>(params, _kTransmissionColor, Vec3f(1.0f));

    c.coat = Get<float>(params, _kCoat, 0.0f);
    const Vec3f coatColor = Get<Vec3f>(params, _kCoatColor, Vec3f(1.0f));
    c.coatRoughness = Get<float>(params, _kCoatRoughness, 0.1f);
    c.coatIor = Get<float>(params, _kCoatIOR, 1.5f);
    const float thinFilmThicknessNm = std::max(
        Get<float>(params, _kThinFilmThickness, 0.0f),
        0.0f);
    const float thinFilmIor = std::max(
        Get<float>(params, _kThinFilmIOR, 1.5f),
        1.0f);

    c.sheen = Get<float>(params, _kSheen, 0.0f);
    c.sheenColor = Get<Vec3f>(params, _kSheenColor, Vec3f(1.0f));
    c.sheenRoughness = Get<float>(params, _kSheenRoughness, 0.3f);

    const float emissionWeight = Get<float>(params, _kEmission, 0.0f);
    const Vec3f emissionCol = Get<Vec3f>(params, _kEmissionColor, Vec3f(1.0f));
    c.emissiveColor = emissionCol * emissionWeight;

    const Vec3f opacityVec = Get<Vec3f>(params, _kOpacity, Vec3f(1.0f));
    c.opacity = (opacityVec[0] + opacityVec[1] + opacityVec[2]) / 3.0f;

    c.thinWalled = Get<bool>(params, _kThinWalled, false);
    if (c.opacity == 1.0f) {
        c.opacity = Get<float>(params, _kOpacity, 1.0f);
    }
    c.presence = c.opacity;

    c.normal = Get<Vec3f>(params, _kNormal, Vec3f(0.0f, 0.0f, 1.0f));
    const Vec3f tangent =
        Get<Vec3f>(params, _kTangent, Vec3f(1.0f, 0.0f, 0.0f));

    Bsdf::ClosureTree tree;
    Bsdf::NodeId root = Bsdf::InvalidNodeId;

    const float diffuseWeight =
        _Clamp01(base * (1.0f - c.metallic) * (1.0f - c.transmission));
    if (diffuseWeight > 0.0f) {
        Bsdf::OrenNayarDiffuseData diffuse;
        diffuse.weight = diffuseWeight;
        diffuse.color = baseCol;
        diffuse.roughness = diffuseRoughness;
        diffuse.energyCompensation = true;
        root = _AppendAdd(&tree, root, tree.Add(diffuse));
    }

    const float clampedSpec = _Clamp01(spec);
    const float metalMix = _Clamp01(c.metallic);
    if (clampedSpec > 0.0f) {
        Bsdf::DielectricData dielectric;
        dielectric.weight = clampedSpec;
        dielectric.tint = _Saturate(c.specularColor);
        dielectric.ior = std::max(c.specularIor, 1.0f);
        dielectric.roughness = specularRoughness;
        dielectric.tangent = tangent;
        dielectric.scatterMode = Bsdf::ScatterMode::Reflection;
        dielectric.thinFilmWeight = 1.0f;
        dielectric.thinFilmThickness = thinFilmThicknessNm;
        dielectric.thinFilmIor = thinFilmIor;

        const Vec3f metalF0 = _Saturate(CompMul(baseCol, c.specularColor));
        Bsdf::ConductorData conductor =
            _MakeApproxConductor(clampedSpec, metalF0, specularRoughness, tangent);
        conductor.thinFilmWeight = 1.0f;
        conductor.thinFilmThickness = thinFilmThicknessNm;
        conductor.thinFilmIor = thinFilmIor;

        const Bsdf::NodeId dielectricId = tree.Add(dielectric);
        const Bsdf::NodeId conductorId = tree.Add(conductor);

        if (metalMix <= 0.0f) {
            root = _AppendAdd(&tree, root, dielectricId);
        } else if (metalMix >= 1.0f) {
            root = _AppendAdd(&tree, root, conductorId);
        } else {
            Bsdf::MixData specMix;
            specMix.bg = dielectricId;
            specMix.fg = conductorId;
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
        Bsdf::SheenData sheen;
        sheen.weight = _Clamp01(c.sheen);
        sheen.color = _Saturate(c.sheenColor);
        sheen.roughness = _ClampRoughness(c.sheenRoughness);
        root = _AppendAdd(&tree, root, tree.Add(sheen));
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
