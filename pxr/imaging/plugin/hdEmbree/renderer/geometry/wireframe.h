//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_WIREFRAME_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_WIREFRAME_H

#include "pxr/base/gf/vec4f.h"
#include "pxr/pxr.h"

#include <cstddef>

PXR_NAMESPACE_OPEN_SCOPE
namespace ty {

/// Parametric hit location and its one-pixel screen-space derivatives.
struct WireframeSample
{
    float u = 0.0f;
    float v = 0.0f;
    float dudx = 0.0f;
    float dvdx = 0.0f;
    float dudy = 0.0f;
    float dvdy = 0.0f;
};

/// Non-owning subdivision topology needed to reconstruct Embree's diced grid.
struct SubdivWireframeTopology
{
    int const* faceVertexCounts = nullptr;
    size_t faceCount = 0;
    size_t const* faceVertexOffsets = nullptr;
    size_t faceVertexOffsetCount = 0;
    float const* edgeLevels = nullptr;
    size_t edgeLevelCount = 0;
};

/// Convert texture-filter ray differentials back to a one-pixel footprint.
///
/// Camera ray differentials shrink by 1/sqrt(spp) for texture filtering, but
/// display wire widths must remain independent of the convergence sample
/// count. Multiply parametric derivatives by this value before edge tests.
float ComputeWireframeDerivativeScale(int samplesPerPixel);

/// Return screen-space line opacity for the hit triangle's three edges.
float ComputeTriangleWireframeOpacity(
    WireframeSample const& sample,
    float lineWidth);

/// Return screen-space line opacity for Embree's final subdivision grid.
///
/// Quad faces use the maximum pair of opposing edge tessellation levels.
/// Non-quad faces decode Embree's sub-patch UV convention and use the two
/// adjacent half edge levels. The returned grid includes every U/V edge and
/// the diagonal used to split each diced quad, including subpixel cells.
float ComputeSubdivisionWireframeOpacity(
    WireframeSample const& sample,
    unsigned int primitiveId,
    SubdivWireframeTopology const& topology,
    float lineWidth);

/// Composite unlit, opaque black wire coverage over the clear color.
GfVec4f CompositeEdgeOnlyWireframe(
    GfVec4f const& clearColor,
    float opacity);

} // namespace ty
PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_IMAGING_PLUGIN_HD_EMBREE_WIREFRAME_H
