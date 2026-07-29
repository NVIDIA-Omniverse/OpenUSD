//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
// Transport classification of compiled material closures.
//
#include "closureClassification.h"

#include <renderer/materials/MaterialXCpp/materials/adobeOpenPbr.h>
#include <renderer/materials/MaterialXCpp/materials/bsdf.h>

#include <variant>

PXR_NAMESPACE_OPEN_SCOPE

static constexpr float _reflectionOnlyEps = 1.0e-6f;

static bool
_IsEffectivelyZero(float value)
{
    return value <= _reflectionOnlyEps;
}

static bool
_IsEffectivelyOpaque(float value)
{
    return value >= 1.0f - _reflectionOnlyEps;
}


static bool
_IsReflectionOnlyNode(
    mxcpp::Bsdf::ClosureTree const& tree,
    mxcpp::Bsdf::NodeId nodeId)
{
    const mxcpp::Bsdf::Node* const node = tree.Get(nodeId);
    if (!node) {
        return false;
    }

    const mxcpp::Bsdf::NodeData& data = node->data;

    // Whole OpenPBR closures are reflection-only when transport exits are off.
    if (const mxcpp::Bsdf::AdobeOpenPbrData* const openPbr =
            std::get_if<mxcpp::Bsdf::AdobeOpenPbrData>(&data)) {
        return _IsEffectivelyOpaque(openPbr->geometryOpacity) &&
               _IsEffectivelyZero(openPbr->transmissionWeight) &&
               _IsEffectivelyZero(openPbr->subsurfaceWeight);
    }

    // Pure reflection lobes are always reflection-only.
    if (std::get_if<mxcpp::Bsdf::OrenNayarDiffuseData>(&data) ||
        std::get_if<mxcpp::Bsdf::BurleyDiffuseData>(&data) ||
        std::get_if<mxcpp::Bsdf::ConductorData>(&data) ||
        std::get_if<mxcpp::Bsdf::SheenData>(&data)) {
        return true;
    }

    // Dielectrics are reflection-only when disabled or authored that way.
    if (const mxcpp::Bsdf::DielectricData* const dielectric =
            std::get_if<mxcpp::Bsdf::DielectricData>(&data)) {
        return _IsEffectivelyZero(dielectric->weight) ||
               dielectric->scatterMode == mxcpp::Bsdf::ScatterMode::Reflection;
    }

    // Coupled dielectric interfaces are reflection-only without transmission.
    if (const mxcpp::Bsdf::DielectricInterfaceData* const dielectricInterface =
            std::get_if<mxcpp::Bsdf::DielectricInterfaceData>(&data)) {
        return _IsEffectivelyZero(dielectricInterface->transmissionWeight);
    }

    // Schlick lobes are reflection-only when disabled or authored that way.
    if (const mxcpp::Bsdf::GeneralizedSchlickData* const schlick =
            std::get_if<mxcpp::Bsdf::GeneralizedSchlickData>(&data)) {
        return _IsEffectivelyZero(schlick->weight) ||
               schlick->scatterMode == mxcpp::Bsdf::ScatterMode::Reflection;
    }

    // Translucent lobes classify as reflection-only when disabled.
    if (const mxcpp::Bsdf::TranslucentData* const translucent =
            std::get_if<mxcpp::Bsdf::TranslucentData>(&data)) {
        return _IsEffectivelyZero(translucent->weight);
    }

    // Subsurface lobes classify as reflection-only when disabled.
    if (const mxcpp::Bsdf::SubsurfaceData* const subsurface =
            std::get_if<mxcpp::Bsdf::SubsurfaceData>(&data)) {
        return _IsEffectivelyZero(subsurface->weight);
    }

    // Endpoint mixes classify the surviving branch; others require both.
    if (const mxcpp::Bsdf::MixData* const mix =
            std::get_if<mxcpp::Bsdf::MixData>(&data)) {
        if (_IsEffectivelyZero(mix->mix)) {
            return _IsReflectionOnlyNode(tree, mix->bg);
        }
        if (_IsEffectivelyOpaque(mix->mix)) {
            return _IsReflectionOnlyNode(tree, mix->fg);
        }
        return _IsReflectionOnlyNode(tree, mix->fg) &&
               _IsReflectionOnlyNode(tree, mix->bg);
    }

    // A layer is reflection-only when both children are reflection-only.
    if (const mxcpp::Bsdf::LayerData* const layer =
            std::get_if<mxcpp::Bsdf::LayerData>(&data)) {
        return _IsReflectionOnlyNode(tree, layer->top) &&
               _IsReflectionOnlyNode(tree, layer->base);
    }

    // An add is reflection-only when both inputs are reflection-only.
    if (const mxcpp::Bsdf::AddData* const add =
            std::get_if<mxcpp::Bsdf::AddData>(&data)) {
        return _IsReflectionOnlyNode(tree, add->in1) &&
               _IsReflectionOnlyNode(tree, add->in2);
    }

    // Multiplication preserves its input classification.
    if (const mxcpp::Bsdf::MultiplyData* const multiply =
            std::get_if<mxcpp::Bsdf::MultiplyData>(&data)) {
        return _IsReflectionOnlyNode(tree, multiply->input);
    }

    static_assert(
        std::variant_size_v<mxcpp::Bsdf::NodeData> == 15,
        "A closure node kind was added: classify it above.");
    return false;
}


bool
ty::IsReflectionOnlyClosure(mxcpp::SurfaceClosure const& closure)
{
    if (!_IsEffectivelyOpaque(closure.presence) ||
        !_IsEffectivelyOpaque(closure.opacity) ||
        closure.HasSubsurfaceScattering()) {
        return false;
    }

    if (closure.HasBsdfTree()) {
        return _IsReflectionOnlyNode(closure.bsdfTree, closure.bsdfTree.root);
    }

    return _IsEffectivelyZero(closure.transmission) &&
           _IsEffectivelyZero(closure.subsurfaceWeight);
}

bool
ty::IsVolumeOnlyBoundary(mxcpp::SurfaceClosure const& closure)
{
    return closure.isVolumeBoundary &&
           !closure.HasBsdfTree() &&
           _IsEffectivelyZero(closure.opacity);
}

PXR_NAMESPACE_CLOSE_SCOPE
