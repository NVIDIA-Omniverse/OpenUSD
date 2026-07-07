//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pbrNodes.h"

#include "../nodeRegistry.h"
#include "../paramMap.h"

#include <string>
#include <utility>

namespace mxcpp {

namespace {

const SlotName _kOut("out");
const SlotName _kWeight("weight");
const SlotName _kColor("color");
const SlotName _kRoughness("roughness");
const SlotName _kEnergyCompensation("energy_compensation");
const SlotName _kRadius("radius");
const SlotName _kAnisotropy("anisotropy");
const SlotName _kMode("mode");

BsdfClosure
_MakeClosure(Bsdf::NodeData data)
{
    BsdfClosure closure;
    closure.tree.root = closure.tree.Add(std::move(data));
    return closure;
}

void
_EvalOrenNayarDiffuseBsdf(
    const ParamMap& inputs,
    const ShadingContext&,
    NodeOutputMap* outputs)
{
    Bsdf::OrenNayarDiffuseData data;
    data.weight = Get<float>(inputs, _kWeight, 1.0f);
    data.color = Get<Vec3f>(inputs, _kColor, Vec3f(0.18f));
    data.roughness = Get<float>(inputs, _kRoughness, 0.0f);
    data.energyCompensation = Get<bool>(inputs, _kEnergyCompensation, false);
    (*outputs)[_kOut] = Value(_MakeClosure(data));
}

void
_EvalBurleyDiffuseBsdf(
    const ParamMap& inputs,
    const ShadingContext&,
    NodeOutputMap* outputs)
{
    Bsdf::BurleyDiffuseData data;
    data.weight = Get<float>(inputs, _kWeight, 1.0f);
    data.color = Get<Vec3f>(inputs, _kColor, Vec3f(0.18f));
    data.roughness = Get<float>(inputs, _kRoughness, 0.0f);
    (*outputs)[_kOut] = Value(_MakeClosure(data));
}

void
_EvalTranslucentBsdf(
    const ParamMap& inputs,
    const ShadingContext&,
    NodeOutputMap* outputs)
{
    Bsdf::TranslucentData data;
    data.weight = Get<float>(inputs, _kWeight, 1.0f);
    data.color = Get<Vec3f>(inputs, _kColor, Vec3f(1.0f));
    (*outputs)[_kOut] = Value(_MakeClosure(data));
}

void
_EvalSubsurfaceBsdf(
    const ParamMap& inputs,
    const ShadingContext&,
    NodeOutputMap* outputs)
{
    Bsdf::SubsurfaceData data;
    data.weight = Get<float>(inputs, _kWeight, 1.0f);
    data.color = Get<Vec3f>(inputs, _kColor, Vec3f(0.18f));
    data.radius = Get<Vec3f>(inputs, _kRadius, Vec3f(1.0f));
    data.anisotropy = Get<float>(inputs, _kAnisotropy, 0.0f);
    (*outputs)[_kOut] = Value(_MakeClosure(data));
}

void
_EvalSheenBsdf(
    const ParamMap& inputs,
    const ShadingContext&,
    NodeOutputMap* outputs)
{
    Bsdf::SheenData data;
    data.weight = Get<float>(inputs, _kWeight, 1.0f);
    data.color = Get<Vec3f>(inputs, _kColor, Vec3f(1.0f));
    data.roughness = Get<float>(inputs, _kRoughness, 0.3f);

    const std::string mode = Get<std::string>(
        inputs, _kMode, std::string("conty_kulla"));
    data.mode = mode == "zeltner"
        ? Bsdf::SheenMode::Zeltner
        : Bsdf::SheenMode::ContyKulla;

    (*outputs)[_kOut] = Value(_MakeClosure(data));
}

void
_EvalChiangHairBsdf(
    const ParamMap&,
    const ShadingContext&,
    NodeOutputMap* outputs)
{
    Bsdf::UnsupportedData data;
    data.kind = Bsdf::UnsupportedNodeKind::ChiangHair;
    (*outputs)[_kOut] = Value(_MakeClosure(data));
}

}  // namespace

#define _REG(name, fn) reg.Register(name, fn)

void
RegisterPbrNodes(NodeRegistry& reg)
{
    _REG("ND_oren_nayar_diffuse_bsdf", &_EvalOrenNayarDiffuseBsdf);
    _REG("ND_burley_diffuse_bsdf", &_EvalBurleyDiffuseBsdf);
    _REG("ND_translucent_bsdf", &_EvalTranslucentBsdf);
    _REG("ND_subsurface_bsdf", &_EvalSubsurfaceBsdf);
    _REG("ND_sheen_bsdf", &_EvalSheenBsdf);
    _REG("ND_chiang_hair_bsdf", &_EvalChiangHairBsdf);
}

#undef _REG

}  // namespace mxcpp
