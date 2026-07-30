//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "adaptiveSubdivision.h"

#include "pxr/base/gf/vec2d.h"
#include "pxr/base/gf/vec4d.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <unordered_map>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

constexpr double _minW = 1.0e-6;
constexpr double _viewGuardScale = 1.1;
constexpr float _minSubdivisionLevel = 1.0f;
constexpr float _minRequiredSubdivisionLevel = 4.0f;
constexpr float _maxSubdivisionLevel = 4096.0f;
constexpr float _maxDisplacementLevelScale = 2.0f;
constexpr double _mediumDisplacementChordErrorPixels = 0.5;
constexpr double _fineDisplacementChordErrorPixels = 0.25;
constexpr size_t _displacementProbeGridWidth = 3;
constexpr std::array<float, _displacementProbeGridWidth>
    _displacementProbeCoordinates{0.0f, 0.5f, 1.0f};

struct _SharedEdgeGroups
{
    std::vector<size_t> cornerGroupIndices;
    size_t groupCount = 0;
};

std::optional<std::vector<size_t>>
_BuildQuadEdgeStripComponents(
    VtIntArray const& faceVertexCounts,
    _SharedEdgeGroups const& sharedEdges)
{
    std::vector<size_t> parents(sharedEdges.groupCount);
    for (size_t group = 0; group < parents.size(); ++group) {
        parents[group] = group;
    }

    const auto findRoot = [&parents](size_t group) {
        while (parents[group] != group) {
            parents[group] = parents[parents[group]];
            group = parents[group];
        }
        return group;
    };
    const auto unite = [&parents, &findRoot](size_t first, size_t second) {
        const size_t firstRoot = findRoot(first);
        const size_t secondRoot = findRoot(second);
        if (firstRoot != secondRoot) {
            parents[secondRoot] = firstRoot;
        }
    };

    size_t offset = 0;
    for (const int count : faceVertexCounts) {
        if (count <= 0 || offset + static_cast<size_t>(count) >
                sharedEdges.cornerGroupIndices.size()) {
            return std::nullopt;
        }
        if (count == 4) {
            // Embree creates transition triangles when opposite levels
            // differ. Connect opposite shared-edge groups so a displacement
            // boost can travel along the complete quad edge strip before
            // levels are changed.
            unite(
                sharedEdges.cornerGroupIndices[offset],
                sharedEdges.cornerGroupIndices[offset + 2]);
            unite(
                sharedEdges.cornerGroupIndices[offset + 1],
                sharedEdges.cornerGroupIndices[offset + 3]);
        }
        offset += static_cast<size_t>(count);
    }
    if (offset != sharedEdges.cornerGroupIndices.size()) {
        return std::nullopt;
    }

    std::vector<size_t> components(sharedEdges.groupCount);
    for (size_t group = 0; group < components.size(); ++group) {
        components[group] = findRoot(group);
    }
    return components;
}

std::optional<_SharedEdgeGroups>
_BuildSharedEdgeGroups(
    VtIntArray const& faceVertexCounts,
    VtIntArray const& faceVertexIndices)
{
    _SharedEdgeGroups groups;
    groups.cornerGroupIndices.resize(faceVertexIndices.size());
    std::unordered_map<uint64_t, size_t> groupIndices;
    groupIndices.reserve(faceVertexIndices.size());

    size_t offset = 0;
    for (const int count : faceVertexCounts) {
        if (count <= 0 || offset + static_cast<size_t>(count) >
                faceVertexIndices.size()) {
            return std::nullopt;
        }
        for (int corner = 0; corner < count; ++corner) {
            const int i0 = faceVertexIndices[offset + corner];
            const int i1 = faceVertexIndices[
                offset + ((corner + 1) % count)];
            if (i0 < 0 || i1 < 0) {
                return std::nullopt;
            }

            const uint32_t lo = static_cast<uint32_t>(std::min(i0, i1));
            const uint32_t hi = static_cast<uint32_t>(std::max(i0, i1));
            const uint64_t key =
                (static_cast<uint64_t>(lo) << 32) | hi;
            const auto [it, inserted] =
                groupIndices.emplace(key, groups.groupCount);
            if (inserted) {
                ++groups.groupCount;
            }
            groups.cornerGroupIndices[offset + corner] = it->second;
        }
        offset += static_cast<size_t>(count);
    }
    if (offset != faceVertexIndices.size()) {
        return std::nullopt;
    }
    return groups;
}

bool
_AreValidSubdivisionLevels(std::vector<float> const& levels)
{
    return std::all_of(
        levels.begin(), levels.end(), [](const float level) {
            return std::isfinite(level) &&
                level >= _minSubdivisionLevel &&
                level <= _maxSubdivisionLevel;
        });
}

bool
_ConsolidateSharedEdgesInPlace(
    _SharedEdgeGroups const& groups,
    std::vector<float>* const levels)
{
    bool changed = false;
    std::vector<float> maxima(
        groups.groupCount, _minSubdivisionLevel);
    for (size_t corner = 0;
         corner < groups.cornerGroupIndices.size(); ++corner) {
        const size_t group = groups.cornerGroupIndices[corner];
        maxima[group] = std::max(maxima[group], (*levels)[corner]);
    }
    for (size_t corner = 0;
         corner < groups.cornerGroupIndices.size(); ++corner) {
        const float maximum =
            maxima[groups.cornerGroupIndices[corner]];
        if ((*levels)[corner] < maximum) {
            (*levels)[corner] = maximum;
            changed = true;
        }
    }
    return changed;
}

bool
_BalanceQuadOppositeEdgesInPlace(
    VtIntArray const& faceVertexCounts,
    std::vector<float>* const levels)
{
    bool changed = false;
    size_t offset = 0;
    for (const int count : faceVertexCounts) {
        if (count == 4) {
            const auto balancePair = [&](const size_t firstIndex,
                                         const size_t secondIndex) {
                float& first = (*levels)[offset + firstIndex];
                float& second = (*levels)[offset + secondIndex];
                const float minimum = static_cast<float>(std::ceil(
                    static_cast<double>(std::max(first, second)) * 0.5));
                if (first < minimum) {
                    first = minimum;
                    changed = true;
                }
                if (second < minimum) {
                    second = minimum;
                    changed = true;
                }
            };
            balancePair(0, 2);
            balancePair(1, 3);
        }
        offset += static_cast<size_t>(count);
    }
    return changed;
}

bool
_IsFinite(GfVec4d const& value)
{
    return std::isfinite(value[0]) && std::isfinite(value[1]) &&
        std::isfinite(value[2]) && std::isfinite(value[3]);
}

bool
_IsFinite(GfVec3f const& value)
{
    return std::isfinite(value[0]) && std::isfinite(value[1]) &&
        std::isfinite(value[2]);
}

bool
_ClipEdgeToViewVolume(GfVec4d* pos0, GfVec4d* pos1)
{
    if (!_IsFinite(*pos0) || !_IsFinite(*pos1)) {
        return false;
    }

    // Clip a homogeneous segment against the OpenGL clip volume used by
    // OpenUSD camera matrices, with X/Y headroom for displaced patches that
    // can move into view. Doing this before division prevents remote edges or
    // eye-plane crossings from producing huge subdivision levels.
    // Keep callable types visible so optimized builds inline every plane.
    const auto clipToPlane = [&](auto const& distance) {
        double d0 = distance(*pos0);
        double d1 = distance(*pos1);
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
        const GfVec4d clipped = *pos0 + (*pos1 - *pos0) * t;
        if (!_IsFinite(clipped)) {
            return false;
        }
        if (inside0) {
            *pos1 = clipped;
        } else {
            *pos0 = clipped;
        }
        return true;
    };

    return
        clipToPlane([](GfVec4d const& posClip) {
            return posClip[3] - _minW;
        }) &&
        clipToPlane([](GfVec4d const& posClip) {
            return posClip[0] + _viewGuardScale * posClip[3];
        }) &&
        clipToPlane([](GfVec4d const& posClip) {
            return _viewGuardScale * posClip[3] - posClip[0];
        }) &&
        clipToPlane([](GfVec4d const& posClip) {
            return posClip[1] + _viewGuardScale * posClip[3];
        }) &&
        clipToPlane([](GfVec4d const& posClip) {
            return _viewGuardScale * posClip[3] - posClip[1];
        }) &&
        clipToPlane([](GfVec4d const& posClip) {
            return posClip[2] + posClip[3];
        }) &&
        clipToPlane([](GfVec4d const& posClip) {
            return posClip[3] - posClip[2];
        });
}

bool
_ProjectEdge(
    GfVec3f const& pos0,
    GfVec3f const& pos1,
    GfMatrix4d const& objectToClip,
    double width,
    double height,
    double* pixelLength)
{
    GfVec4d clip0 =
        GfVec4d(pos0[0], pos0[1], pos0[2], 1.0) * objectToClip;
    GfVec4d clip1 =
        GfVec4d(pos1[0], pos1[1], pos1[2], 1.0) * objectToClip;
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

bool
_ProjectPoint(
    GfVec3f const& point,
    GfMatrix4d const& objectToClip,
    double width,
    double height,
    GfVec2d* pixel)
{
    if (!pixel || !_IsFinite(point)) {
        return false;
    }
    const GfVec4d clip =
        GfVec4d(point[0], point[1], point[2], 1.0) * objectToClip;
    if (!_IsFinite(clip) || clip[3] <= _minW) {
        return false;
    }

    const GfVec2d projected(
        (clip[0] / clip[3] * 0.5 + 0.5) * width,
        (clip[1] / clip[3] * 0.5 + 0.5) * height);
    if (!std::isfinite(projected[0]) ||
        !std::isfinite(projected[1])) {
        return false;
    }
    *pixel = projected;
    return true;
}

double
_DistanceToLineSegment(
    GfVec2d const& point,
    GfVec2d const& start,
    GfVec2d const& end)
{
    const GfVec2d segment = end - start;
    const double lengthSquared = segment.GetLengthSq();
    if (!std::isfinite(lengthSquared) || lengthSquared <= 0.0) {
        return (point - start).GetLength();
    }
    const double parameter = std::clamp(
        GfDot(point - start, segment) / lengthSquared, 0.0, 1.0);
    return (point - (start + parameter * segment)).GetLength();
}

bool
_ComputeProjectedChordError(
    GfVec3f const& start,
    GfVec3f const& midpoint,
    GfVec3f const& end,
    GfMatrix4d const& objectToClip,
    double width,
    double height,
    double* errorPixels)
{
    if (!errorPixels) {
        return false;
    }

    // Only measure curves whose first or second half reaches the guarded
    // view volume. This retains the existing offscreen/near-plane protection
    // while still detecting a displaced midpoint that bends into view.
    double ignoredLength = 0.0;
    if (!_ProjectEdge(
            start, midpoint, objectToClip,
            width, height, &ignoredLength) &&
        !_ProjectEdge(
            midpoint, end, objectToClip,
            width, height, &ignoredLength)) {
        return false;
    }

    GfVec2d projectedStart;
    GfVec2d projectedMidpoint;
    GfVec2d projectedEnd;
    if (!_ProjectPoint(
            start, objectToClip, width, height, &projectedStart) ||
        !_ProjectPoint(
            midpoint, objectToClip, width, height,
            &projectedMidpoint) ||
        !_ProjectPoint(
            end, objectToClip, width, height, &projectedEnd)) {
        return false;
    }

    const double error = _DistanceToLineSegment(
        projectedMidpoint, projectedStart, projectedEnd);
    if (!std::isfinite(error)) {
        return false;
    }
    *errorPixels = error;
    return true;
}

double
_GetTargetDisplacementChordErrorPixels(int refineLevel)
{
    return refineLevel >= 2
        ? _fineDisplacementChordErrorPixels
        : _mediumDisplacementChordErrorPixels;
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
    if (candidateLevels.size() != faceVertexIndices.size() ||
        !_AreValidSubdivisionLevels(candidateLevels)) {
        return {};
    }
    const std::optional<_SharedEdgeGroups> groups =
        _BuildSharedEdgeGroups(faceVertexCounts, faceVertexIndices);
    if (!groups) {
        return {};
    }

    // Embree accepts a level per face-edge, but adjacent faces must agree
    // on their shared edge. Choosing the maximum avoids cracks without making
    // the better-resolved face coarser.
    std::vector<float> result(candidateLevels);
    _ConsolidateSharedEdgesInPlace(*groups, &result);
    return result;
}

std::vector<float>
HdEmbreeBalanceSubdivisionLevels(
    VtIntArray const& faceVertexCounts,
    VtIntArray const& faceVertexIndices,
    std::vector<float> const& candidateLevels)
{
    if (candidateLevels.size() != faceVertexIndices.size() ||
        !_AreValidSubdivisionLevels(candidateLevels)) {
        return {};
    }
    const std::optional<_SharedEdgeGroups> groups =
        _BuildSharedEdgeGroups(faceVertexCounts, faceVertexIndices);
    if (!groups) {
        return {};
    }

    std::vector<float> result(candidateLevels);
    _ConsolidateSharedEdgesInPlace(*groups, &result);
    while (true) {
        bool changed =
            _BalanceQuadOppositeEdgesInPlace(faceVertexCounts, &result);
        changed =
            _ConsolidateSharedEdgesInPlace(*groups, &result) || changed;
        if (!changed) {
            return result;
        }
    }
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
    int refineLevel,
    HdEmbreeDisplacedPositionProbe const& displacedPositionProbe)
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

    std::vector<float> candidateLevels(
        faceVertexIndices.size(), _minRequiredSubdivisionLevel);
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
                    static_cast<double>(_minRequiredSubdivisionLevel),
                    static_cast<double>(_maxSubdivisionLevel)));
        }
        offset += static_cast<size_t>(count);
    }
    if (offset != faceVertexIndices.size()) {
        return {};
    }
    const std::vector<float> baselineLevels =
        HdEmbreeBalanceSubdivisionLevels(
            faceVertexCounts, faceVertexIndices, candidateLevels);
    if (baselineLevels.empty() || !displacedPositionProbe) {
        return baselineLevels;
    }

    const std::optional<_SharedEdgeGroups> sharedEdges =
        _BuildSharedEdgeGroups(faceVertexCounts, faceVertexIndices);
    if (!sharedEdges) {
        return baselineLevels;
    }
    const std::optional<std::vector<size_t>> edgeStripComponents =
        _BuildQuadEdgeStripComponents(faceVertexCounts, *sharedEdges);
    if (!edgeStripComponents) {
        return baselineLevels;
    }

    std::vector<bool> boostedEdgeStrips(sharedEdges->groupCount, false);
    const auto markDirectionForBoost =
        [&sharedEdges, &edgeStripComponents, &boostedEdgeStrips](
            size_t firstEdge, size_t secondEdge) {
            for (const size_t edge : {firstEdge, secondEdge}) {
                const size_t sharedGroup =
                    sharedEdges->cornerGroupIndices[edge];
                boostedEdgeStrips[(*edgeStripComponents)[sharedGroup]] = true;
            }
        };
    const double targetErrorPixels =
        _GetTargetDisplacementChordErrorPixels(refineLevel);
    constexpr size_t probeCount =
        _displacementProbeGridWidth * _displacementProbeGridWidth;
    size_t faceOffset = 0;
    for (size_t faceIndex = 0;
         faceIndex < faceVertexCounts.size(); ++faceIndex) {
        const int count = faceVertexCounts[faceIndex];
        if (count != 4) {
            faceOffset += static_cast<size_t>(count);
            continue;
        }
        if (faceIndex > std::numeric_limits<unsigned int>::max()) {
            return baselineLevels;
        }

        std::array<GfVec3f, probeCount> samples;
        bool samplesValid = true;
        for (size_t vIndex = 0;
             vIndex < _displacementProbeGridWidth && samplesValid;
             ++vIndex) {
            for (size_t uIndex = 0;
                 uIndex < _displacementProbeGridWidth; ++uIndex) {
                GfVec3f& sample = samples[
                    vIndex * _displacementProbeGridWidth + uIndex];
                if (!displacedPositionProbe(
                        static_cast<unsigned int>(faceIndex),
                        _displacementProbeCoordinates[uIndex],
                        _displacementProbeCoordinates[vIndex],
                        &sample) ||
                    !_IsFinite(sample)) {
                    samplesValid = false;
                    break;
                }
            }
        }
        if (!samplesValid) {
            faceOffset += static_cast<size_t>(count);
            continue;
        }

        double maximumUError = 0.0;
        double maximumVError = 0.0;
        for (GfMatrix4d const& matrix : objectToClip) {
            for (size_t row = 0;
                 row < _displacementProbeGridWidth; ++row) {
                const size_t rowStart =
                    row * _displacementProbeGridWidth;
                double error = 0.0;
                if (_ComputeProjectedChordError(
                        samples[rowStart], samples[rowStart + 1],
                        samples[rowStart + 2], matrix,
                        width, height, &error)) {
                    maximumUError = std::max(maximumUError, error);
                }
            }
            for (size_t column = 0;
                 column < _displacementProbeGridWidth; ++column) {
                double error = 0.0;
                if (_ComputeProjectedChordError(
                        samples[column],
                        samples[column + _displacementProbeGridWidth],
                        samples[column +
                            2 * _displacementProbeGridWidth],
                        matrix, width, height, &error)) {
                    maximumVError = std::max(maximumVError, error);
                }
            }
        }

        // For a quad, edges 0/2 control segments in the u direction and
        // edges 1/3 control segments in the v direction. Record the boost on
        // the complete edge strip rather than changing this face immediately:
        // a one-sided change would make Embree stitch unequal opposite levels
        // with large transition triangles that can fold after displacement.
        if (maximumUError > targetErrorPixels) {
            markDirectionForBoost(faceOffset, faceOffset + 2);
        }
        if (maximumVError > targetErrorPixels) {
            markDirectionForBoost(faceOffset + 1, faceOffset + 3);
        }
        faceOffset += static_cast<size_t>(count);
    }

    std::vector<float> displacementLevels = baselineLevels;
    for (size_t edge = 0; edge < displacementLevels.size(); ++edge) {
        const size_t sharedGroup =
            sharedEdges->cornerGroupIndices[edge];
        if (boostedEdgeStrips[(*edgeStripComponents)[sharedGroup]]) {
            displacementLevels[edge] = std::min(
                _maxSubdivisionLevel,
                baselineLevels[edge] * _maxDisplacementLevelScale);
        }
    }

    return HdEmbreeBalanceSubdivisionLevels(
        faceVertexCounts, faceVertexIndices, displacementLevels);
}

PXR_NAMESPACE_CLOSE_SCOPE
