//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_BASIS_CURVES_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_BASIS_CURVES_H

#include <renderer/api.h>

#include "pxr/imaging/hd/basisCurves.h"
#include "pxr/pxr.h"

#include <cstddef>
#include <cstdint>
#include <memory>

PXR_NAMESPACE_OPEN_SCOPE

class HdEmbreeRenderParam;

namespace ty {
struct InstanceContext;
struct PrototypeContext;
} // namespace ty

/// Hydra BasisCurves adapter backed by an instanced Embree prototype scene.
///
/// One Rprim owns one prototype scene, zero or more representation-homogeneous
/// curve geometry records, and one top-level Embree instance per flattened
/// Hydra instance. An uninstanced Rprim owns one identity instance. Curve
/// records keep their context and shared-buffer storage alive for the complete
/// lifetime of the Embree geometry that borrows them.
class HdEmbreeBasisCurves final : public HdBasisCurves
{
public:
    HF_MALLOC_TAG_NEW("new HdEmbreeBasisCurves");

    explicit HdEmbreeBasisCurves(SdfPath const& id);
    ~HdEmbreeBasisCurves() override;

    HdDirtyBits GetInitialDirtyBitsMask() const override;

    void Sync(HdSceneDelegate* sceneDelegate,
              HdRenderParam* renderParam,
              HdDirtyBits* dirtyBits,
              TfToken const& reprToken) override;

    void Finalize(HdRenderParam* renderParam) override;

    /// Inject the renderer-owned object-space minimum diameter and its
    /// monotonic change epoch. There is deliberately no local default: before
    /// the first injection Sync publishes an empty prototype and a validation
    /// error. The render delegate injects the setting before registering a
    /// factory-created Rprim.
    void SetMinimumWidth(float minimumWidth, std::uint64_t epoch);

    /// Apply a new renderer-owned minimum diameter and synchronously rebuild
    /// this Rprim from cached validated input. No SceneDelegate data is read.
    /// Rendering must already be stopped by the caller.
    bool RebuildForMinimumWidth(
        float minimumWidth,
        std::uint64_t epoch,
        HdEmbreeRenderParam* renderParam);

    /// Rebuild material geomprop observer tables after a material recompiles.
    /// Rendering must already be stopped by the caller.
    void RefreshMaterialBindings();

    // Narrow inspection API used by focused internal lifecycle tests. These
    // accessors do not transfer ownership and are valid only while rendering
    // and scene mutation are stopped.
    size_t GetCurveGeometryRecordCount() const noexcept;
    size_t GetInstanceCount() const noexcept;
    std::uint64_t GetGeometryGeneration() const noexcept;
    /// Return one record vertex's object-space radius, or -1 for an invalid
    /// record/vertex index.
    float GetCurveGeometryRadius(
        size_t recordIndex,
        size_t vertexIndex) const noexcept;
    ty::PrototypeContext const* GetPrototypeContext(
        size_t recordIndex) const noexcept;
    ty::InstanceContext const* GetInstanceContext(
        size_t instanceIndex) const noexcept;

protected:
    void _InitRepr(TfToken const& reprToken,
                   HdDirtyBits* dirtyBits) override;

    HdDirtyBits _PropagateDirtyBits(HdDirtyBits bits) const override;

private:
    struct _Impl;
    std::unique_ptr<_Impl> const _impl;

    HdEmbreeBasisCurves(HdEmbreeBasisCurves const&) = delete;
    HdEmbreeBasisCurves& operator=(HdEmbreeBasisCurves const&) = delete;
};

PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_IMAGING_PLUGIN_HD_EMBREE_BASIS_CURVES_H
