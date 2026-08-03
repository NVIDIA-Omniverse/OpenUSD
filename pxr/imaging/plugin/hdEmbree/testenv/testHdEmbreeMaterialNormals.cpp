//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include <renderer/geometry/context.h>
#include <renderer/geometry/triangleMesh.h>
#include <renderer/integrator/shadingNormal.h>

#include "pxr/base/gf/vec2f.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/pxr.h"

#include <embree4/rtcore.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>

PXR_NAMESPACE_USING_DIRECTIVE

static bool
_IsClose(
    GfVec3f const& actual,
    GfVec3f const& expected,
    float tolerance = 1.0e-6f)
{
    return std::abs(actual[0] - expected[0]) <= tolerance &&
           std::abs(actual[1] - expected[1]) <= tolerance &&
           std::abs(actual[2] - expected[2]) <= tolerance;
}

static bool
_TestNormalResolution()
{
    const GfVec3f normalSrfWldExt(0.0f, 0.0f, 1.0f);
    const GfVec3f normalCandidateWldExt(-8.0f, 0.0f, 6.0f);
    const GfVec3f normalGeomWldExt(0.8f, 0.0f, 0.6f);
    GfVec3f resolved;
    if (GfDot(normalCandidateWldExt, normalGeomWldExt) >= 0.0f ||
        !ty::TryResolveNormalShdWldExt(
            normalCandidateWldExt, normalSrfWldExt, &resolved) ||
        !_IsClose(resolved, GfVec3f(-0.8f, 0.0f, 0.6f))) {
        return false;
    }

    const GfVec3f omegaOutWld =
        GfVec3f(0.0f, 0.2f, 0.98f).GetNormalized();
    const GfVec3f normalViewBaseWldOut(0.0f, 1.0f, 0.0f);
    const GfVec3f viewBackfacingNormal =
        GfVec3f(0.0f, 0.1f, -0.995f).GetNormalized();
    return
        GfDot(viewBackfacingNormal, normalViewBaseWldOut) > 0.0f &&
        GfDot(viewBackfacingNormal, omegaOutWld) < 0.0f &&
        ty::TryResolveNormalShdWldExt(
            viewBackfacingNormal, normalViewBaseWldOut, &resolved) &&
        _IsClose(resolved, viewBackfacingNormal);
}

static bool
_TestExteriorNormalFacing()
{
    const GfVec3f normalShdWldExt =
        GfVec3f(0.3f, -0.4f, 0.8660254f).GetNormalized();
    const GfVec3f normalShdFrontWldOut =
        ty::FaceNormalShdWldOut(normalShdWldExt, true);
    const GfVec3f normalShdBackWldOut =
        ty::FaceNormalShdWldOut(normalShdWldExt, false);
    return
        _IsClose(normalShdFrontWldOut, normalShdWldExt) &&
        _IsClose(normalShdBackWldOut, -normalShdWldExt) &&
        _IsClose(normalShdFrontWldOut, -normalShdBackWldOut);
}

static bool
_TestInvalidNormals()
{
    const GfVec3f normalSrfWldOut(0.0f, 0.0f, 1.0f);
    const float infinity = std::numeric_limits<float>::infinity();
    GfVec3f resolved;
    return
        !ty::TryResolveNormalShdWldExt(
            GfVec3f(0.0f), normalSrfWldOut, &resolved) &&
        !ty::TryResolveNormalShdWldExt(
            GfVec3f(0.0f, 0.0f, -1.0f),
            normalSrfWldOut, &resolved) &&
        !ty::TryResolveNormalShdWldExt(
            GfVec3f(infinity, 0.0f, 1.0f),
            normalSrfWldOut, &resolved);
}

static bool
_TestSmoothShadowOffset()
{
    const GfVec3f p0(1.0f, 0.0f, 0.0f);
    const GfVec3f p1(0.0f, 1.0f, 0.0f);
    const GfVec3f p2(0.0f, 0.0f, 1.0f);
    const GfVec3f normalGeomWldExt =
        GfCross(p1 - p0, p2 - p0).GetNormalized();
    const GfVec3f offset = ty::ComputeSmoothTriangleShadowOffset(
        p0, p1, p2, p0, p1, p2, 0.25f, 0.25f, normalGeomWldExt);
    const GfVec3f normalSmooth =
        (p0 * 0.5f + p1 * 0.25f + p2 * 0.25f).GetNormalized();
    if (GfDot(offset, normalSmooth) <= 0.0f) {
        return false;
    }

    return _IsClose(
        ty::ComputeSmoothTriangleShadowOffset(
            p0, p1, p2, normalGeomWldExt, normalGeomWldExt,
            normalGeomWldExt, 0.25f, 0.25f, normalGeomWldExt),
        GfVec3f(0.0f)) &&
        _IsClose(
            ty::ComputeSmoothTriangleShadowOffset(
                GfVec3f(0.0f), GfVec3f(1.0f, 0.0f, 0.0f),
                GfVec3f(2.0f, 0.0f, 0.0f), normalGeomWldExt,
                normalGeomWldExt, normalGeomWldExt,
                0.25f, 0.25f, normalGeomWldExt),
            GfVec3f(0.0f));
}

static bool
_TestSmoothShadowOffsetUsesTriangleCorners()
{
    VtVec3iArray triangleIndices{GfVec3i(0, 1, 2)};
    VtVec3fArray points{
        GfVec3f(1.0f, 0.0f, 0.0f),
        GfVec3f(0.0f, 1.0f, 0.0f),
        GfVec3f(0.0f, 0.0f, 1.0f)};
    VtVec3fArray dPdu{GfVec3f(100.0f, 0.0f, 0.0f)};
    VtVec3fArray dPdv{GfVec3f(0.0f, 0.01f, 0.0f)};
    ty::PrototypeContext context;
    context.triangleIndices = &triangleIndices;
    context.points = &points;
    context.triangleDPdu = &dPdu;
    context.triangleDPdv = &dPdv;

    ty::InstanceContext instanceContext;
    instanceContext.objectToWorldMatrix = GfMatrix4f(1.0f);
    instanceContext.worldToObjectMatrix = GfMatrix4f(1.0f);
    const GfVec3f normalGeomWldExt =
        GfCross(points[1] - points[0], points[2] - points[0])
            .GetNormalized();
    const auto computeOffset = [&]() {
        return ty::ComputeSmoothTriangleShadowOffsetFromContext(
            &context, &instanceContext, 0,
            points[0], points[1], points[2],
            0.25f, 0.25f, normalGeomWldExt);
    };

    const GfVec3f expected = ty::ComputeSmoothTriangleShadowOffset(
        points[0], points[1], points[2],
        points[0], points[1], points[2],
        0.25f, 0.25f, normalGeomWldExt);
    const GfVec3f baseline = computeOffset();
    const GfVec3f posWld =
        ty::InterpolateTrianglePosition(
            points[0], points[1], points[2], 0.25f, 0.25f);
    const GfVec3f pseudoP0 =
        posWld - 0.25f * dPdu[0] - 0.25f * dPdv[0];
    const GfVec3f oldPseudoOffset =
        ty::ComputeSmoothTriangleShadowOffset(
            pseudoP0, pseudoP0 + dPdu[0], pseudoP0 + dPdv[0],
            points[0], points[1], points[2],
            0.25f, 0.25f, normalGeomWldExt);
    if (expected.GetLengthSq() <= 1.0e-12f ||
        !_IsClose(baseline, expected) ||
        _IsClose(oldPseudoOffset, expected, 1.0e-4f)) {
        return false;
    }
    dPdu[0] *= 1000.0f;
    dPdv[0] *= 0.001f;
    return _IsClose(computeOffset(), baseline);
}

static bool
_TestTrianglePositionMatchesEmbree()
{
    struct PaddedVertex
    {
        float x;
        float y;
        float z;
        float padding;
    };

    const std::array<GfVec3f, 3> points{
        GfVec3f(16777216.0f, -33554432.0f, 1048576.0f),
        GfVec3f(16777220.0f, -33554424.0f, 1048577.0f),
        GfVec3f(16777208.0f, -33554440.0f, 1048575.0f)};
    const std::array<PaddedVertex, 3> vertices{{
        {points[0][0], points[0][1], points[0][2], 0.0f},
        {points[1][0], points[1][1], points[1][2], 0.0f},
        {points[2][0], points[2][1], points[2][2], 0.0f}}};
    const std::array<unsigned int, 3> indices{0, 1, 2};

    RTCDevice device = rtcNewDevice(nullptr);
    if (!device) {
        return false;
    }
    RTCGeometry geometry = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_TRIANGLE);
    if (!geometry) {
        rtcReleaseDevice(device);
        return false;
    }
    rtcSetSharedGeometryBuffer(
        geometry, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3,
        vertices.data(), 0, sizeof(PaddedVertex), vertices.size());
    rtcSetSharedGeometryBuffer(
        geometry, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3,
        indices.data(), 0, sizeof(indices), 1);
    rtcCommitGeometry(geometry);

    bool matches = true;
    for (const GfVec2f& bary : {
             GfVec2f(0.25f, 0.25f),
             GfVec2f(0.49999997f, 0.25f),
             GfVec2f(0.125f, 0.75f)}) {
        alignas(16) float sampled[4] = {};
        rtcInterpolate1(
            geometry, 0, bary[0], bary[1], RTC_BUFFER_TYPE_VERTEX, 0,
            sampled, nullptr, nullptr, 3);
        const GfVec3f expected(sampled[0], sampled[1], sampled[2]);
        const GfVec3f actual = ty::InterpolateTrianglePosition(
            points[0], points[1], points[2], bary[0], bary[1]);
        if (actual != expected) {
            matches = false;
        }
    }

    rtcReleaseGeometry(geometry);
    rtcReleaseDevice(device);
    return matches;
}

static bool
_TestShadowTerminatorWeight()
{
    const GfVec3f normalSrfWldOut(0.0f, 0.0f, 1.0f);
    const GfVec3f normalGeomWldOut =
        GfVec3f(0.8f, 0.0f, 0.6f).GetNormalized();
    const GfVec3f omegaClearReflectionWld(0.0f, 0.0f, 1.0f);
    const GfVec3f omegaTerminatedReflectionWld =
        GfVec3f(-1.0f, 0.0f, 0.05f).GetNormalized();
    const GfVec3f omegaClearTransmissionWld(0.0f, 0.0f, -1.0f);
    const GfVec3f omegaTerminatedTransmissionWld =
        GfVec3f(1.0f, 0.0f, -0.05f).GetNormalized();
    return
        ty::ComputeShadowTerminatorOffsetWeight(
            normalSrfWldOut, normalGeomWldOut,
            omegaClearReflectionWld) == 0.0f &&
        ty::ComputeShadowTerminatorOffsetWeight(
            normalSrfWldOut, normalGeomWldOut,
            omegaTerminatedReflectionWld) == 1.0f &&
        ty::ComputeShadowTerminatorOffsetWeight(
            normalSrfWldOut, normalGeomWldOut,
            omegaClearTransmissionWld) == 0.0f &&
        ty::ComputeShadowTerminatorOffsetWeight(
            normalSrfWldOut, normalGeomWldOut,
            omegaTerminatedTransmissionWld) == 1.0f;
}

int
main()
{
    bool passed = true;
    if (!_TestNormalResolution()) {
        std::printf("  normal resolution failed\n");
        passed = false;
    }
    if (!_TestInvalidNormals()) {
        std::printf("  invalid-normal rejection failed\n");
        passed = false;
    }
    if (!_TestExteriorNormalFacing()) {
        std::printf("  exterior normal facing failed\n");
        passed = false;
    }
    if (!_TestSmoothShadowOffset()) {
        std::printf("  smooth shadow offset failed\n");
        passed = false;
    }
    if (!_TestSmoothShadowOffsetUsesTriangleCorners()) {
        std::printf("  smooth shadow triangle-corner regression failed\n");
        passed = false;
    }
    if (!_TestTrianglePositionMatchesEmbree()) {
        std::printf("  triangle interpolation differs from Embree\n");
        passed = false;
    }
    if (!_TestShadowTerminatorWeight()) {
        std::printf("  shadow terminator weight failed\n");
        passed = false;
    }
    std::printf(
        "testHdEmbreeMaterialNormals: %s\n",
        passed ? "PASS" : "FAIL");
    return passed ? 0 : 1;
}
