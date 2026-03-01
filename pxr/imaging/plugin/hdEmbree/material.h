//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_MATERIAL_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_MATERIAL_H

#include "pxr/pxr.h"
#include "pxr/imaging/hd/material.h"
#include "pxr/imaging/plugin/hdEmbree/mxLite/graph.h"

#include <memory>

PXR_NAMESPACE_OPEN_SCOPE

/// \class HdEmbreeMaterial
///
/// A material Sprim that compiles an HdMaterialNetwork2 into an
/// MxLiteEvalGraph for CPU-side shading in the Embree renderer.
///
class HdEmbreeMaterial final : public HdMaterial
{
public:
    HdEmbreeMaterial(SdfPath const& id);
    ~HdEmbreeMaterial() override;

    void Sync(HdSceneDelegate *sceneDelegate,
              HdRenderParam   *renderParam,
              HdDirtyBits     *dirtyBits) override;

    HdDirtyBits GetInitialDirtyBitsMask() const override;

    /// Return the compiled evaluation graph, or nullptr if unavailable.
    MxLiteEvalGraph* GetEvalGraph() const { return _evalGraph.get(); }

private:
    std::unique_ptr<MxLiteEvalGraph> _evalGraph;
};

PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_IMAGING_PLUGIN_HD_EMBREE_MATERIAL_H
