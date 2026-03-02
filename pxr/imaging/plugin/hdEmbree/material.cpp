//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/imaging/plugin/hdEmbree/material.h"
#include "pxr/imaging/plugin/hdEmbree/renderParam.h"

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

    if (!(*dirtyBits & HdMaterial::AllDirty)) {
        *dirtyBits = HdMaterial::Clean;
        return;
    }

    // Stop the render thread before touching _evalGraph so the renderer
    // does not read a half-destroyed graph.  Bumping the scene version
    // also tells the render pass to restart accumulation.
    if (auto *embreeParam =
            dynamic_cast<HdEmbreeRenderParam *>(renderParam)) {
        embreeParam->AcquireSceneForEdit();
    }

    SdfPath const& id = GetId();

    _evalGraph.reset();

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
            _evalGraph = MxLiteEvalGraph::Compile(network);
            if (_evalGraph && !_evalGraph->IsValid()) {
                _evalGraph.reset();
            }
        } catch (...) {
            TF_WARN("HdEmbreeMaterial: exception compiling graph for %s",
                    id.GetText());
            _evalGraph.reset();
        }
    }

    *dirtyBits = HdMaterial::Clean;
}

PXR_NAMESPACE_CLOSE_SCOPE
