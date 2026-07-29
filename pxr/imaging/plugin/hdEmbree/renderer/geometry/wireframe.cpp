//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//

#include "wireframe.h"

#include <algorithm>
#include <cmath>
#include <limits>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

constexpr float _minimumDerivative = 1.0e-8f;

float
_EdgeFalloff(float pixelDistance, float lineWidth)
{
    if (!std::isfinite(pixelDistance)) {
        return 0.0f;
    }
    const float width = std::max(1.0f, lineWidth);
    const float normalizedDistance = std::max(0.0f, pixelDistance) / width;
    return std::exp2(-4.0f * normalizedDistance * normalizedDistance);
}

float
_PixelDistance(
    float value,
    float derivativeX,
    float derivativeY)
{
    const float footprint =
        std::abs(derivativeX) + std::abs(derivativeY);
    if (!std::isfinite(value) || !std::isfinite(footprint) ||
        footprint <= _minimumDerivative) {
        return std::numeric_limits<float>::infinity();
    }
    return std::abs(value) / footprint;
}

float
_DistanceToInteger(float value)
{
    const float lower = std::floor(value);
    return std::min(value - lower, lower + 1.0f - value);
}

float
_GridWireframeOpacity(
    HdEmbreeWireframeSample const& sample,
    float uSegments,
    float vSegments,
    float lineWidth)
{
    if (!std::isfinite(uSegments) || !std::isfinite(vSegments) ||
        uSegments < 1.0f || vSegments < 1.0f) {
        return 0.0f;
    }

    const float gridU = sample.u * uSegments;
    const float gridV = sample.v * vSegments;
    const float gridDuDx = sample.dudx * uSegments;
    const float gridDuDy = sample.dudy * uSegments;
    const float gridDvDx = sample.dvdx * vSegments;
    const float gridDvDy = sample.dvdy * vSegments;

    const float uDistance = _PixelDistance(
        _DistanceToInteger(gridU),
        gridDuDx, gridDuDy);
    const float vDistance = _PixelDistance(
        _DistanceToInteger(gridV),
        gridDvDx, gridDvDy);

    // Embree splits each diced quad along the (1,0) -> (0,1) diagonal.
    // At grid boundaries the horizontal/vertical terms already cover the
    // shared diagonal endpoints, so only the current cell's diagonal is
    // needed here.
    const float localU = gridU - std::floor(gridU);
    const float localV = gridV - std::floor(gridV);
    const float diagonalDistance = _PixelDistance(
        localU + localV - 1.0f,
        gridDuDx + gridDvDx,
        gridDuDy + gridDvDy);

    return _EdgeFalloff(
        std::min({uDistance, vDistance, diagonalDistance}),
        lineWidth);
}

} // anonymous namespace

float
HdEmbreeComputeWireframeDerivativeScale(int samplesPerPixel)
{
    return samplesPerPixel > 1
        ? std::sqrt(static_cast<float>(samplesPerPixel))
        : 1.0f;
}

float
HdEmbreeComputeTriangleWireframeOpacity(
    HdEmbreeWireframeSample const& sample,
    float lineWidth)
{
    const float w = 1.0f - sample.u - sample.v;
    const float wDx = -sample.dudx - sample.dvdx;
    const float wDy = -sample.dudy - sample.dvdy;
    const float distance = std::min({
        _PixelDistance(sample.u, sample.dudx, sample.dudy),
        _PixelDistance(sample.v, sample.dvdx, sample.dvdy),
        _PixelDistance(w, wDx, wDy)});
    return _EdgeFalloff(distance, lineWidth);
}

float
HdEmbreeComputeSubdivisionWireframeOpacity(
    HdEmbreeWireframeSample const& encodedSample,
    unsigned int primitiveId,
    HdEmbreeSubdivWireframeTopology const& topology,
    float lineWidth)
{
    if (!topology.faceVertexCounts || !topology.faceVertexOffsets ||
        !topology.edgeLevels || primitiveId >= topology.faceCount ||
        topology.faceVertexOffsetCount != topology.faceCount + 1) {
        return 0.0f;
    }

    const int faceVertexCount = topology.faceVertexCounts[primitiveId];
    if (faceVertexCount < 3) {
        return 0.0f;
    }
    const size_t faceOffset = topology.faceVertexOffsets[primitiveId];
    const size_t faceEnd = topology.faceVertexOffsets[primitiveId + 1];
    if (faceEnd < faceOffset ||
        faceEnd - faceOffset != static_cast<size_t>(faceVertexCount) ||
        faceEnd > topology.edgeLevelCount) {
        return 0.0f;
    }

    HdEmbreeWireframeSample sample = encodedSample;
    float uSegments = 1.0f;
    float vSegments = 1.0f;
    if (faceVertexCount == 4) {
        uSegments = std::ceil(std::max(
            topology.edgeLevels[faceOffset],
            topology.edgeLevels[faceOffset + 2]));
        vSegments = std::ceil(std::max(
            topology.edgeLevels[faceOffset + 1],
            topology.edgeLevels[faceOffset + 3]));
    } else {
        // Embree encodes an n-gon's sub-patch id in 2x2 UV tiles. Decode the
        // local [0,1] coordinates exactly as documented for subdivision hits.
        const unsigned int low = static_cast<unsigned int>(
            std::floor(0.5f * sample.u));
        const unsigned int high = static_cast<unsigned int>(
            std::floor(0.5f * sample.v));
        const unsigned int subPatch = 4u * high + low;
        if (subPatch >= static_cast<unsigned int>(faceVertexCount)) {
            return 0.0f;
        }
        sample.u =
            2.0f * (0.5f * sample.u - std::floor(0.5f * sample.u)) - 0.5f;
        sample.v =
            2.0f * (0.5f * sample.v - std::floor(0.5f * sample.v)) - 0.5f;

        const size_t currentEdge = faceOffset + subPatch;
        const size_t previousEdge = faceOffset +
            (subPatch + static_cast<unsigned int>(faceVertexCount) - 1u) %
                static_cast<unsigned int>(faceVertexCount);
        const float segments = std::ceil(0.5f * std::max(
            topology.edgeLevels[currentEdge],
            topology.edgeLevels[previousEdge]));
        uSegments = segments;
        vSegments = segments;
    }

    return _GridWireframeOpacity(
        sample, uSegments, vSegments, lineWidth);
}

GfVec4f
HdEmbreeCompositeEdgeOnlyWireframe(
    GfVec4f const& clearColor,
    float opacity)
{
    const float coverage = std::clamp(opacity, 0.0f, 1.0f);
    const GfVec4f solidBlack(0.0f, 0.0f, 0.0f, 1.0f);
    return (1.0f - coverage) * clearColor + coverage * solidBlack;
}

PXR_NAMESPACE_CLOSE_SCOPE
