//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/imaging/plugin/hdEmbree/delegate/adaptiveSubdivision.h"

#include "pxr/base/gf/vec2d.h"
#include "pxr/base/gf/vec4d.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

constexpr double _minW = 1.0e-6;

bool
_IsFinite(GfVec4d const& value)
{
    return std::isfinite(value[0]) && std::isfinite(value[1]) &&
        std::isfinite(value[2]) && std::isfinite(value[3]);
}

bool
_ClipEdgeToViewVolume(GfVec4d* p0, GfVec4d* p1)
{
    if (!_IsFinite(*p0) || !_IsFinite(*p1)) {
        return false;
    }

    // Clip a homogeneous segment against the OpenGL clip volume used by
    // OpenUSD camera matrices. Doing this before division prevents edges
    // outside the viewport (or crossing the eye plane) from producing huge
    // subdivision levels.
    const auto clipToPlane = [&](auto const& distance) {
        double d0 = distance(*p0);
        double d1 = distance(*p1);
        const bool inside0 = d0 >= 0.0;
        const bool inside1 = d1 >= 0.0;
        if (!inside0 && !inside1) {
            return false;
        }
        if (inside0 && inside1) {
            return true;
        }
        const double denominator = d0 - d1;
        if (!std::isfinite(denominator) ||
            std::abs(denominator) <= _minW) {
            return false;
        }
        const double t = std::clamp(d0 / denominator, 0.0, 1.0);
        const GfVec4d clipped = *p0 + (*p1 - *p0) * t;
        if (!_IsFinite(clipped)) {
            return false;
        }
        if (inside0) {
            *p1 = clipped;
        } else {
            *p0 = clipped;
        }
        return true;
    };

    return
        clipToPlane([](GfVec4d const& p) { return p[3] - _minW; }) &&
        clipToPlane([](GfVec4d const& p) { return p[0] + p[3]; }) &&
        clipToPlane([](GfVec4d const& p) { return p[3] - p[0]; }) &&
        clipToPlane([](GfVec4d const& p) { return p[1] + p[3]; }) &&
        clipToPlane([](GfVec4d const& p) { return p[3] - p[1]; }) &&
        clipToPlane([](GfVec4d const& p) { return p[2] + p[3]; }) &&
        clipToPlane([](GfVec4d const& p) { return p[3] - p[2]; });
}

bool
_ProjectEdge(
    GfVec3f const& p0,
    GfVec3f const& p1,
    GfMatrix4d const& objectToClip,
    double width,
    double height,
    double* pixelLength)
{
    GfVec4d clip0 =
        GfVec4d(p0[0], p0[1], p0[2], 1.0) * objectToClip;
    GfVec4d clip1 =
        GfVec4d(p1[0], p1[1], p1[2], 1.0) * objectToClip;
    if (!_ClipEdgeToViewVolume(&clip0, &clip1)) {
        return false;
    }

    const GfVec2d pixel0(
        (clip0[0] / clip0[3] * 0.5 + 0.5) * width,
        (clip0[1] / clip0[3] * 0.5 + 0.5) * height);
    const GfVec2d pixel1(
        (clip1[0] / clip1[3] * 0.5 + 0.5) * width,
        (clip1[1] / clip1[3] * 0.5 + 0.5) * height);
    if (!std::isfinite(pixel0[0]) || !std::isfinite(pixel0[1]) ||
        !std::isfinite(pixel1[0]) || !std::isfinite(pixel1[1])) {
        return false;
    }
    *pixelLength = (pixel1 - pixel0).GetLength();
    return true;
}

} // namespace

double
HdEmbreeGetTargetSubdivisionEdgePixels(int refineLevel)
{
    // Screen-space targets keep complexity visually stable as the camera and
    // output resolution change; fixed object-space rates cannot do that.
    return refineLevel >= 3 ? 0.5 :
        refineLevel == 2 ? 1.0 : 4.0;
}

std::vector<float>
HdEmbreeConsolidateSharedEdgeLevels(
    VtIntArray const& faceVertexCounts,
    VtIntArray const& faceVertexIndices,
    std::vector<float> const& candidateLevels)
{
    if (candidateLevels.size() != faceVertexIndices.size()) {
        return {};
    }
    // Embree accepts a level per face-edge, but adjacent faces must agree
    // on their shared edge. Choosing the maximum avoids cracks without making
    // the better-resolved face coarser.
    std::vector<uint64_t> edgeKeys(faceVertexIndices.size());
    std::unordered_map<uint64_t, float> edgeLevels;
    size_t offset = 0;
    for (int count : faceVertexCounts) {
        if (count <= 0 || offset + static_cast<size_t>(count) >
                faceVertexIndices.size()) {
            return {};
        }
        for (int corner = 0; corner < count; ++corner) {
            const int i0 = faceVertexIndices[offset + corner];
            const int i1 = faceVertexIndices[
                offset + ((corner + 1) % count)];
            if (i0 < 0 || i1 < 0 ||
                !std::isfinite(candidateLevels[offset + corner])) {
                return {};
            }
            const uint32_t lo = static_cast<uint32_t>(std::min(i0, i1));
            const uint32_t hi = static_cast<uint32_t>(std::max(i0, i1));
            const uint64_t key =
                (static_cast<uint64_t>(lo) << 32) | hi;
            edgeKeys[offset + corner] = key;
            auto [it, inserted] = edgeLevels.emplace(
                key, candidateLevels[offset + corner]);
            if (!inserted) {
                it->second = std::max(
                    it->second, candidateLevels[offset + corner]);
            }
        }
        offset += static_cast<size_t>(count);
    }
    if (offset != faceVertexIndices.size()) {
        return {};
    }

    std::vector<float> result(edgeKeys.size(), 1.0f);
    for (size_t i = 0; i < edgeKeys.size(); ++i) {
        result[i] = edgeLevels[edgeKeys[i]];
    }
    return result;
}

std::vector<float>
HdEmbreeComputeAdaptiveSubdivisionLevels(
    VtVec3fArray const& points,
    VtIntArray const& faceVertexCounts,
    VtIntArray const& faceVertexIndices,
    std::vector<GfMatrix4f> const& instanceTransforms,
    GfMatrix4d const& viewMatrix,
    GfMatrix4d const& projectionMatrix,
    GfRect2i const& dataWindow,
    int refineLevel)
{
    if (points.empty() || faceVertexIndices.empty() ||
        instanceTransforms.empty() || dataWindow.GetWidth() <= 0 ||
        dataWindow.GetHeight() <= 0) {
        return {};
    }

    const double width = static_cast<double>(dataWindow.GetWidth());
    const double height = static_cast<double>(dataWindow.GetHeight());
    const double targetPixels =
        HdEmbreeGetTargetSubdivisionEdgePixels(refineLevel);
    // Use every instance because a shared prototype must satisfy the
    // instance with the largest projected edge.
    std::vector<GfMatrix4d> objectToClip;
    objectToClip.reserve(instanceTransforms.size());
    for (GfMatrix4f const& transform : instanceTransforms) {
        objectToClip.push_back(
            GfMatrix4d(transform) * viewMatrix * projectionMatrix);
    }

    std::vector<float> candidateLevels(faceVertexIndices.size(), 1.0f);
    size_t offset = 0;
    for (int count : faceVertexCounts) {
        if (count <= 0 || offset + static_cast<size_t>(count) >
                faceVertexIndices.size()) {
            return {};
        }
        for (int corner = 0; corner < count; ++corner) {
            const int i0 = faceVertexIndices[offset + corner];
            const int i1 = faceVertexIndices[
                offset + ((corner + 1) % count)];
            if (i0 < 0 || i1 < 0 ||
                static_cast<size_t>(i0) >= points.size() ||
                static_cast<size_t>(i1) >= points.size()) {
                return {};
            }

            double maxPixelLength = 0.0;
            for (GfMatrix4d const& matrix : objectToClip) {
                double pixelLength = 0.0;
                if (_ProjectEdge(
                        points[i0], points[i1], matrix,
                        width, height, &pixelLength)) {
                    maxPixelLength = std::max(maxPixelLength, pixelLength);
                }
            }
            // Embree levels are segments per edge. Rounding up guarantees
            // those segments are no longer than the requested pixel target.
            candidateLevels[offset + corner] = static_cast<float>(
                std::clamp(
                    std::ceil(maxPixelLength / targetPixels),
                    1.0, 4096.0));
        }
        offset += static_cast<size_t>(count);
    }
    if (offset != faceVertexIndices.size()) {
        return {};
    }
    return HdEmbreeConsolidateSharedEdgeLevels(
        faceVertexCounts, faceVertexIndices, candidateLevels);
}

PXR_NAMESPACE_CLOSE_SCOPE
