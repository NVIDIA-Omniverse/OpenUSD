//
// MaterialXCpp surface-shader helpers.
//
#ifndef MXCPP_SURFACE_SHADER_UTILS_H
#define MXCPP_SURFACE_SHADER_UTILS_H

#include "paramMap.h"
#include "surfaceClosure.h"

#include <algorithm>
#include <utility>
#include <variant>

namespace mxcpp {

inline Vec3f
SaturateVec(const Vec3f& v)
{
    return Vec3f(
        std::clamp(v[0], 0.0f, 1.0f),
        std::clamp(v[1], 0.0f, 1.0f),
        std::clamp(v[2], 0.0f, 1.0f));
}

inline Vec3f
MixVec(const Vec3f& bg, const Vec3f& fg, float mix)
{
    return bg + (fg - bg) * mix;
}

inline float
MixFloat(float bg, float fg, float mix)
{
    return bg + (fg - bg) * mix;
}

inline void
ClearSurfaceLegacySummary(SurfaceClosure* closure)
{
    if (!closure) {
        return;
    }

    closure->baseColor = Vec3f(0.0f);
    closure->metallic = 0.0f;
    closure->specular = 0.0f;
    closure->specularColor = Vec3f(0.0f);
    closure->transmission = 0.0f;
    closure->transmissionColor = Vec3f(0.0f);
    closure->coat = 0.0f;
    closure->sheen = 0.0f;
    closure->subsurfaceWeight = 0.0f;
}

inline SurfaceClosure
MakeEmptySurfaceClosure()
{
    SurfaceClosure closure;
    ClearSurfaceLegacySummary(&closure);
    closure.emissiveColor = Vec3f(0.0f);
    closure.bsdfTree.Clear();
    closure.hasInteriorMedium = false;
    closure.interiorMedium = MediumProperties{};
    closure.hasPrecomputedSubsurfaceMedium = false;
    closure.precomputedSubsurfaceMedium = MediumProperties{};
    return closure;
}

inline SurfaceClosure
MakeUnlitSurfaceClosure(const Vec3f& emissionColor, float opacity = 1.0f)
{
    SurfaceClosure closure = MakeEmptySurfaceClosure();
    closure.emissiveColor = emissionColor;
    closure.opacity = std::clamp(opacity, 0.0f, 1.0f);
    closure.presence = closure.opacity;
    return closure;
}

inline bool
ApplySubsurfaceSummaryFromTree(
    const Bsdf::ClosureTree& tree,
    Bsdf::NodeId nodeId,
    SurfaceClosure* closure)
{
    const Bsdf::Node* node = tree.Get(nodeId);
    if (!node || !closure) {
        return false;
    }

    if (const auto* subsurface =
            std::get_if<Bsdf::SubsurfaceData>(&node->data)) {
        closure->subsurfaceWeight = std::clamp(
            subsurface->weight, 0.0f, 1.0f);
        closure->subsurfaceColor = SaturateVec(subsurface->color);
        closure->subsurfaceRadius = subsurface->radius;
        closure->subsurfaceRadiusScale = Vec3f(1.0f);
        closure->subsurfaceAnisotropy = subsurface->anisotropy;
        return closure->HasSubsurfaceScattering();
    }

    if (const auto* mix = std::get_if<Bsdf::MixData>(&node->data)) {
        return ApplySubsurfaceSummaryFromTree(tree, mix->fg, closure) ||
               ApplySubsurfaceSummaryFromTree(tree, mix->bg, closure);
    }
    if (const auto* layer = std::get_if<Bsdf::LayerData>(&node->data)) {
        return ApplySubsurfaceSummaryFromTree(tree, layer->top, closure) ||
               ApplySubsurfaceSummaryFromTree(tree, layer->base, closure);
    }
    if (const auto* add = std::get_if<Bsdf::AddData>(&node->data)) {
        return ApplySubsurfaceSummaryFromTree(tree, add->in1, closure) ||
               ApplySubsurfaceSummaryFromTree(tree, add->in2, closure);
    }
    if (const auto* multiply = std::get_if<Bsdf::MultiplyData>(&node->data)) {
        return ApplySubsurfaceSummaryFromTree(tree, multiply->input, closure);
    }

    return false;
}

inline Bsdf::NodeId
RemapClosureNodeId(
    const Bsdf::ClosureTree& source,
    Bsdf::NodeId id,
    Bsdf::NodeId offset)
{
    return source.IsValid(id) ? id + offset : Bsdf::InvalidNodeId;
}

inline void
RemapClosureNodeIds(
    const Bsdf::ClosureTree& source,
    Bsdf::NodeId offset,
    Bsdf::NodeData* data)
{
    if (auto* mix = std::get_if<Bsdf::MixData>(data)) {
        mix->fg = RemapClosureNodeId(source, mix->fg, offset);
        mix->bg = RemapClosureNodeId(source, mix->bg, offset);
    } else if (auto* layer = std::get_if<Bsdf::LayerData>(data)) {
        layer->top = RemapClosureNodeId(source, layer->top, offset);
        layer->base = RemapClosureNodeId(source, layer->base, offset);
    } else if (auto* add = std::get_if<Bsdf::AddData>(data)) {
        add->in1 = RemapClosureNodeId(source, add->in1, offset);
        add->in2 = RemapClosureNodeId(source, add->in2, offset);
    } else if (auto* multiply = std::get_if<Bsdf::MultiplyData>(data)) {
        multiply->input = RemapClosureNodeId(source, multiply->input, offset);
    }
}

inline Bsdf::NodeId
AppendClosureTree(
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
        RemapClosureNodeIds(source, offset, &data);
        target->nodes.push_back(Bsdf::Node{std::move(data)});
    }

    return RemapClosureNodeId(source, source.root, offset);
}

inline Bsdf::NodeId
WeightClosureTreeRoot(
    Bsdf::ClosureTree* target,
    Bsdf::NodeId root,
    float weight)
{
    if (!target || !target->IsValid(root)) {
        return Bsdf::InvalidNodeId;
    }

    if (weight == 1.0f) {
        return root;
    }

    Bsdf::MultiplyData multiply;
    multiply.input = root;
    multiply.weight = Vec3f(weight);
    return target->Add(multiply);
}

inline SurfaceClosure
EvalSurfaceConstructor(const ParamMap& params)
{
    static const SlotName bsdf("bsdf");
    static const SlotName edf("edf");
    static const SlotName opacity("opacity");
    static const SlotName thinWalled("thin_walled");

    SurfaceClosure closure = MakeEmptySurfaceClosure();

    const UniformEdf uniformEdf = Get<UniformEdf>(
        params, edf, UniformEdf{Vec3f(0.0f)});
    const BsdfClosure bsdfClosure = Get<BsdfClosure>(
        params, bsdf, BsdfClosure{});

    closure.bsdfTree = bsdfClosure.tree;
    closure.hasInteriorMedium = bsdfClosure.hasInteriorMedium &&
        !bsdfClosure.interiorMedium.IsVacuum();
    closure.interiorMedium = bsdfClosure.interiorMedium;
    ApplySubsurfaceSummaryFromTree(
        closure.bsdfTree, closure.bsdfTree.root, &closure);
    closure.emissiveColor = uniformEdf.emittance;
    closure.presence = std::clamp(
        Get<float>(params, opacity, 1.0f), 0.0f, 1.0f);
    closure.thinWalled = Get<bool>(params, thinWalled, false);
    if (closure.thinWalled) {
        closure.hasInteriorMedium = false;
        closure.interiorMedium = MediumProperties{};
    }
    return closure;
}

inline SurfaceClosure
MixSurfaceClosures(
    const SurfaceClosure& bg,
    const SurfaceClosure& fg,
    float mix)
{
    SurfaceClosure result = MakeEmptySurfaceClosure();

    result.baseColor = MixVec(bg.baseColor, fg.baseColor, mix);
    result.roughness = MixFloat(bg.roughness, fg.roughness, mix);
    result.metallic = MixFloat(bg.metallic, fg.metallic, mix);
    result.specular = MixFloat(bg.specular, fg.specular, mix);
    result.specularIor = MixFloat(bg.specularIor, fg.specularIor, mix);
    result.specularColor = MixVec(bg.specularColor, fg.specularColor, mix);
    result.emissiveColor = MixVec(bg.emissiveColor, fg.emissiveColor, mix);
    result.transmission = MixFloat(bg.transmission, fg.transmission, mix);
    result.transmissionColor =
        MixVec(bg.transmissionColor, fg.transmissionColor, mix);
    result.opacity = MixFloat(bg.opacity, fg.opacity, mix);
    result.presence = MixFloat(bg.presence, fg.presence, mix);
    result.coat = MixFloat(bg.coat, fg.coat, mix);
    result.coatRoughness =
        MixFloat(bg.coatRoughness, fg.coatRoughness, mix);
    result.coatIor = MixFloat(bg.coatIor, fg.coatIor, mix);
    result.sheen = MixFloat(bg.sheen, fg.sheen, mix);
    result.sheenColor = MixVec(bg.sheenColor, fg.sheenColor, mix);
    result.sheenRoughness =
        MixFloat(bg.sheenRoughness, fg.sheenRoughness, mix);
    result.normal = MixVec(bg.normal, fg.normal, mix);
    result.normalSpace =
        mix >= 0.5f ? fg.normalSpace : bg.normalSpace;
    result.thinWalled = mix >= 0.5f ? fg.thinWalled : bg.thinWalled;

    result.subsurfaceWeight =
        MixFloat(bg.subsurfaceWeight, fg.subsurfaceWeight, mix);
    result.subsurfaceColor =
        MixVec(bg.subsurfaceColor, fg.subsurfaceColor, mix);
    result.subsurfaceRadius =
        MixVec(bg.subsurfaceRadius, fg.subsurfaceRadius, mix);
    result.subsurfaceRadiusScale =
        MixVec(bg.subsurfaceRadiusScale, fg.subsurfaceRadiusScale, mix);
    result.subsurfaceAnisotropy =
        MixFloat(bg.subsurfaceAnisotropy, fg.subsurfaceAnisotropy, mix);

    const bool chooseFg = mix >= 0.5f;
    if (fg.hasInteriorMedium && (!bg.hasInteriorMedium || chooseFg)) {
        result.hasInteriorMedium = true;
        result.interiorMedium = fg.interiorMedium;
    } else if (bg.hasInteriorMedium) {
        result.hasInteriorMedium = true;
        result.interiorMedium = bg.interiorMedium;
    }

    if (fg.hasPrecomputedSubsurfaceMedium &&
        (!bg.hasPrecomputedSubsurfaceMedium || chooseFg)) {
        result.hasPrecomputedSubsurfaceMedium = true;
        result.precomputedSubsurfaceMedium =
            fg.precomputedSubsurfaceMedium;
    } else if (bg.hasPrecomputedSubsurfaceMedium) {
        result.hasPrecomputedSubsurfaceMedium = true;
        result.precomputedSubsurfaceMedium =
            bg.precomputedSubsurfaceMedium;
    }

    const Bsdf::NodeId bgRoot =
        AppendClosureTree(&result.bsdfTree, bg.bsdfTree);
    const Bsdf::NodeId fgRoot =
        AppendClosureTree(&result.bsdfTree, fg.bsdfTree);

    if (result.bsdfTree.IsValid(bgRoot) && result.bsdfTree.IsValid(fgRoot)) {
        Bsdf::MixData mixData;
        mixData.fg = fgRoot;
        mixData.bg = bgRoot;
        mixData.mix = mix;
        result.bsdfTree.root = result.bsdfTree.Add(mixData);
    } else if (result.bsdfTree.IsValid(fgRoot)) {
        result.bsdfTree.root =
            WeightClosureTreeRoot(&result.bsdfTree, fgRoot, mix);
    } else if (result.bsdfTree.IsValid(bgRoot)) {
        result.bsdfTree.root =
            WeightClosureTreeRoot(&result.bsdfTree, bgRoot, 1.0f - mix);
    }

    return result;
}

}  // namespace mxcpp

#endif  // MXCPP_SURFACE_SHADER_UTILS_H
