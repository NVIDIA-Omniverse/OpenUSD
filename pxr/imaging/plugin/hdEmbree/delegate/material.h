//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_MATERIAL_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_MATERIAL_H

#include <renderer/materials/material.h>
#include <renderer/materials/MaterialXCpp/graph.h>

#include "pxr/imaging/hd/material.h"
#include "pxr/pxr.h"

#include <memory>

PXR_NAMESPACE_OPEN_SCOPE

/// \class HdEmbreeMaterial
///
/// A material Sprim that compiles an HdMaterialNetwork2 into an
/// mxcpp::EvalGraph for CPU-side shading in the Embree renderer.
///
class HdEmbreeMaterial final : public HdMaterial
{
public:
    HdEmbreeMaterial(SdfPath const& id);
    ~HdEmbreeMaterial() override;

    void Sync(HdSceneDelegate *sceneDelegate,
              HdRenderParam   *renderParam,
              HdDirtyBits     *dirtyBits) override;

    void Finalize(HdRenderParam *renderParam) override;

    HdDirtyBits GetInitialDirtyBitsMask() const override;

    /// Recompile the material after graph-affecting render settings changed.
    void ResyncForRenderSettingsChange(HdRenderParam *renderParam);

    /// Return the compiled evaluation graph, or nullptr if unavailable.
    ty::MaterialData const* GetRenderMaterial() const { return &_renderMaterial; }

private:
    // Non-owning; cached from the last Hydra Sync for direct recompile when
    // render-setting context priority changes outside normal dirty tracking.
    HdSceneDelegate *_sceneDelegate = nullptr;
    std::unique_ptr<mxcpp::EvalGraph> _surfaceGraph;
    std::unique_ptr<mxcpp::EvalGraph> _displacementGraph;
    ty::MaterialData _renderMaterial;
};

PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_IMAGING_PLUGIN_HD_EMBREE_MATERIAL_H
