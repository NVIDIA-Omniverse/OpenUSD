//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "disneyPrincipled.h"

#include "../paramMap.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace mxcpp {

static const SlotName _kBaseColor("baseColor");
static const SlotName _kMetallic("metallic");
static const SlotName _kRoughness("roughness");
static const SlotName _kAnisotropic("anisotropic");
static const SlotName _kSpecular("specular");
static const SlotName _kSpecularTint("specularTint");
static const SlotName _kSheen("sheen");
static const SlotName _kSheenTint("sheenTint");
static const SlotName _kClearcoat("clearcoat");
static const SlotName _kClearcoatGloss("clearcoatGloss");
static const SlotName _kSpecTrans("specTrans");
static const SlotName _kIor("ior");
static const SlotName _kSubsurface("subsurface");
static const SlotName _kSubsurfaceDistance("subsurfaceDistance");

namespace {

float
_Clamp01(float x)
{
    return std::clamp(x, 0.0f, 1.0f);
}

float
_ClampEpsilon(float x)
{
    return std::clamp(x, 1.0e-5f, 1.0f);
}

Vec2f
_ComputeRoughnessAnisotropy(float roughness, float anisotropy)
{
    const float roughnessSqr = _ClampEpsilon(roughness * roughness);
    const float clampedAnisotropy = std::clamp(anisotropy, 0.0f, 0.98f);
    if (clampedAnisotropy <= 0.0f) {
        return Vec2f(roughnessSqr, roughnessSqr);
    }

    const float aspect = std::sqrt(1.0f - clampedAnisotropy);
    return Vec2f(
        std::min(roughnessSqr / std::max(aspect, 1.0e-5f), 1.0f),
        roughnessSqr * aspect);
}

Vec2f
_ComputeDualRoughness(float roughness)
{
    const float roughnessSqr = _ClampEpsilon(roughness * roughness);
    return Vec2f(roughnessSqr, roughnessSqr);
}

Vec3f
_Lerp(const Vec3f& bg, const Vec3f& fg, float mix)
{
    return bg * (1.0f - mix) + fg * mix;
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
EvalDisneyPrincipled(const ParamMap& params)
{
    SurfaceClosure c;

    const Vec3f baseColor = Get<Vec3f>(params, _kBaseColor, Vec3f(0.16f));
    const float metallic = Get<float>(params, _kMetallic, 0.0f);
    const float roughness = Get<float>(params, _kRoughness, 0.5f);
    const float anisotropic = Get<float>(params, _kAnisotropic, 0.0f);
    const float specular = Get<float>(params, _kSpecular, 0.5f);
    const float specularTint = Get<float>(params, _kSpecularTint, 0.0f);
    const float sheen = Get<float>(params, _kSheen, 0.0f);
    const float sheenTint = Get<float>(params, _kSheenTint, 0.5f);
    const float clearcoat = Get<float>(params, _kClearcoat, 0.0f);
    const float clearcoatGloss = Get<float>(params, _kClearcoatGloss, 1.0f);
    const float specTrans = Get<float>(params, _kSpecTrans, 0.0f);
    const float ior = Get<float>(params, _kIor, 1.5f);
    const float subsurface = Get<float>(params, _kSubsurface, 0.0f);
    const Vec3f subsurfaceDistance =
        Get<Vec3f>(params, _kSubsurfaceDistance, Vec3f(1.0f));

    c.baseColor = baseColor;
    c.roughness = roughness;
    c.metallic = metallic;
    c.specular = specular * 0.08f;
    c.specularColor = _Lerp(Vec3f(1.0f), baseColor, _Clamp01(specularTint));
    c.specularIor = ior;
    c.transmission = specTrans;
    c.transmissionColor = baseColor;
    c.sheen = sheen;
    c.sheenColor = _Lerp(Vec3f(1.0f), baseColor, _Clamp01(sheenTint));
    c.coat = clearcoat * 0.04f;
    c.coatRoughness = 1.0f - clearcoatGloss;
    c.coatIor = 1.5f;

    Bsdf::ClosureTree tree;

    Bsdf::BurleyDiffuseData diffuse;
    diffuse.weight = _Clamp01(1.0f - metallic);
    diffuse.color = baseColor;
    diffuse.roughness = roughness;
    const Bsdf::NodeId diffuseId = tree.Add(diffuse);

    Bsdf::SubsurfaceData subsurfaceData;
    subsurfaceData.weight = 1.0f;
    subsurfaceData.color = baseColor;
    subsurfaceData.radius = subsurfaceDistance;
    const Bsdf::NodeId subsurfaceId = tree.Add(subsurfaceData);

    Bsdf::MixData subsurfaceMix;
    subsurfaceMix.bg = diffuseId;
    subsurfaceMix.fg = subsurfaceId;
    subsurfaceMix.mix = _Clamp01(subsurface);
    Bsdf::NodeId root = tree.Add(subsurfaceMix);

    if (_Clamp01(sheen) > 0.0f) {
        Bsdf::SheenData sheenData;
        sheenData.weight = _Clamp01(sheen);
        sheenData.color = _Lerp(Vec3f(1.0f), baseColor, _Clamp01(sheenTint));
        root = _AppendLayer(&tree, tree.Add(sheenData), root);
    }

    if (_Clamp01(specTrans) > 0.0f) {
        Bsdf::DielectricData transmission;
        transmission.weight = 1.0f;
        transmission.tint = baseColor;
        transmission.ior = std::max(ior, 1.0f);
        transmission.roughness = Vec2f(1.0e-5f, 1.0e-5f);
        transmission.scatterMode = Bsdf::ScatterMode::Transmission;

        Bsdf::MixData transmissionMix;
        transmissionMix.bg = root;
        transmissionMix.fg = tree.Add(transmission);
        transmissionMix.mix = _Clamp01(specTrans);
        root = tree.Add(transmissionMix);
    }

    const Vec2f specularRoughness =
        _ComputeRoughnessAnisotropy(roughness, anisotropic);
    const Vec3f dielectricTint =
        _Lerp(Vec3f(1.0f), baseColor, _Clamp01(specularTint));

    if (_Clamp01(specular) > 0.0f) {
        Bsdf::GeneralizedSchlickData dielectric;
        dielectric.weight = specular * 0.08f;
        dielectric.color0 = dielectricTint;
        dielectric.color82 = dielectricTint;
        dielectric.color90 = Vec3f(1.0f);
        dielectric.roughness = specularRoughness;
        root = _AppendLayer(&tree, tree.Add(dielectric), root);
    }

    Bsdf::GeneralizedSchlickData metallicData;
    metallicData.weight = 1.0f;
    metallicData.color0 = baseColor;
    metallicData.color82 = baseColor;
    metallicData.color90 = Vec3f(1.0f);
    metallicData.roughness = specularRoughness;

    const Bsdf::NodeId metallicId = tree.Add(metallicData);
    if (_Clamp01(metallic) <= 0.0f) {
        // no-op
    } else if (_Clamp01(metallic) >= 1.0f) {
        root = metallicId;
    } else {
        Bsdf::MixData metallicMix;
        metallicMix.bg = root;
        metallicMix.fg = metallicId;
        metallicMix.mix = _Clamp01(metallic);
        root = tree.Add(metallicMix);
    }

    if (_Clamp01(clearcoat) > 0.0f) {
        Bsdf::GeneralizedSchlickData coat;
        coat.weight = clearcoat * 0.04f;
        coat.color0 = Vec3f(1.0f);
        coat.color82 = Vec3f(1.0f);
        coat.color90 = Vec3f(1.0f);
        coat.roughness = _ComputeDualRoughness(1.0f - clearcoatGloss);
        root = _AppendLayer(&tree, tree.Add(coat), root);
    }

    tree.root = root;
    c.bsdfTree = std::move(tree);
    return c;
}

}  // namespace mxcpp
