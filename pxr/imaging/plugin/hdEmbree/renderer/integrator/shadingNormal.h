//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_IMAGING_PLUGIN_HDEMBREE_RENDERER_INTEGRATOR_SHADING_NORMAL_H
#define PXR_IMAGING_PLUGIN_HDEMBREE_RENDERER_INTEGRATOR_SHADING_NORMAL_H

#include <renderer/geometry/context.h>
#include <renderer/geometry/normalTransforms.h>
#include <renderer/geometry/triangleMesh.h>
#include <renderer/rendererMath.h>

#include "pxr/base/gf/vec3f.h"
#include "pxr/pxr.h"

#include <algorithm>
#include <cmath>

PXR_NAMESPACE_OPEN_SCOPE
namespace ty {

/// Normalizes `normalCandidateWldExt` into `outNormalShdWldExt`.
///
/// `normalSrfWldExt` must be a finite unit normal with authored exterior
/// orientation. The candidate is accepted only when finite, non-degenerate,
/// and in the same open hemisphere as that base normal.
/// `outNormalShdWldExt` must be non-null and is written only on success.
/// Returns false for every invalid
/// input and does not throw. This intentionally mirrors mxcpp closure-normal
/// validation in `Bsdf::detail::TryResolveShadingNormal`; the Gf/mxcpp type
/// boundary prevents sharing the implementation directly.
inline bool
TryResolveNormalShdWldExt(
    GfVec3f const& normalCandidateWldExt,
    GfVec3f const& normalSrfWldExt,
    GfVec3f* outNormalShdWldExt)
{
    if (!outNormalShdWldExt ||
        !std::isfinite(normalCandidateWldExt[0]) ||
        !std::isfinite(normalCandidateWldExt[1]) ||
        !std::isfinite(normalCandidateWldExt[2])) {
        return false;
    }

    const double maximumComponent = std::max({
        std::abs(static_cast<double>(normalCandidateWldExt[0])),
        std::abs(static_cast<double>(normalCandidateWldExt[1])),
        std::abs(static_cast<double>(normalCandidateWldExt[2]))});
    if (!std::isfinite(maximumComponent) || maximumComponent == 0.0) {
        return false;
    }

    const double scaledX =
        static_cast<double>(normalCandidateWldExt[0]) / maximumComponent;
    const double scaledY =
        static_cast<double>(normalCandidateWldExt[1]) / maximumComponent;
    const double scaledZ =
        static_cast<double>(normalCandidateWldExt[2]) / maximumComponent;
    const double scaledLength = std::sqrt(
        scaledX * scaledX + scaledY * scaledY + scaledZ * scaledZ);
    if (!std::isfinite(scaledLength) || scaledLength == 0.0) {
        return false;
    }

    const GfVec3f candidate(
        static_cast<float>(scaledX / scaledLength),
        static_cast<float>(scaledY / scaledLength),
        static_cast<float>(scaledZ / scaledLength));
    if (GfDot(candidate, normalSrfWldExt) <= 0.0f) {
        return false;
    }

    *outNormalShdWldExt = candidate;
    return true;
}

/// Faces finite unit exterior `normalShdWldExt` to the incident transport
/// side selected by the immutable geometric `frontFacing` classification.
/// The complete resolved normal is negated for a back-face hit so normal and
/// bump maps describe one view-independent exterior relief field.
inline GfVec3f
FaceNormalShdWldOut(
    GfVec3f const& normalShdWldExt,
    bool frontFacing)
{
    return frontFacing ? normalShdWldExt : -normalShdWldExt;
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
    const GfVec3f edge01 = p1 - p0;
    const GfVec3f edge02 = p2 - p0;
    if (!IsFinite(edge01) || !IsFinite(edge02) ||
        GfCross(edge01, edge02).GetLengthSq() <= 1.0e-18f) {
        return GfVec3f(0.0f);
    }

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

/// Fetch and transform genuine triangle corners, then compute their smooth
/// shadow offset. Corner normals correspond to the same primitive and remain
/// in object space on entry. Authored-st derivatives are not consumed.
/// Returns zero for refined/displaced meshes or invalid cached/transformed
/// inputs.
inline GfVec3f
ComputeSmoothTriangleShadowOffsetFromContext(
    PrototypeContext const* prototypeContext,
    InstanceContext const* instanceContext,
    unsigned int primitiveId,
    GfVec3f n0,
    GfVec3f n1,
    GfVec3f n2,
    float baryU,
    float baryV,
    GfVec3f const& normalGeomWldExt)
{
    if (!prototypeContext || !instanceContext ||
        prototypeContext->refined || prototypeContext->displaced) {
        return GfVec3f(0.0f);
    }

    GfVec3f p0;
    GfVec3f p1;
    GfVec3f p2;
    if (!SampleTrianglePositions(
            prototypeContext, primitiveId, &p0, &p1, &p2)) {
        return GfVec3f(0.0f);
    }

    p0 = instanceContext->objectToWorldMatrix.Transform(p0);
    p1 = instanceContext->objectToWorldMatrix.Transform(p1);
    p2 = instanceContext->objectToWorldMatrix.Transform(p2);
    n0 = TransformNormalToWorld(instanceContext, n0);
    n1 = TransformNormalToWorld(instanceContext, n1);
    n2 = TransformNormalToWorld(instanceContext, n2);
    if (!IsFinite(p0) || !IsFinite(p1) || !IsFinite(p2) ||
        !TryNormalizeDirection(n0, &n0) ||
        !TryNormalizeDirection(n1, &n1) ||
        !TryNormalizeDirection(n2, &n2)) {
        return GfVec3f(0.0f);
    }
    if (GfDot(n0, normalGeomWldExt) < 0.0f) {
        n0 = -n0;
    }
    if (GfDot(n1, normalGeomWldExt) < 0.0f) {
        n1 = -n1;
    }
    if (GfDot(n2, normalGeomWldExt) < 0.0f) {
        n2 = -n2;
    }
    return ComputeSmoothTriangleShadowOffset(
        p0, p1, p2, n0, n1, n2, baryU, baryV, normalGeomWldExt);
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
