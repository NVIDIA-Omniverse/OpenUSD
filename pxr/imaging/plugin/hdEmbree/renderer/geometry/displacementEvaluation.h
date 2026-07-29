//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_DISPLACEMENT_EVALUATION_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_DISPLACEMENT_EVALUATION_H

#include "pxr/base/gf/vec3f.h"
#include "pxr/pxr.h"

#include <embree4/rtcore_geometry.h>

PXR_NAMESPACE_OPEN_SCOPE
namespace ty {

struct PrototypeContext;

/// Hit-local displaced subdivision frame and the three probes needed to
/// complete its normal derivatives lazily.
///
/// The public frame values are in object space and Embree patch coordinates.
/// The remaining fields cache the center/U/V displacement evaluations so a
/// later derivative request needs only the outer UU/UV/VV ring. This is a
/// short-lived value; it does not retain or own the geometry or context.
struct DisplacedSubdivFrame
{
    GfVec3f normal = GfVec3f(0.0f);
    GfVec3f dPdu = GfVec3f(0.0f);
    GfVec3f dPdv = GfVec3f(0.0f);

    GfVec3f uObjectOffset = GfVec3f(0.0f);
    GfVec3f vObjectOffset = GfVec3f(0.0f);
    GfVec3f uBaseDPdu = GfVec3f(0.0f);
    GfVec3f uBaseDPdv = GfVec3f(0.0f);
    GfVec3f vBaseDPdu = GfVec3f(0.0f);
    GfVec3f vBaseDPdv = GfVec3f(0.0f);

    unsigned int primID = 0;
    float u = 0.0f;
    float v = 0.0f;
    float du = 0.0f;
    float dv = 0.0f;
    bool valid = false;
};

/// Evaluate the prototype's scalar displacement graph at one undisplaced
/// subdivision-surface location.
///
/// \p position, \p normal, \p dPdu, and \p dPdv are object-space base-surface
/// values. The graph-facing context converts the normal and surface
/// derivatives to the prototype's displacement world space while retaining
/// object-space position derivatives for MaterialX position nodes.
/// \p displacement is written only on success.
bool EvaluateDisplacement(
    PrototypeContext const* context,
    unsigned int primID,
    float u,
    float v,
    GfVec3f const& position,
    GfVec3f const& normal,
    GfVec3f const& dPdu,
    GfVec3f const& dPdv,
    float* displacement);

/// Convert a graph displacement value in world units into the object-space
/// offset that Embree's prototype callback must apply.
///
/// The offset is chosen so transforming it by the prototype object-to-world
/// matrix produces exactly `displacement * worldNormal`, including under
/// non-uniform scale and reflection.
bool ComputeObjectSpaceDisplacementOffset(
    PrototypeContext const* context,
    GfVec3f const& objectNormal,
    float displacement,
    GfVec3f* objectOffset);

/// Evaluate the final object-space position of one displaced subdivision
/// surface point.
///
/// This follows the same limit-surface interpolation, MaterialX evaluation,
/// orientation, and world-distance conversion as the Embree displacement
/// callback. The output is written only when the complete result is finite.
bool ComputeDisplacedSubdivPosition(
    RTCGeometry geometry,
    PrototypeContext const* context,
    unsigned int primID,
    float u,
    float v,
    GfVec3f* outPosObj);

/// Compute a smooth object-space frame for the displaced subdivision surface.
///
/// Embree interpolation supplies the undisplaced limit surface and its first
/// derivatives. The displacement graph is sampled at the center and at one
/// finite-difference probe in each parameter direction. Each probe applies
/// the same world-normal/object-offset mapping as the Embree callback, so the
/// reconstructed frame remains correct under non-uniform transforms. Outputs
/// are written only when the complete frame is finite and non-degenerate.
bool ComputeDisplacedSubdivFrame(
    RTCGeometry geometry,
    PrototypeContext const* context,
    unsigned int primID,
    float u,
    float v,
    DisplacedSubdivFrame* outFrame);

/// Compatibility overload returning only the immediately needed frame.
bool ComputeDisplacedSubdivFrame(
    RTCGeometry geometry,
    PrototypeContext const* context,
    unsigned int primID,
    float u,
    float v,
    GfVec3f* outNormal,
    GfVec3f* outDPdu,
    GfVec3f* outDPdv);

/// Complete the object-space derivatives of a displaced smooth normal.
///
/// \p frame must be the successful result for this geometry and context. The
/// cached C/U/V probes are reused and only the outer UU/UV/VV displacement
/// samples are evaluated. The derivatives are returned in Embree patch
/// coordinates and aligned with `frame.normal`. Outputs are written only when
/// both derivatives are finite and all displaced probe frames are valid.
bool ComputeDisplacedSubdivNormalDerivatives(
    RTCGeometry geometry,
    PrototypeContext const* context,
    DisplacedSubdivFrame const& frame,
    GfVec3f* outDndu,
    GfVec3f* outDndv);

} // namespace ty
PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_IMAGING_PLUGIN_HD_EMBREE_DISPLACEMENT_EVALUATION_H
