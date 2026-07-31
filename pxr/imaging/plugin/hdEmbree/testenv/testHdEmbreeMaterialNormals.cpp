//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include <renderer/integrator/shadingNormal.h>

#include "pxr/base/gf/vec3f.h"
#include "pxr/pxr.h"

#include <cmath>
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
    const GfVec3f normalSrfWldOut(0.0f, 0.0f, 1.0f);
    const GfVec3f normalCandidateWldOut(-8.0f, 0.0f, 6.0f);
    const GfVec3f normalGeomWldExt(0.8f, 0.0f, 0.6f);
    GfVec3f resolved;
    if (GfDot(normalCandidateWldOut, normalGeomWldExt) >= 0.0f ||
        !ty::TryResolveNormalShdWldOut(
            normalCandidateWldOut, normalSrfWldOut, &resolved) ||
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
        ty::TryResolveNormalShdWldOut(
            viewBackfacingNormal, normalViewBaseWldOut, &resolved) &&
        _IsClose(resolved, viewBackfacingNormal);
}

static bool
_TestInvalidNormals()
{
    const GfVec3f normalSrfWldOut(0.0f, 0.0f, 1.0f);
    const float infinity = std::numeric_limits<float>::infinity();
    GfVec3f resolved;
    return
        !ty::TryResolveNormalShdWldOut(
            GfVec3f(0.0f), normalSrfWldOut, &resolved) &&
        !ty::TryResolveNormalShdWldOut(
            GfVec3f(0.0f, 0.0f, -1.0f),
            normalSrfWldOut, &resolved) &&
        !ty::TryResolveNormalShdWldOut(
            GfVec3f(infinity, 0.0f, 1.0f),
            normalSrfWldOut, &resolved);
}

static bool
_TestBumpDirectionValidity()
{
    const GfVec3f normalSrfWldOut(0.0f, 0.0f, 1.0f);
    const GfVec3f normalShdWldOut(-0.8f, 0.0f, 0.6f);
    const GfVec3f omegaTransmissionWld =
        GfVec3f(0.1f, 0.0f, -1.0f).GetNormalized();
    const GfVec3f omegaDisagreementWld =
        GfVec3f(-1.0f, 0.0f, -0.1f).GetNormalized();
    const GfVec3f normalGeomWldOut =
        GfVec3f(0.8f, 0.0f, 0.6f).GetNormalized();
    const GfVec3f omegaReflectionBelowFacetWld =
        GfVec3f(-1.0f, 0.0f, 0.1f).GetNormalized();
    return
        ty::BumpDirectionIsValid(
            normalShdWldOut, normalSrfWldOut, omegaTransmissionWld) &&
        !ty::BumpDirectionIsValid(
            normalShdWldOut, normalSrfWldOut, omegaDisagreementWld) &&
        GfDot(normalGeomWldOut, omegaReflectionBelowFacetWld) < 0.0f &&
        ty::BumpDirectionIsValid(
            normalShdWldOut, normalSrfWldOut,
            omegaReflectionBelowFacetWld);
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
        GfVec3f(0.0f));
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
    if (!_TestBumpDirectionValidity()) {
        std::printf("  bump-direction validity failed\n");
        passed = false;
    }
    if (!_TestSmoothShadowOffset()) {
        std::printf("  smooth shadow offset failed\n");
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
