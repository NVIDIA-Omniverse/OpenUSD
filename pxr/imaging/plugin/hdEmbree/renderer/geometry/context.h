//
// Copyright 2016 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_CONTEXT_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_CONTEXT_H

#include "pxr/pxr.h"

#include "pxr/imaging/plugin/hdEmbree/renderer/geometry/primvarSampler.h"
#include "pxr/imaging/hd/enums.h"
#include "pxr/imaging/plugin/hdEmbree/renderer/lights/lightLinking.h"
#include "pxr/imaging/plugin/hdEmbree/renderer/materials/material.h"
#include "pxr/imaging/plugin/hdEmbree/renderer/materials/materialEvalContext.h"

#include "pxr/base/gf/matrix4f.h"
#include "pxr/base/vt/array.h"

#include <unordered_map>
#include <string>
#include <vector>
#include "pxr/imaging/plugin/hdEmbree/renderer/materials/MaterialXCpp/value.h"

#include <embree4/rtcore.h>

PXR_NAMESPACE_OPEN_SCOPE

/// Reserved ray ID used only by owner-prototype SSS boundary queries. Such
/// rays must see both sides of the closed surface regardless of display cull
/// style, while ordinary renderer rays always use ID zero.
constexpr unsigned int HdEmbreeFaceCullBypassRayId = 0x48444543u;

/// Surface display mode selected by the active Hydra mesh representation.
enum class HdEmbreeWireframeMode
{
    disabled,
    edgeOnly,
    edgeOnSurface
};


/// \class HdEmbreePrototypeContext
///
/// A small bit of state attached to each bit of prototype geometry in embree,
/// for renderer integrators and geometric AOV evaluation.
///
struct HdEmbreePrototypeContext
{
    int32_t primId = 0;
    HdCullStyle cullStyle = HdCullStyleDontCare;
    bool doubleSided = false;
    bool refined = false;
    HdEmbreeWireframeMode wireframeMode =
        HdEmbreeWireframeMode::disabled;
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
    /// Converts Embree's subdivision winding to the authored USD orientation.
    /// Coarse triangles are already reordered by HdMeshUtil and stay +1.
    float orientationSign = 1.0f;
    /// Renderer-owned material evaluation services. Geometry callbacks borrow
    /// this state; it is updated only while rendering is stopped and remains
    /// read-only throughout geometry commits and rendering.
    HdEmbreeMaterialEvalServices const* materialEvalServices = nullptr;
    /// Prototype-level transforms used while Embree evaluates displacement.
    /// Per-instance transforms are unavailable when a shared prototype scene
    /// is committed, so these default to object-space identity.
    GfMatrix4f displacementObjectToWorldMatrix = GfMatrix4f(1.0f);
    GfMatrix4f displacementWorldToObjectMatrix = GfMatrix4f(1.0f);
    VtVec3fArray const* triangleDPdu = nullptr;
    VtVec3fArray const* triangleDPdv = nullptr;
    /// A name-indexed map of primvar samplers.
    TfHashMap<TfToken, HdEmbreePrimvarSampler*, TfToken::HashFunctor>
        primvarMap;
    /// String-keyed mirror of primvarMap for the material geomprop-lookup
    /// callback, which receives plain string names from the pxr-independent
    /// shading core.  Looking up here avoids constructing a TfToken — a
    /// locked global-table operation — on every material input evaluation,
    /// which is far too hot for the shading inner loop.  The samplers are
    /// owned by primvarMap; this map only references them.
    std::unordered_map<std::string, HdEmbreePrimvarSampler*>
        primvarMapByString;
    /// A copy of the primitive params for this rprim.
    VtIntArray primitiveParams;
    /// The bound material, or nullptr if none.
    HdEmbreeMaterialData const* material = nullptr;
    /// Per-mesh uniform primvar values for geompropvalueuniform nodes.
    /// Built once during Sync from HdInterpolationConstant primvars.
    std::unordered_map<std::string, mxcpp::Value> uniformPrimvarMap;
};

///
/// \class HdEmbreeInstanceContext
///
/// A small bit of state attached to each bit of instanced geometry in embree,
/// for renderer integrators and geometric AOV evaluation.
///
struct HdEmbreeInstanceContext
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
    HdEmbreeCategorySet categories;
};


PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_IMAGING_PLUGIN_HD_EMBREE_CONTEXT_H
