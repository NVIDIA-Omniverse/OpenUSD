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

    /// Compile dirty surface and optional displacement terminals independently.
    ///
    /// sceneDelegate and dirtyBits must be non-null. renderParam may be null.
    /// Clean input clears dirtyBits and leaves the stable MaterialData handle
    /// unchanged. Dirty input with a renderParam stops rendering and publishes
    /// scene/material versions before replacing graph contents. Authored
    /// malformed graph compilation produces CompileResult diagnostics without
    /// throwing; only the external GetMaterialResource call is caught as an
    /// exception boundary. An absent displacement terminal is valid and leaves
    /// displacementGraph null, while an absent or invalid surface leaves
    /// surfaceGraph null.
    ///
    /// The MaterialData address remains stable. Mesh prototype contexts observe
    /// it and refresh handle-indexed geomprop bindings after the published
    /// material version is seen. dirtyBits is clean on every return.
    void Sync(HdSceneDelegate *sceneDelegate,
              HdRenderParam   *renderParam,
              HdDirtyBits     *dirtyBits) override;

    /// With a non-null renderParam, stop rendering and publish material
    /// invalidation before Hydra destroys the stable handle observed by mesh
    /// prototype contexts.
    void Finalize(HdRenderParam *renderParam) override;

    HdDirtyBits GetInitialDirtyBitsMask() const override;

    /// Recompile the cached delegate resource after graph-affecting render
    /// settings changed. Does nothing before the first Sync; otherwise has the
    /// same publication and failure behavior as Sync.
    void ResyncForRenderSettingsChange(HdRenderParam *renderParam);

    /// Return the stable renderer material handle. Individual terminal graph
    /// pointers may be null when absent or invalid.
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
