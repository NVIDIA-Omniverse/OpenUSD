//
// Copyright 2016 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_CONTEXT_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_CONTEXT_H

#include "curveGeometry.h"
#include "primvarSampler.h"

#include <renderer/embreeCompat.h>
#include <renderer/lights/lightLinking.h>
#include <renderer/materials/material.h>
#include <renderer/materials/materialEvalContext.h>
#include <renderer/materials/MaterialXCpp/value.h>

#include "pxr/base/gf/matrix4f.h"
#include "pxr/base/vt/array.h"
#include "pxr/imaging/hd/enums.h"
#include "pxr/pxr.h"

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

/// Triangle sampler layout cached for hit-time corner-normal fetches.
enum class TriangleCornerSamplerKind
{
    none,
    vertex,
    faceVarying
};

/// Surface family of the Embree prototype geometry owning this context.
/// Curve representation details stay in CurveGeometryRepresentation so they
/// cannot be inferred from mesh-only flags such as `refined`.
enum class GeometryKind
{
    triangleMesh,
    subdivisionMesh,
    roundCurve,
    orientedRibbon
};


/// \class PrototypeContext
///
/// Renderer state attached to Embree prototype geometry as user data.
///
/// The owning Rprim geometry record owns this object and every owning
/// container within it. Its address remains stable while the attached geometry
/// can be traversed. Observing pointers name state owned by that record or
/// Rprim, its bound material Sprim, or the Renderer. Sync may mutate this
/// context only after
/// HdEmbreeRenderParam::AcquireSceneForEdit() has stopped rendering; geometry
/// commit callbacks and render workers otherwise treat it as read-only. The
/// displacement callback may atomically set displacementExceptionReported.
///
struct PrototypeContext
{
    int32_t primId = 0;
    GeometryKind geometryKind = GeometryKind::triangleMesh;
    /// Record-wide curve buffer representation. This field is meaningful only
    /// for roundCurve and orientedRibbon contexts.
    CurveGeometryRepresentation curveRepresentation =
        CurveGeometryRepresentation::roundLinear;
    /// Record-local Embree primitive ID to authored curve/segment/U mapping.
    /// The owning curve record finishes this vector before binding its stable
    /// context address and never mutates it while Embree can traverse it.
    std::vector<CurveSegmentMetadata> curvePrimitiveMetadata;
    HdCullStyle cullStyle = HdCullStyleDontCare;
    bool doubleSided = false;
    bool refined = false;
    WireframeMode wireframeMode =
        WireframeMode::disabled;
    bool blendWireframeColor = true;
    float wireframeLineWidth = 0.0f;
    /// Coarse-face layout and live Embree edge levels used to reconstruct the
    /// final diced subdivision grid at a hit. Offsets has face-count + 1
    /// entries; its last entry is the sum of positive faceVertexCounts.
    /// subdivisionLevels observes HdEmbreeMesh storage and has one entry per
    /// coarse face edge while refined geometry is live.
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
    /// Observing pointers to mesh-owned object-space triangle derivatives.
    /// The arrays have one entry per triangulated primitive when populated.
    VtVec3fArray const* triangleDPdu = nullptr;
    VtVec3fArray const* triangleDPdv = nullptr;
    /// Mesh-owned coarse triangle topology and object-space points. These
    /// provide genuine barycentric corners; authored-st derivatives above
    /// must never be reinterpreted as triangle edges.
    VtVec3iArray const* triangleIndices = nullptr;
    VtVec3fArray const* points = nullptr;
    /// Name-indexed owning storage for primvar samplers.
    std::unordered_map<
        TfToken,
        std::unique_ptr<PrimvarSampler>,
        TfToken::HashFunctor> primvarMap;
    /// Cached view of the triangle normals sampler. This avoids hit-time map
    /// lookup and RTTI. Only vertex/varying and face-varying layouts can
    /// provide distinct corner normals; every other layout stays `none`.
    PrimvarSampler const* triangleNormalSampler = nullptr;
    TriangleCornerSamplerKind triangleNormalSamplerKind =
        TriangleCornerSamplerKind::none;
    /// Fixed-name tangent samplers cached to avoid hit-time token hashing.
    PrimvarSampler const* tangentSampler = nullptr;
    PrimvarSampler const* bitangentSampler = nullptr;
    PrimvarSampler const* computedTangentSampler = nullptr;
    PrimvarSampler const* computedBitangentSampler = nullptr;
    /// Optional primitive-to-coarse-face data consumed by the elementId AOV.
    /// Empty storage falls back to the raw Embree primitive ID.
    VtIntArray primitiveParams;
    /// Material-Sprim-owned stable handle, or nullptr if none. The handle
    /// remains registered until rendering is stopped and bindings refresh.
    MaterialData const* material = nullptr;
    /// Handle-indexed against material->geomPropNames. With a bound material,
    /// both vectors have exactly material->geomPropNames.size() entries; both
    /// are empty without one. They are rebuilt after every material-table or
    /// primvarMap mutation. Sampler pointers observe the unique_ptr-owned
    /// entries in primvarMap; empty values represent absent constant string
    /// primvars. The tables remain valid until the next stopped
    /// material-binding or primvar refresh.
    std::vector<PrimvarSampler*> geomPropSamplers;
    std::vector<mxcpp::Value> geomPropUniformValues;
};

///
/// \class InstanceContext
///
/// Renderer state attached to one top-level Embree instance as user data.
///
/// HdEmbreeMesh owns the stable-address object, and rootScene is a borrowed
/// prototype-scene handle owned by the same mesh. Instance Sync mutates this
/// state only after AcquireSceneForEdit() stops rendering; render workers
/// treat every field as immutable.
///
struct InstanceContext
{
    /// Mutually inverse affine transforms between prototype object space and
    /// world space. Normals use the inverse transpose of the relevant matrix.
    GfMatrix4f objectToWorldMatrix;
    GfMatrix4f worldToObjectMatrix;
    /// Borrowed committed prototype scene used to resolve geomID and call
    /// rtcInterpolate.
    RTCScene rootScene;
    /// Hydra instance index represented by this top-level geometry.
    int32_t instanceId;
    /// Resolved Hydra light- and shadow-link category memberships.
    CategorySet categories;
};


} // namespace ty
PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_IMAGING_PLUGIN_HD_EMBREE_CONTEXT_H
