//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_IMAGING_PLUGIN_HDEMBREE_RENDERER_INTEGRATOR_SHADING_NORMAL_H
#define PXR_IMAGING_PLUGIN_HDEMBREE_RENDERER_INTEGRATOR_SHADING_NORMAL_H

#include "pxr/base/gf/vec3f.h"
#include "pxr/pxr.h"

#include <algorithm>
#include <cmath>

PXR_NAMESPACE_OPEN_SCOPE
namespace ty {

/// Normalizes `normalCandidateWldOut` into `outNormalShdWldOut`.
///
/// `normalSrfWldOut` must be a finite unit normal on the incident transport
/// side. The candidate is accepted only when finite, non-degenerate, and in
/// the same open hemisphere as that base normal. `outNormalShdWldOut` must be
/// non-null and is written only on success. Returns false for every invalid
/// input and does not throw. This intentionally mirrors mxcpp closure-normal
/// validation in `Bsdf::detail::TryResolveShadingNormal`; the Gf/mxcpp type
/// boundary prevents sharing the implementation directly.
inline bool
TryResolveNormalShdWldOut(
    GfVec3f const& normalCandidateWldOut,
    GfVec3f const& normalSrfWldOut,
    GfVec3f* outNormalShdWldOut)
{
    if (!outNormalShdWldOut ||
        !std::isfinite(normalCandidateWldOut[0]) ||
        !std::isfinite(normalCandidateWldOut[1]) ||
        !std::isfinite(normalCandidateWldOut[2])) {
        return false;
    }

    const double maximumComponent = std::max({
        std::abs(static_cast<double>(normalCandidateWldOut[0])),
        std::abs(static_cast<double>(normalCandidateWldOut[1])),
        std::abs(static_cast<double>(normalCandidateWldOut[2]))});
    if (!std::isfinite(maximumComponent) || maximumComponent == 0.0) {
        return false;
    }

    const double scaledX =
        static_cast<double>(normalCandidateWldOut[0]) / maximumComponent;
    const double scaledY =
        static_cast<double>(normalCandidateWldOut[1]) / maximumComponent;
    const double scaledZ =
        static_cast<double>(normalCandidateWldOut[2]) / maximumComponent;
    const double scaledLength = std::sqrt(
        scaledX * scaledX + scaledY * scaledY + scaledZ * scaledZ);
    if (!std::isfinite(scaledLength) || scaledLength == 0.0) {
        return false;
    }

    const GfVec3f candidate(
        static_cast<float>(scaledX / scaledLength),
        static_cast<float>(scaledY / scaledLength),
        static_cast<float>(scaledZ / scaledLength));
    if (GfDot(candidate, normalSrfWldOut) <= 0.0f) {
        return false;
    }

    *outNormalShdWldOut = candidate;
    return true;
}

/// Returns whether `omegaInWld` represents the same event in the finite unit
/// material and smooth-base frames. All inputs must use the incident transport
/// side and world space. Tangent directions are accepted at the closed
/// boundary. Does not throw.
inline bool
BumpDirectionIsValid(
    GfVec3f const& normalShdWldOut,
    GfVec3f const& normalSrfWldOut,
    GfVec3f const& omegaInWld)
{
    return
        GfDot(normalSrfWldOut, omegaInWld) *
        GfDot(normalSrfWldOut, normalShdWldOut) *
        GfDot(normalShdWldOut, omegaInWld) >=
        0.0f;
}

/// Approximates the lift from a triangle facet to its smooth surface.
///
/// `p0` through `p2` and finite unit `n0` through `n2` are corresponding
/// world-space triangle corners. `u` and `v` are Embree barycentrics and
/// `normalGeomWldExt` is the finite unit exterior facet normal. Returns a
/// finite world-space offset, or zero when the approximation is non-finite.
/// Does not throw.
inline GfVec3f
ComputeSmoothTriangleShadowOffset(
    GfVec3f const& p0,
    GfVec3f const& p1,
    GfVec3f const& p2,
    GfVec3f const& n0,
    GfVec3f const& n1,
    GfVec3f const& n2,
    float u,
    float v,
    GfVec3f const& normalGeomWldExt)
{
    const float bary0 = 1.0f - u - v;
    const GfVec3f posWld = p0 * bary0 + p1 * u + p2 * v;
    const GfVec3f normalWld = n0 * bary0 + n1 * u + n2 * v;

    const float lift20 = GfDot(n2 - n0, p0 - p2);
    const float lift21 = GfDot(n2 - n1, p1 - p2);
    const float lift10 = GfDot(n1 - n0, p1 - p0);
    float lift =
        lift20 * bary0 * (bary0 - 1.0f) +
        (lift20 + lift21 + lift10) * bary0 * u +
        lift21 * u * (u - 1.0f);

    if (GfDot(normalWld, normalGeomWldExt) > 0.0f) {
        float height0 = std::max({
            GfDot(p1 - p0, n0), GfDot(p2 - p0, n0), 0.0f});
        float height1 = std::max({
            GfDot(p0 - p1, n1), GfDot(p2 - p1, n1), 0.0f});
        float height2 = std::max({
            GfDot(p0 - p2, n2), GfDot(p1 - p2, n2), 0.0f});
        height0 = std::max(GfDot(p0 - posWld, n0) + height0, 0.0f);
        height1 = std::max(GfDot(p1 - posWld, n1) + height1, 0.0f);
        height2 = std::max(GfDot(p2 - posWld, n2) + height2, 0.0f);
        lift = std::max(
            std::min({height0, height1, height2}), lift * 0.5f);
    } else {
        float height0 = std::max({
            GfDot(p0 - p1, n0), GfDot(p0 - p2, n0), 0.0f});
        float height1 = std::max({
            GfDot(p1 - p0, n1), GfDot(p1 - p2, n1), 0.0f});
        float height2 = std::max({
            GfDot(p2 - p0, n2), GfDot(p2 - p1, n2), 0.0f});
        height0 = std::max(GfDot(posWld - p0, n0) + height0, 0.0f);
        height1 = std::max(GfDot(posWld - p1, n1) + height1, 0.0f);
        height2 = std::max(GfDot(posWld - p2, n2) + height2, 0.0f);
        lift = std::min(
            -std::min({height0, height1, height2}), lift * 0.5f);
    }

    if (!std::isfinite(lift) ||
        !std::isfinite(normalWld[0]) ||
        !std::isfinite(normalWld[1]) ||
        !std::isfinite(normalWld[2])) {
        return GfVec3f(0.0f);
    }
    return normalWld * lift;
}

/// Returns the smooth-terminator lift weight for `omegaInWld`.
///
/// The normals and direction must be finite unit world-space values on the
/// incident transport side. `cutoff` must be finite; non-positive values
/// disable the correction. Returns a finite value in [0, 1]. Does not throw.
inline float
ComputeShadowTerminatorOffsetWeight(
    GfVec3f const& normalSrfWldOut,
    GfVec3f const& normalGeomWldOut,
    GfVec3f const& omegaInWld,
    float cutoff = 0.1f)
{
    if (!(cutoff > 0.0f)) {
        return 0.0f;
    }

    const float baseDot = GfDot(normalSrfWldOut, omegaInWld);
    const bool transmission = baseDot < 0.0f;
    const float normalDot = std::abs(baseDot);
    const GfVec3f normalGeomSideWld =
        transmission ? -normalGeomWldOut : normalGeomWldOut;
    const float facetDot = GfDot(normalGeomSideWld, omegaInWld);
    const float weight = normalDot < cutoff
        ? 2.0f - (facetDot + normalDot) / cutoff
        : 1.0f - facetDot / cutoff;
    return std::clamp(weight, 0.0f, 1.0f);
}

}  // namespace ty
PXR_NAMESPACE_CLOSE_SCOPE

#endif
