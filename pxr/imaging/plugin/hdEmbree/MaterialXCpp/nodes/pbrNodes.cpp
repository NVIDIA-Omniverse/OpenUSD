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
const SlotName _kTint("tint");
const SlotName _kIor("ior");
const SlotName _kRoughness("roughness");
const SlotName _kRetroreflective("retroreflective");
const SlotName _kThinfilmThickness("thinfilm_thickness");
const SlotName _kThinfilmIor("thinfilm_ior");
const SlotName _kNormal("normal");
const SlotName _kTangent("tangent");
const SlotName _kScatterMode("scatter_mode");
const SlotName _kEnergyCompensation("energy_compensation");
const SlotName _kRadius("radius");
const SlotName _kAbsorption("absorption");
const SlotName _kScattering("scattering");
const SlotName _kAnisotropy("anisotropy");
const SlotName _kMode("mode");
const SlotName _kTop("top");
const SlotName _kBase("base");

BsdfClosure
_MakeClosure(Bsdf::NodeData data)
{
    BsdfClosure closure;
    closure.tree.root = closure.tree.Add(std::move(data));
    return closure;
}

Bsdf::ScatterMode
_GetScatterMode(const ParamMap& inputs)
{
    const std::string mode = Get<std::string>(
        inputs, _kScatterMode, std::string("R"));
    if (mode == "T") {
        return Bsdf::ScatterMode::Transmission;
    }
    if (mode == "RT") {
        return Bsdf::ScatterMode::ReflectionTransmission;
    }
    return Bsdf::ScatterMode::Reflection;
}

Bsdf::NodeId
_RemapNodeId(
    const Bsdf::ClosureTree& source,
    Bsdf::NodeId id,
    Bsdf::NodeId offset)
{
    return source.IsValid(id) ? id + offset : Bsdf::InvalidNodeId;
}

void
_RemapNodeIds(
    const Bsdf::ClosureTree& source,
    Bsdf::NodeId offset,
    Bsdf::NodeData* data)
{
    if (auto* mix = std::get_if<Bsdf::MixData>(data)) {
        mix->fg = _RemapNodeId(source, mix->fg, offset);
        mix->bg = _RemapNodeId(source, mix->bg, offset);
    } else if (auto* layer = std::get_if<Bsdf::LayerData>(data)) {
        layer->top = _RemapNodeId(source, layer->top, offset);
        layer->base = _RemapNodeId(source, layer->base, offset);
    } else if (auto* add = std::get_if<Bsdf::AddData>(data)) {
        add->in1 = _RemapNodeId(source, add->in1, offset);
        add->in2 = _RemapNodeId(source, add->in2, offset);
    } else if (auto* multiply = std::get_if<Bsdf::MultiplyData>(data)) {
        multiply->input = _RemapNodeId(source, multiply->input, offset);
    }
}

Bsdf::NodeId
_AppendClosureTree(
    Bsdf::ClosureTree* target,
    const Bsdf::ClosureTree& source)
{
    if (!target || source.Empty()) {
        return Bsdf::InvalidNodeId;
    }

    const Bsdf::NodeId offset =
        static_cast<Bsdf::NodeId>(target->nodes.size());
    target->nodes.reserve(target->nodes.size() + source.nodes.size());

    for (const Bsdf::Node& node : source.nodes) {
        Bsdf::NodeData data = node.data;
        _RemapNodeIds(source, offset, &data);
        target->nodes.push_back(Bsdf::Node{std::move(data)});
    }

    return _RemapNodeId(source, source.root, offset);
}

BsdfClosure
_MakeLayerClosure(
    const BsdfClosure& topClosure,
    const BsdfClosure& baseClosure)
{
    BsdfClosure closure;
    const auto applyInteriorMedium = [&]() {
        if (baseClosure.hasInteriorMedium) {
            closure.hasInteriorMedium = true;
            closure.interiorMedium = baseClosure.interiorMedium;
        } else if (topClosure.hasInteriorMedium) {
            closure.hasInteriorMedium = true;
            closure.interiorMedium = topClosure.interiorMedium;
        }
    };

    const Bsdf::NodeId top =
        _AppendClosureTree(&closure.tree, topClosure.tree);
    const Bsdf::NodeId base =
        _AppendClosureTree(&closure.tree, baseClosure.tree);

    if (!closure.tree.IsValid(top)) {
        closure.tree.root = base;
        applyInteriorMedium();
        return closure;
    }
    if (!closure.tree.IsValid(base)) {
        closure.tree.root = top;
        applyInteriorMedium();
        return closure;
    }

    Bsdf::LayerData layer;
    layer.top = top;
    layer.base = base;
    closure.tree.root = closure.tree.Add(layer);
    applyInteriorMedium();
    return closure;
}

BsdfClosure
_MakeLayerClosure(
    const BsdfClosure& topClosure,
    const VdfClosure& baseClosure)
{
    BsdfClosure closure;
    closure.tree = topClosure.tree;
    closure.hasInteriorMedium = topClosure.hasInteriorMedium;
    closure.interiorMedium = topClosure.interiorMedium;
    if (!baseClosure.medium.IsVacuum()) {
        closure.hasInteriorMedium = true;
        closure.interiorMedium = baseClosure.medium;
    }
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
_EvalAbsorptionVdf(
    const ParamMap& inputs,
    const ShadingContext&,
    NodeOutputMap* outputs)
{
    VdfClosure closure;
    closure.medium.sigmaA = Get<Vec3f>(inputs, _kAbsorption, Vec3f(0.0f));
    (*outputs)[_kOut] = Value(closure);
}

void
_EvalAnisotropicVdf(
    const ParamMap& inputs,
    const ShadingContext&,
    NodeOutputMap* outputs)
{
    VdfClosure closure;
    closure.medium.sigmaA = Get<Vec3f>(inputs, _kAbsorption, Vec3f(0.0f));
    closure.medium.sigmaS = Get<Vec3f>(inputs, _kScattering, Vec3f(0.0f));
    closure.medium.anisotropy = Get<float>(inputs, _kAnisotropy, 0.0f);
    (*outputs)[_kOut] = Value(closure);
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
_EvalDielectricBsdf(
    const ParamMap& inputs,
    const ShadingContext&,
    NodeOutputMap* outputs)
{
    Bsdf::DielectricData data;
    data.weight = Get<float>(inputs, _kWeight, 1.0f);
    data.tint = Get<Vec3f>(inputs, _kTint, Vec3f(1.0f));
    data.ior = Get<float>(inputs, _kIor, 1.5f);
    data.roughness = Get<Vec2f>(inputs, _kRoughness, Vec2f(0.05f, 0.05f));
    data.retroreflective = Get<bool>(inputs, _kRetroreflective, false);
    data.thinFilmThickness = Get<float>(inputs, _kThinfilmThickness, 0.0f);
    data.thinFilmIor = Get<float>(inputs, _kThinfilmIor, 1.5f);
    data.tangent = Get<Vec3f>(inputs, _kTangent, Vec3f(1.0f, 0.0f, 0.0f));
    data.scatterMode = _GetScatterMode(inputs);

    if (inputs.Find(_kNormal)) {
        data.normal = Get<Vec3f>(inputs, _kNormal, Vec3f(0.0f, 0.0f, 1.0f));
        data.hasShadingNormal = true;
    }

    (*outputs)[_kOut] = Value(_MakeClosure(data));
}

void
_EvalLayerBsdf(
    const ParamMap& inputs,
    const ShadingContext&,
    NodeOutputMap* outputs)
{
    const BsdfClosure top = Get<BsdfClosure>(inputs, _kTop, BsdfClosure{});
    const BsdfClosure base = Get<BsdfClosure>(inputs, _kBase, BsdfClosure{});
    (*outputs)[_kOut] = Value(_MakeLayerClosure(top, base));
}

void
_EvalLayerVdf(
    const ParamMap& inputs,
    const ShadingContext&,
    NodeOutputMap* outputs)
{
    const BsdfClosure top = Get<BsdfClosure>(inputs, _kTop, BsdfClosure{});
    const VdfClosure base = Get<VdfClosure>(inputs, _kBase, VdfClosure{});
    (*outputs)[_kOut] = Value(_MakeLayerClosure(top, base));
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
    _REG("ND_absorption_vdf", &_EvalAbsorptionVdf);
    _REG("ND_anisotropic_vdf", &_EvalAnisotropicVdf);
    _REG("ND_sheen_bsdf", &_EvalSheenBsdf);
    _REG("ND_dielectric_bsdf", &_EvalDielectricBsdf);
    _REG("ND_layer_bsdf", &_EvalLayerBsdf);
    _REG("ND_layer_vdf", &_EvalLayerVdf);
    _REG("ND_chiang_hair_bsdf", &_EvalChiangHairBsdf);
}

#undef _REG

}  // namespace mxcpp
