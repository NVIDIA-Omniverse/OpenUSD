//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_TRIANGLE_MESH_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_TRIANGLE_MESH_H

#include "context.h"

#include "pxr/base/gf/vec3f.h"
#include "pxr/pxr.h"

PXR_NAMESPACE_OPEN_SCOPE
namespace ty {

/// Fetch the genuine object-space corners corresponding to Embree triangle
/// barycentrics. Returns false for missing or invalid mesh-owned cache data
/// and leaves outputs unchanged.
inline bool
SampleTrianglePositions(
    PrototypeContext const* prototypeContext,
    unsigned int primitiveId,
    GfVec3f* outP0,
    GfVec3f* outP1,
    GfVec3f* outP2)
{
    if (!prototypeContext || !prototypeContext->triangleIndices ||
        !prototypeContext->points || !outP0 || !outP1 || !outP2) {
        return false;
    }
    VtVec3iArray const& triangleIndices =
        *prototypeContext->triangleIndices;
    VtVec3fArray const& points = *prototypeContext->points;
    if (primitiveId >= triangleIndices.size()) {
        return false;
    }
    GfVec3i const& triangle = triangleIndices[primitiveId];
    if (triangle[0] < 0 || triangle[1] < 0 || triangle[2] < 0 ||
        static_cast<size_t>(triangle[0]) >= points.size() ||
        static_cast<size_t>(triangle[1]) >= points.size() ||
        static_cast<size_t>(triangle[2]) >= points.size()) {
        return false;
    }
    *outP0 = points[triangle[0]];
    *outP1 = points[triangle[1]];
    *outP2 = points[triangle[2]];
    return true;
}

/// Matches Embree triangle interpolation's weighted-barycentric association.
/// Inputs and barycentrics must be finite. Returns the interpolated position.
inline GfVec3f
InterpolateTrianglePosition(
    GfVec3f const& p0,
    GfVec3f const& p1,
    GfVec3f const& p2,
    float u,
    float v)
{
    const float bary0 = 1.0f - u - v;
    return bary0 * p0 + (u * p1 + v * p2);
}

} // namespace ty
PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_IMAGING_PLUGIN_HD_EMBREE_TRIANGLE_MESH_H
