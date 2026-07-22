//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/imaging/plugin/hdEmbree/delegate/material.h"
#include "pxr/imaging/plugin/hdEmbree/renderer/materials/mxcppAdapter.h"
#include "pxr/imaging/plugin/hdEmbree/delegate/renderParam.h"

#include "pxr/imaging/hd/sceneDelegate.h"
#include "pxr/base/tf/diagnostic.h"

PXR_NAMESPACE_OPEN_SCOPE

HdEmbreeMaterial::HdEmbreeMaterial(SdfPath const& id)
    : HdMaterial(id)
{
}

HdEmbreeMaterial::~HdEmbreeMaterial() = default;

HdDirtyBits
HdEmbreeMaterial::GetInitialDirtyBitsMask() const
{
    return HdMaterial::AllDirty;
}

void
HdEmbreeMaterial::Sync(HdSceneDelegate *sceneDelegate,
                       HdRenderParam   *renderParam,
                       HdDirtyBits     *dirtyBits)
{
    HD_TRACE_FUNCTION();

    _sceneDelegate = sceneDelegate;

    if (!(*dirtyBits & HdMaterial::AllDirty)) {
        *dirtyBits = HdMaterial::Clean;
        return;
    }

    if (renderParam) {
        // The render thread reads the compiled graph during shading, so stop
        // the current render and bump the scene version before replacing it.
        static_cast<HdEmbreeRenderParam*>(renderParam)
            ->NotifyDisplacementChange();
    }

    SdfPath const& id = GetId();

    _evalGraph.reset();
    _displacementGraph.reset();
    _renderMaterial.evalGraph = nullptr;
    _renderMaterial.displacementGraph = nullptr;

    VtValue networkMapValue;
    try {
        networkMapValue = sceneDelegate->GetMaterialResource(id);
    } catch (...) {
        TF_WARN("HdEmbreeMaterial: exception in GetMaterialResource for %s",
                id.GetText());
        *dirtyBits = HdMaterial::Clean;
        return;
    }

    if (networkMapValue.IsEmpty()) {
        *dirtyBits = HdMaterial::Clean;
        return;
    }

    HdMaterialNetwork2 network;
    bool haveNetwork = false;

    if (networkMapValue.IsHolding<HdMaterialNetwork2>()) {
        network = networkMapValue.UncheckedGet<HdMaterialNetwork2>();
        haveNetwork = true;
    } else if (networkMapValue.IsHolding<HdMaterialNetworkMap>()) {
        const auto& networkMap =
            networkMapValue.UncheckedGet<HdMaterialNetworkMap>();
        network = HdConvertToHdMaterialNetwork2(networkMap);
        haveNetwork = true;
    } else {
        TF_WARN("HdEmbreeMaterial: unsupported material resource type '%s' "
                "for %s", networkMapValue.GetTypeName().c_str(),
                id.GetText());
    }

    if (haveNetwork) {
        try {
            // Surface shading runs at ray hits, but Embree requests
            // displacement while committing subdivision geometry. Compile the
            // terminals independently so either consumer can run without
            // evaluating the other (and either terminal may be absent).
            auto mxcppGraph = ConvertHdNetworkToMxcppGraph(network);
            _evalGraph = mxcpp::EvalGraph::Compile(mxcppGraph);
            if (_evalGraph && !_evalGraph->IsValid()) {
                _evalGraph.reset();
            }
            _displacementGraph =
                mxcpp::EvalGraph::Compile(mxcppGraph, "displacement");
            if (_displacementGraph && !_displacementGraph->IsValid()) {
                _displacementGraph.reset();
            }
        } catch (...) {
            TF_WARN("HdEmbreeMaterial: exception compiling graph for %s",
                    id.GetText());
            _evalGraph.reset();
            _displacementGraph.reset();
        }
    }

    // Mesh prototype contexts retain this material handle, so keep the
    // handle stable and replace only the graphs it points at.
    _renderMaterial.evalGraph = _evalGraph.get();
    _renderMaterial.displacementGraph = _displacementGraph.get();
    *dirtyBits = HdMaterial::Clean;
}

void
HdEmbreeMaterial::Finalize(HdRenderParam *renderParam)
{
    // Render worker threads dereference this material (and its compiled
    // eval graph) through the stable renderer handle held in prototype contexts, so the
    // render must be stopped before the render index deletes this sprim.
    // Bumping the scene version guarantees the render pass restarts only
    // after the affected rprims have re-synced their material bindings.
    if (renderParam) {
        static_cast<HdEmbreeRenderParam*>(renderParam)
            ->NotifyDisplacementChange();
    }
}

void
HdEmbreeMaterial::ResyncForRenderContextChange(HdRenderParam *renderParam)
{
    if (!_sceneDelegate) {
        return;
    }

    HdDirtyBits dirtyBits = HdMaterial::DirtyResource;
    Sync(_sceneDelegate, renderParam, &dirtyBits);
}

PXR_NAMESPACE_CLOSE_SCOPE
