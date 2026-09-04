//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
// Mesh and curve surface-frame resolution.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_GEOMETRY_SURFACE_DERIVATIVES_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_GEOMETRY_SURFACE_DERIVATIVES_H

#include <renderer/embreeCompat.h>

#include "pxr/base/gf/vec3f.h"

PXR_NAMESPACE_OPEN_SCOPE
namespace ty {

struct PrototypeContext;
struct InstanceContext;
struct DisplacedSubdivFrame;


/// Resolve an object-space shading normal for a valid Embree hit.
///
/// `prototypeContext` and `rootScene` must be valid and `geomID` must identify
/// the hit prototype. Invalid authored/displacement data falls back to the
/// oriented Embree geometric normal. When supplied, `displacedFrame` is always
/// initialized and is valid only when displaced-frame evaluation succeeds.
GfVec3f ResolveObjectSpaceNormal(
    PrototypeContext const* prototypeContext,
    RTCScene rootScene,
    unsigned int geomID,
    RTCRayHit const& rayHit,
    DisplacedSubdivFrame* displacedFrame = nullptr);

/// Resolve the object-space position of the actual intersected surface.
///
/// When exact mesh interpolation is requested, triangles and subdivision
/// surfaces may replace transform cancellation with primitive interpolation.
/// Curves always retain the inverse-transformed ray hit because vertex
/// interpolation on an Embree curve returns its centerline rather than its
/// surface. A valid displaced frame takes precedence for subdivision.
GfVec3f ResolveObjectSpaceSurfacePosition(
    PrototypeContext const* prototypeContext,
    InstanceContext const* instanceContext,
    RTCScene rootScene,
    unsigned int geomID,
    RTCRayHit const& rayHit,
    DisplacedSubdivFrame const* displacedFrame,
    bool exactMeshInterpolation);

/// Compute object-space triangle surface and normal derivatives.
///
/// All pointers must be valid and `primID` must address the prototype's cached
/// triangle data. Degenerate position derivatives fall back to an orthonormal
/// frame; unavailable normal derivatives are returned as zero.
void ComputeTriangleSurfaceDerivatives(
    PrototypeContext const* prototypeContext,
    unsigned int primID,
    float u,
    float v,
    GfVec3f const& normal,
    GfVec3f* outDPdu,
    GfVec3f* outDPdv,
    GfVec3f* outDndu,
    GfVec3f* outDndv);

/// Compute an object-space curve frame from the centerline derivative and
/// Embree's actual surface normal.
///
/// The curve's generated primitive ID remains in Embree's record-local
/// domain. Sphere points and invalid/degenerate centerline derivatives return
/// a deterministic orthonormal frame. Curve normal derivatives are zero
/// because authored ribbon normals orient geometry and are not surface-normal
/// samples.
void ComputeCurveSurfaceDerivatives(
    PrototypeContext const* prototypeContext,
    RTCScene rootScene,
    unsigned int geomID,
    unsigned int primitiveId,
    float u,
    GfVec3f const& surfaceNormal,
    GfVec3f* outDPdu,
    GfVec3f* outDPdv,
    GfVec3f* outDndu,
    GfVec3f* outDndv);

/// Compute object-space subdivision surface and normal derivatives.
///
/// The scene/context/geometry identifiers and all output pointers must be
/// valid. Invalid or degenerate interpolation falls back to an orthonormal
/// position frame and zero normal derivatives.
void ComputeSubdivSurfaceDerivatives(
    PrototypeContext const* prototypeContext,
    RTCScene rootScene,
    unsigned int geomID,
    unsigned int primID,
    float u,
    float v,
    GfVec3f const& normal,
    GfVec3f* outDPdu,
    GfVec3f* outDPdv,
    GfVec3f* outDndu,
    GfVec3f* outDndv,
    DisplacedSubdivFrame const* displacedFrame = nullptr);

/// Complete displaced-normal derivatives and transform them to world space.
///
/// Returns false for null inputs, invalid cached frames, failed displacement
/// evaluation, or non-finite results. Outputs are written only on success.
bool TryComputeDisplacedSubdivNormalDerivativesToWorld(
    PrototypeContext const* prototypeContext,
    InstanceContext const* instanceContext,
    RTCScene rootScene,
    unsigned int geomID,
    DisplacedSubdivFrame const& frame,
    GfVec3f const& faceForwardedWorldNormal,
    GfVec3f* outDndu,
    GfVec3f* outDndv);

} // namespace ty
PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_IMAGING_PLUGIN_HD_EMBREE_GEOMETRY_SURFACE_DERIVATIVES_H
