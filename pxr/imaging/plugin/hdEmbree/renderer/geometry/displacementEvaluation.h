//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_DISPLACEMENT_EVALUATION_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_DISPLACEMENT_EVALUATION_H

#include "pxr/pxr.h"

#include "pxr/base/gf/vec3f.h"

#include <embree4/rtcore_geometry.h>

PXR_NAMESPACE_OPEN_SCOPE

struct HdEmbreePrototypeContext;

/// Evaluate the prototype's scalar displacement graph at one undisplaced
/// subdivision-surface location.
///
/// \p position, \p normal, \p dPdu, and \p dPdv are object-space base-surface
/// values. The graph-facing context converts the normal and surface
/// derivatives to the prototype's displacement world space while retaining
/// object-space position derivatives for MaterialX position nodes.
/// \p displacement is written only on success.
bool HdEmbreeEvaluateDisplacement(
    HdEmbreePrototypeContext const* context,
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
bool HdEmbreeComputeObjectSpaceDisplacementOffset(
    HdEmbreePrototypeContext const* context,
    GfVec3f const& objectNormal,
    float displacement,
    GfVec3f* objectOffset);

/// Compute a smooth object-space frame for the displaced subdivision surface.
///
/// Embree interpolation supplies the undisplaced limit surface and its first
/// derivatives. The displacement graph is sampled at the center and at one
/// finite-difference probe in each parameter direction. Each probe applies
/// the same world-normal/object-offset mapping as the Embree callback, so the
/// reconstructed frame remains correct under non-uniform transforms. Outputs
/// are written only when the complete frame is finite and non-degenerate.
bool HdEmbreeComputeDisplacedSubdivFrame(
    RTCGeometry geometry,
    HdEmbreePrototypeContext const* context,
    unsigned int primID,
    float u,
    float v,
    GfVec3f* outNormal,
    GfVec3f* outDPdu,
    GfVec3f* outDPdv);

PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_IMAGING_PLUGIN_HD_EMBREE_DISPLACEMENT_EVALUATION_H
