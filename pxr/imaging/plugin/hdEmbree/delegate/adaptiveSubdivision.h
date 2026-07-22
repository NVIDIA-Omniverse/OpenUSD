//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_ADAPTIVE_SUBDIVISION_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_ADAPTIVE_SUBDIVISION_H

#include "pxr/pxr.h"
#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/gf/matrix4f.h"
#include "pxr/base/gf/rect2i.h"
#include "pxr/base/vt/array.h"
#include "pxr/base/vt/types.h"

#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

/// Returns the target screen-space edge length for a Hydra refine level.
double HdEmbreeGetTargetSubdivisionEdgePixels(int refineLevel);

/// Applies the maximum candidate level to every occurrence of a shared
/// unoriented coarse edge. Returns empty for invalid topology or candidates.
std::vector<float> HdEmbreeConsolidateSharedEdgeLevels(
    VtIntArray const& faceVertexCounts,
    VtIntArray const& faceVertexIndices,
    std::vector<float> const& candidateLevels);

/// Raises candidate levels until shared coarse edges agree and opposite edge
/// pairs of every quad differ by at most 2:1. Shared-edge consolidation
/// and quad balancing repeat to a fixed point. Non-quad faces are not balanced.
/// Never lowers a candidate and returns empty for invalid topology or levels.
std::vector<float> HdEmbreeBalanceSubdivisionLevels(
    VtIntArray const& faceVertexCounts,
    VtIntArray const& faceVertexIndices,
    std::vector<float> const& candidateLevels);

/// Computes one balanced Embree subdivision level per face corner. Candidates
/// include the viewport guard and minimum required level, then shared edges
/// and quad opposite edges are balanced. An empty result indicates invalid
/// topology or framing inputs.
std::vector<float> HdEmbreeComputeAdaptiveSubdivisionLevels(
    VtVec3fArray const& points,
    VtIntArray const& faceVertexCounts,
    VtIntArray const& faceVertexIndices,
    std::vector<GfMatrix4f> const& instanceTransforms,
    GfMatrix4d const& viewMatrix,
    GfMatrix4d const& projectionMatrix,
    GfRect2i const& dataWindow,
    int refineLevel);

PXR_NAMESPACE_CLOSE_SCOPE

#endif
