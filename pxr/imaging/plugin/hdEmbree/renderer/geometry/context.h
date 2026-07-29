//
// Copyright 2016 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_CONTEXT_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_CONTEXT_H

#include "primvarSampler.h"

#include <renderer/lights/lightLinking.h>
#include <renderer/materials/material.h>
#include <renderer/materials/materialEvalContext.h>
#include <renderer/materials/MaterialXCpp/value.h>

#include "pxr/base/gf/matrix4f.h"
#include "pxr/base/vt/array.h"
#include "pxr/imaging/hd/enums.h"
#include "pxr/pxr.h"

#include <embree4/rtcore.h>

#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE
namespace ty {

/// Reserved ray ID used only by owner-prototype SSS boundary queries. Such
/// rays must see both sides of the closed surface regardless of display cull
/// style, while ordinary renderer rays always use ID zero.
constexpr unsigned int FaceCullBypassRayId = 0x48444543u;

/// Surface display mode selected by the active Hydra mesh representation.
enum class WireframeMode
{
    disabled,
    edgeOnly,
    edgeOnSurface
};


/// \class PrototypeContext
///
/// A small bit of state attached to each bit of prototype geometry in embree,
/// for renderer integrators and geometric AOV evaluation.
///
struct PrototypeContext
{
    int32_t primId = 0;
    HdCullStyle cullStyle = HdCullStyleDontCare;
    bool doubleSided = false;
    bool refined = false;
    WireframeMode wireframeMode =
        WireframeMode::disabled;
    bool blendWireframeColor = true;
    float wireframeLineWidth = 0.0f;
    /// Coarse-face layout and live Embree edge levels used to reconstruct the
    /// final diced subdivision grid at a hit. Offsets has face-count + 1
    /// entries; subdivisionLevels remains owned by HdEmbreeMesh.
    VtIntArray faceVertexCounts;
    std::vector<size_t> faceVertexOffsets;
    std::vector<float> const* subdivisionLevels = nullptr;
    /// Whether the active repr and display style permit custom displacement.
    bool displacementEnabled = true;
    bool displaced = false;
    /// Deduplicates callback-boundary diagnostics across Embree worker
    /// invocations during one prototype commit.
    std::atomic<bool> displacementExceptionReported{false};
    /// Converts Embree's subdivision winding to the authored USD orientation.
    /// Always exactly -1 or +1. Coarse triangles are already reordered by
    /// HdMeshUtil and stay +1.
    float orientationSign = 1.0f;
    /// Renderer-owned material evaluation services. Geometry callbacks borrow
    /// this state; it is updated only while rendering is stopped and remains
    /// read-only throughout geometry commits and rendering.
    MaterialEvalServices const* materialEvalServices = nullptr;
    /// Prototype-level transforms used while Embree evaluates displacement.
    /// Per-instance transforms are unavailable when a shared prototype scene
    /// is committed, so these default to object-space identity.
    GfMatrix4f displacementObjectToWorldMatrix = GfMatrix4f(1.0f);
    GfMatrix4f displacementWorldToObjectMatrix = GfMatrix4f(1.0f);
    VtVec3fArray const* triangleDPdu = nullptr;
    VtVec3fArray const* triangleDPdv = nullptr;
    /// Name-indexed owning storage for primvar samplers.
    std::unordered_map<
        TfToken,
        std::unique_ptr<PrimvarSampler>,
        TfToken::HashFunctor> primvarMap;
    /// A copy of the primitive params for this rprim.
    VtIntArray primitiveParams;
    /// The bound material, or nullptr if none.
    MaterialData const* material = nullptr;
    /// Handle-indexed against material->geomPropNames. Both vectors are
    /// rebuilt after every material-table or primvarMap mutation; sampler
    /// pointers observe the unique_ptr-owned entries in primvarMap.
    std::vector<PrimvarSampler*> geomPropSamplers;
    std::vector<mxcpp::Value> geomPropUniformValues;
};

///
/// \class InstanceContext
///
/// A small bit of state attached to each bit of instanced geometry in embree,
/// for renderer integrators and geometric AOV evaluation.
///
struct InstanceContext
{
    /// The object-to-world transform, for transforming normals to worldspace.
    GfMatrix4f objectToWorldMatrix;
    /// The inverse world-to-object transform.
    GfMatrix4f worldToObjectMatrix;
    /// The scene the prototype geometry lives in, for passing to
    /// rtcInterpolate.
    RTCScene rootScene;
    /// The instance id of this instance.
    int32_t instanceId;
    /// Resolved Hydra light- and shadow-link category memberships.
    CategorySet categories;
};


} // namespace ty
PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_IMAGING_PLUGIN_HD_EMBREE_CONTEXT_H
