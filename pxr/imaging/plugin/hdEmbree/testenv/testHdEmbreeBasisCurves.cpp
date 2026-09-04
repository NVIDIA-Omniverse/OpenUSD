//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//

#include <delegate/basisCurves.h>
#include <delegate/renderBuffer.h>
#include <delegate/renderDelegate.h>
#include <delegate/renderParam.h>
#include <renderer/geometry/context.h>
#include <renderer/geometry/curveEmbree.h>
#include <renderer/geometry/curveGeometry.h>
#include <renderer/geometry/curveSamplers.h>
#include <renderer/geometry/curveTopology.h>
#include <renderer/renderer.h>
#include <renderer/rendererMath.h>

#include "pxr/base/tf/diagnosticTrap.h"
#include "pxr/base/gf/rect2i.h"
#include "pxr/imaging/hd/basisCurvesSchema.h"
#include "pxr/imaging/hd/basisCurvesTopology.h"
#include "pxr/imaging/hd/basisCurvesTopologySchema.h"
#include "pxr/imaging/hd/extComputation.h"
#include "pxr/imaging/hd/extComputationCpuCallback.h"
#include "pxr/imaging/hd/extComputationContext.h"
#include "pxr/imaging/hd/extComputationOutputSchema.h"
#include "pxr/imaging/hd/extComputationPrimvarSchema.h"
#include "pxr/imaging/hd/extComputationPrimvarsSchema.h"
#include "pxr/imaging/hd/extComputationSchema.h"
#include "pxr/imaging/hd/geomSubsetSchema.h"
#include "pxr/imaging/hd/instancer.h"
#include "pxr/imaging/hd/material.h"
#include "pxr/imaging/hd/primvarSchema.h"
#include "pxr/imaging/hd/primvarsSchema.h"
#include "pxr/imaging/hd/retainedDataSource.h"
#include "pxr/imaging/hd/retainedSceneIndex.h"
#include "pxr/imaging/hd/renderIndex.h"
#include "pxr/imaging/hd/renderPass.h"
#include "pxr/imaging/hd/renderThread.h"
#include "pxr/imaging/hd/selection.h"
#include "pxr/imaging/hd/repr.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/hd/visibilitySchema.h"
#include "pxr/imaging/hdx/pickTask.h"
#include "pxr/imaging/hdx/unitTestUtils.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usdGeom/basisCurves.h"
#include "pxr/usdImaging/usdImaging/basisCurvesAdapter.h"
#include "pxr/usdImaging/usdImaging/dataSourceStageGlobals.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

class _StageGlobals final : public UsdImagingDataSourceStageGlobals
{
public:
    UsdTimeCode GetTime() const override
    {
        return UsdTimeCode::Default();
    }

    void FlagAsTimeVarying(
        SdfPath const&,
        HdDataSourceLocator const&) const override
    {
    }

    void FlagAsAssetPathDependent(SdfPath const&) const override
    {
    }
};

struct _ControlExpectation
{
    ty::CurveControlKind kind;
    size_t logicalControlIndex;
    size_t neighborLogicalControlIndex;
};

ty::CurveTopologyInput
_MakeInput(
    TfToken const& curveType,
    TfToken const& curveBasis,
    TfToken const& curveWrap,
    VtIntArray const& curveVertexCounts,
    size_t physicalPointCount,
    VtIntArray const& curveIndices = VtIntArray(),
    VtIntArray const& invisibleCurves = VtIntArray(),
    VtIntArray const& invisiblePoints = VtIntArray())
{
    ty::CurveTopologyInput input;
    input.curveType = curveType;
    input.curveBasis = curveBasis;
    input.curveWrap = curveWrap;
    input.curveVertexCounts = curveVertexCounts;
    input.curveIndices = curveIndices;
    input.invisibleCurves = invisibleCurves;
    input.invisiblePoints = invisiblePoints;
    input.physicalPointCount = physicalPointCount;
    return input;
}

ty::CurvePrimvarInput
_MakePrimvar(
    VtValue const& value,
    TfToken const& interpolation,
    VtIntArray const& indices = VtIntArray(),
    bool hasIndices = false)
{
    ty::CurvePrimvarInput result;
    result.authored = true;
    result.value = value;
    result.interpolation = interpolation;
    result.hasIndices = hasIndices || !indices.empty();
    result.indices = indices;
    return result;
}

template <typename T>
ty::CurvePrimvarInput
_MakePrimvar(
    VtArray<T> const& value,
    TfToken const& interpolation,
    VtIntArray const& indices = VtIntArray(),
    bool hasIndices = false)
{
    return _MakePrimvar(
        VtValue(value), interpolation, indices, hasIndices);
}

ty::CurveGeometryInput
_MakeGeometryInput(
    VtVec3fArray const& points,
    float minimumWidth = ty::DefaultMinimumCurveWidth)
{
    ty::CurveGeometryInput result;
    result.points = VtValue(points);
    result.minimumWidth = minimumWidth;
    return result;
}

bool
_Close(float first, float second, float tolerance = 1.0e-5f)
{
    return std::abs(first - second) <= tolerance;
}

bool
_Close(
    GfVec3f const& first,
    GfVec3f const& second,
    float tolerance = 1.0e-5f)
{
    return (first - second).GetLength() <= tolerance;
}

bool
_Close(
    GfMatrix4f const& first,
    GfMatrix4f const& second,
    float tolerance = 1.0e-5f)
{
    for (size_t row = 0; row < 4; ++row) {
        for (size_t column = 0; column < 4; ++column) {
            if (!_Close(first[row][column], second[row][column], tolerance)) {
                return false;
            }
        }
    }
    return true;
}

bool
_ExpectGeometryValid(
    ty::CurveGeometryResult const& result,
    char const* context)
{
    if (result.IsValid()) {
        return true;
    }
    std::printf(
        "    %s: expected geometry success, got %s\n",
        context,
        ty::GetCurveGeometryErrorName(result.diagnostic.error));
    return false;
}

bool
_ExpectGeometryError(
    ty::CurveGeometryResult const& result,
    ty::CurveGeometryError expected,
    char const* context)
{
    if (!result.IsValid() &&
        result.diagnostic.error == expected &&
        result.primitives.empty()) {
        return true;
    }
    std::printf(
        "    %s: expected geometry error %s, got %s with %zu plans\n",
        context,
        ty::GetCurveGeometryErrorName(expected),
        ty::GetCurveGeometryErrorName(result.diagnostic.error),
        result.primitives.size());
    return false;
}

bool
_Require(bool condition, char const* context, char const* message)
{
    if (!condition) {
        std::printf("    %s: %s\n", context, message);
    }
    return condition;
}

bool
_HasNoCanonicalPayload(ty::CurveTopologyResult const& result)
{
    return result.logicalControlDomainSize == 0 &&
        result.referencedPointDomainSize == 0 &&
        result.physicalPointCount == 0 &&
        result.constantDomainSize == 0 &&
        result.uniformDomainSize == 0 &&
        result.varyingDomainSize == 0 &&
        result.authoredSegmentCount == 0 &&
        result.vstep == 0 &&
        result.curves.empty() &&
        result.logicalToPhysicalPointIndices.empty() &&
        result.segments.empty() &&
        result.recovery.ignoredInvisibleCurveCount == 0 &&
        result.recovery.ignoredInvisiblePointCount == 0;
}

bool
_ExpectValid(
    ty::CurveTopologyResult const& result,
    char const* context)
{
    if (result.IsValid()) {
        return true;
    }
    std::printf(
        "    %s: expected success, got %s\n",
        context,
        ty::GetCurveTopologyErrorName(result.diagnostic.error));
    return false;
}

bool
_ExpectError(
    ty::CurveTopologyResult const& result,
    ty::CurveTopologyError expected,
    char const* context)
{
    if (result.IsValid() || result.diagnostic.error != expected) {
        std::printf(
            "    %s: expected %s, got %s\n",
            context,
            ty::GetCurveTopologyErrorName(expected),
            ty::GetCurveTopologyErrorName(result.diagnostic.error));
        return false;
    }
    if (!_HasNoCanonicalPayload(result)) {
        std::printf("    %s: fatal result retained canonical payload\n", context);
        return false;
    }
    return true;
}

bool
_CheckControl(
    ty::CurveControlReference const& control,
    _ControlExpectation const& expected,
    char const* context,
    size_t controlIndex)
{
    if (control.kind == expected.kind &&
        control.logicalControlIndex == expected.logicalControlIndex &&
        control.neighborLogicalControlIndex ==
            expected.neighborLogicalControlIndex) {
        return true;
    }
    std::printf(
        "    %s: control %zu was kind/index/neighbor %d/%zu/%zu, "
        "expected %d/%zu/%zu\n",
        context,
        controlIndex,
        static_cast<int>(control.kind),
        control.logicalControlIndex,
        control.neighborLogicalControlIndex,
        static_cast<int>(expected.kind),
        expected.logicalControlIndex,
        expected.neighborLogicalControlIndex);
    return false;
}

bool
_CheckControls(
    ty::CanonicalCurveSegment const& segment,
    std::initializer_list<_ControlExpectation> expected,
    char const* context)
{
    if (segment.controlCount != expected.size()) {
        std::printf(
            "    %s: got %u controls, expected %zu\n",
            context,
            static_cast<unsigned int>(segment.controlCount),
            expected.size());
        return false;
    }
    size_t controlIndex = 0;
    for (_ControlExpectation const& expectation : expected) {
        if (!_CheckControl(
                segment.controls[controlIndex],
                expectation,
                context,
                controlIndex)) {
            return false;
        }
        ++controlIndex;
    }
    return true;
}

bool
_SameMetadata(
    ty::CurveSegmentMetadata const& first,
    ty::CurveSegmentMetadata const& second)
{
    return first.authoredCurveId == second.authoredCurveId &&
        first.authoredSegmentId == second.authoredSegmentId &&
        first.authoredU0 == second.authoredU0 &&
        first.authoredU1 == second.authoredU1 &&
        first.isAuthoredCurveStart == second.isAuthoredCurveStart &&
        first.isAuthoredCurveEnd == second.isAuthoredCurveEnd;
}

bool
_SameDomain(
    ty::CurveAuthoredDomain const& first,
    ty::CurveAuthoredDomain const& second)
{
    return first.logicalControlOffset == second.logicalControlOffset &&
        first.logicalControlCount == second.logicalControlCount &&
        first.authoredSegmentOffset == second.authoredSegmentOffset &&
        first.authoredSegmentCount == second.authoredSegmentCount &&
        first.varyingOffset == second.varyingOffset &&
        first.varyingCount == second.varyingCount;
}

bool
_CheckSingleCurveSummary(
    ty::CurveTopologyResult const& result,
    size_t controlCount,
    size_t segmentCount,
    size_t varyingCount,
    std::uint8_t segmentControlCount,
    bool periodic,
    char const* context)
{
    if (!_Require(result.constantDomainSize == 1, context,
                  "constant domain was not one") ||
        !_Require(result.uniformDomainSize == 1, context,
                  "uniform domain was not one") ||
        !_Require(result.logicalControlDomainSize == controlCount, context,
                  "logical control domain was incorrect") ||
        !_Require(result.referencedPointDomainSize == controlCount, context,
                  "referenced point domain was incorrect") ||
        !_Require(result.physicalPointCount == controlCount, context,
                  "physical point domain was incorrect") ||
        !_Require(result.varyingDomainSize == varyingCount, context,
                  "varying domain was incorrect") ||
        !_Require(result.authoredSegmentCount == segmentCount, context,
                  "authored segment total was incorrect") ||
        !_Require(result.curves.size() == 1, context,
                  "expected one authored curve") ||
        !_Require(result.segments.size() == segmentCount, context,
                  "visible segment count was incorrect")) {
        return false;
    }

    ty::CurveAuthoredDomain const& domain = result.curves[0];
    if (!_Require(domain.logicalControlOffset == 0, context,
                  "logical control offset was not zero") ||
        !_Require(domain.logicalControlCount == controlCount, context,
                  "logical control count was incorrect") ||
        !_Require(domain.authoredSegmentOffset == 0, context,
                  "authored segment offset was not zero") ||
        !_Require(domain.authoredSegmentCount == segmentCount, context,
                  "per-curve segment count was incorrect") ||
        !_Require(domain.varyingOffset == 0, context,
                  "varying offset was not zero") ||
        !_Require(domain.varyingCount == varyingCount, context,
                  "per-curve varying count was incorrect")) {
        return false;
    }

    for (size_t segmentIndex = 0;
         segmentIndex < result.segments.size();
         ++segmentIndex) {
        ty::CanonicalCurveSegment const& segment =
            result.segments[segmentIndex];
        const bool expectedStart = !periodic && segmentIndex == 0;
        const bool expectedEnd =
            !periodic && segmentIndex + 1 == segmentCount;
        if (segment.controlCount != segmentControlCount ||
            segment.metadata.authoredCurveId != 0 ||
            segment.metadata.authoredSegmentId != segmentIndex ||
            segment.metadata.authoredU0 != 0.0f ||
            segment.metadata.authoredU1 != 1.0f ||
            segment.metadata.isAuthoredCurveStart != expectedStart ||
            segment.metadata.isAuthoredCurveEnd != expectedEnd) {
            std::printf(
                "    %s: segment %zu metadata/control count was incorrect\n",
                context,
                segmentIndex);
            return false;
        }
    }
    return true;
}

bool
TestEmptyBatch()
{
    const ty::CurveTopologyResult result = ty::CanonicalizeCurveTopology(
        _MakeInput(
            HdTokens->linear,
            TfToken("ignoredLinearBasis"),
            HdTokens->nonperiodic,
            VtIntArray(),
            0));
    if (!_ExpectValid(result, "empty batch")) {
        return false;
    }
    return result.curveType == ty::CurveType::linear &&
        result.curveBasis == ty::CurveBasis::none &&
        result.curveWrap == ty::CurveWrap::nonperiodic &&
        result.vstep == 0 &&
        result.logicalControlDomainSize == 0 &&
        result.referencedPointDomainSize == 0 &&
        result.physicalPointCount == 0 &&
        result.constantDomainSize == 1 &&
        result.uniformDomainSize == 0 &&
        result.varyingDomainSize == 0 &&
        result.authoredSegmentCount == 0 &&
        result.curves.empty() &&
        result.logicalToPhysicalPointIndices.empty() &&
        result.segments.empty();
}

bool
TestSupportedTopologyMatrix()
{
    struct _TopologyCase
    {
        char const* name;
        TfToken curveType;
        TfToken curveBasis;
        TfToken curveWrap;
        int controlCount;
        size_t segmentCount;
        size_t varyingCount;
        size_t vstep;
        ty::CurveBasis expectedBasis;
    };

    const _TopologyCase cases[] = {
        {"linear/bezier/nonperiodic", HdTokens->linear, HdTokens->bezier,
         HdTokens->nonperiodic, 3, 2, 3, 0, ty::CurveBasis::none},
        {"linear/bezier/periodic", HdTokens->linear, HdTokens->bezier,
         HdTokens->periodic, 4, 4, 4, 0, ty::CurveBasis::none},
        {"linear/bspline/nonperiodic", HdTokens->linear, HdTokens->bspline,
         HdTokens->nonperiodic, 3, 2, 3, 0, ty::CurveBasis::none},
        {"linear/bspline/periodic", HdTokens->linear, HdTokens->bspline,
         HdTokens->periodic, 4, 4, 4, 0, ty::CurveBasis::none},
        {"linear/catmullRom/nonperiodic", HdTokens->linear,
         HdTokens->catmullRom, HdTokens->nonperiodic, 3, 2, 3, 0,
         ty::CurveBasis::none},
        {"linear/catmullRom/periodic", HdTokens->linear,
         HdTokens->catmullRom, HdTokens->periodic, 4, 4, 4, 0,
         ty::CurveBasis::none},
        {"cubic/bezier/nonperiodic", HdTokens->cubic, HdTokens->bezier,
         HdTokens->nonperiodic, 7, 2, 3, 3, ty::CurveBasis::bezier},
        {"cubic/bezier/periodic", HdTokens->cubic, HdTokens->bezier,
         HdTokens->periodic, 6, 2, 2, 3, ty::CurveBasis::bezier},
        {"cubic/bspline/nonperiodic", HdTokens->cubic, HdTokens->bspline,
         HdTokens->nonperiodic, 5, 2, 3, 1, ty::CurveBasis::bspline},
        {"cubic/bspline/periodic", HdTokens->cubic, HdTokens->bspline,
         HdTokens->periodic, 4, 4, 4, 1, ty::CurveBasis::bspline},
        {"cubic/bspline/pinned", HdTokens->cubic, HdTokens->bspline,
         HdTokens->pinned, 3, 2, 3, 1, ty::CurveBasis::bspline},
        {"cubic/catmullRom/nonperiodic", HdTokens->cubic,
         HdTokens->catmullRom, HdTokens->nonperiodic, 5, 2, 3, 1,
         ty::CurveBasis::catmullRom},
        {"cubic/catmullRom/periodic", HdTokens->cubic,
         HdTokens->catmullRom, HdTokens->periodic, 4, 4, 4, 1,
         ty::CurveBasis::catmullRom},
        {"cubic/catmullRom/pinned", HdTokens->cubic,
         HdTokens->catmullRom, HdTokens->pinned, 3, 2, 3, 1,
         ty::CurveBasis::catmullRom},
    };

    for (_TopologyCase const& testCase : cases) {
        const ty::CurveTopologyResult result =
            ty::CanonicalizeCurveTopology(_MakeInput(
                testCase.curveType,
                testCase.curveBasis,
                testCase.curveWrap,
                VtIntArray{testCase.controlCount},
                static_cast<size_t>(testCase.controlCount)));
        if (!_ExpectValid(result, testCase.name) ||
            !_Require(result.curveBasis == testCase.expectedBasis,
                      testCase.name, "normalized basis was incorrect") ||
            !_Require(result.vstep == testCase.vstep,
                      testCase.name, "vstep was incorrect") ||
            !_CheckSingleCurveSummary(
                result,
                static_cast<size_t>(testCase.controlCount),
                testCase.segmentCount,
                testCase.varyingCount,
                static_cast<std::uint8_t>(
                    testCase.curveType == HdTokens->linear ? 2 : 4),
                testCase.curveWrap == HdTokens->periodic,
                testCase.name)) {
            return false;
        }
    }
    return true;
}

bool
TestSchemaLiteralBoundaries()
{
    struct _BoundaryCase
    {
        char const* name;
        TfToken curveType;
        TfToken curveBasis;
        TfToken curveWrap;
        int controlCount;
        bool valid;
    };
    const _BoundaryCase cases[] = {
        {"linear nonperiodic n=2", HdTokens->linear, HdTokens->bezier,
         HdTokens->nonperiodic, 2, false},
        {"linear nonperiodic n=3", HdTokens->linear, HdTokens->bezier,
         HdTokens->nonperiodic, 3, true},
        {"linear periodic n=3", HdTokens->linear, HdTokens->bezier,
         HdTokens->periodic, 3, false},
        {"linear periodic n=4", HdTokens->linear, HdTokens->bezier,
         HdTokens->periodic, 4, true},
        {"Bezier nonperiodic n=3", HdTokens->cubic, HdTokens->bezier,
         HdTokens->nonperiodic, 3, false},
        {"Bezier nonperiodic n=4", HdTokens->cubic, HdTokens->bezier,
         HdTokens->nonperiodic, 4, true},
        {"Bezier periodic n=2", HdTokens->cubic, HdTokens->bezier,
         HdTokens->periodic, 2, false},
        {"Bezier periodic n=3", HdTokens->cubic, HdTokens->bezier,
         HdTokens->periodic, 3, true},
        {"B-spline nonperiodic n=3", HdTokens->cubic, HdTokens->bspline,
         HdTokens->nonperiodic, 3, false},
        {"B-spline nonperiodic n=4", HdTokens->cubic, HdTokens->bspline,
         HdTokens->nonperiodic, 4, true},
        {"Catmull-Rom nonperiodic n=3", HdTokens->cubic,
         HdTokens->catmullRom, HdTokens->nonperiodic, 3, false},
        {"Catmull-Rom nonperiodic n=4", HdTokens->cubic,
         HdTokens->catmullRom, HdTokens->nonperiodic, 4, true},
        {"B-spline periodic n=1", HdTokens->cubic, HdTokens->bspline,
         HdTokens->periodic, 1, true},
        {"Catmull-Rom periodic n=1", HdTokens->cubic,
         HdTokens->catmullRom, HdTokens->periodic, 1, true},
        {"B-spline pinned n=1", HdTokens->cubic, HdTokens->bspline,
         HdTokens->pinned, 1, false},
        {"B-spline pinned n=2", HdTokens->cubic, HdTokens->bspline,
         HdTokens->pinned, 2, true},
        {"Catmull-Rom pinned n=1", HdTokens->cubic,
         HdTokens->catmullRom, HdTokens->pinned, 1, false},
        {"Catmull-Rom pinned n=2", HdTokens->cubic,
         HdTokens->catmullRom, HdTokens->pinned, 2, true},
    };

    for (_BoundaryCase const& testCase : cases) {
        const ty::CurveTopologyResult result =
            ty::CanonicalizeCurveTopology(_MakeInput(
                testCase.curveType,
                testCase.curveBasis,
                testCase.curveWrap,
                VtIntArray{testCase.controlCount},
                static_cast<size_t>(testCase.controlCount)));
        if (testCase.valid) {
            if (!_ExpectValid(result, testCase.name)) {
                return false;
            }
        } else if (!_ExpectError(
                result,
                ty::CurveTopologyError::insufficientCurveVertexCount,
                testCase.name)) {
            return false;
        }
    }

    const ty::CurveTopologyResult minimumPeriodicBezier =
        ty::CanonicalizeCurveTopology(_MakeInput(
            HdTokens->cubic, HdTokens->bezier, HdTokens->periodic,
            VtIntArray{3}, 3));
    if (!_ExpectValid(minimumPeriodicBezier,
                      "minimum periodic Bezier") ||
        !_CheckSingleCurveSummary(
            minimumPeriodicBezier, 3, 1, 1, 4, true,
            "minimum periodic Bezier") ||
        !_CheckControls(
            minimumPeriodicBezier.segments[0],
            {{ty::CurveControlKind::authored, 0, 0},
             {ty::CurveControlKind::authored, 1, 1},
             {ty::CurveControlKind::authored, 2, 2},
             {ty::CurveControlKind::authored, 0, 0}},
            "minimum periodic Bezier")) {
        return false;
    }

    const TfToken unitPeriodicBases[] = {
        HdTokens->bspline,
        HdTokens->catmullRom};
    for (TfToken const& basis : unitPeriodicBases) {
        const char* const context = basis == HdTokens->bspline
            ? "unit periodic B-spline"
            : "unit periodic Catmull-Rom";
        const ty::CurveTopologyResult unitPeriodic =
            ty::CanonicalizeCurveTopology(_MakeInput(
                HdTokens->cubic, basis, HdTokens->periodic,
                VtIntArray{1}, 1));
        if (!_ExpectValid(unitPeriodic, context) ||
            !_CheckSingleCurveSummary(
                unitPeriodic, 1, 1, 1, 4, true, context) ||
            !_CheckControls(
                unitPeriodic.segments[0],
                {{ty::CurveControlKind::authored, 0, 0},
                 {ty::CurveControlKind::authored, 0, 0},
                 {ty::CurveControlKind::authored, 0, 0},
                 {ty::CurveControlKind::authored, 0, 0}},
                context)) {
            return false;
        }
    }

    const ty::CurveTopologyResult nonperiodicMisaligned =
        ty::CanonicalizeCurveTopology(_MakeInput(
            HdTokens->cubic, HdTokens->bezier, HdTokens->nonperiodic,
            VtIntArray{5, 6, 7}, 18));
    if (!_ExpectError(
            nonperiodicMisaligned,
            ty::CurveTopologyError::misalignedCurveVertexCount,
            "Bezier nonperiodic alignment") ||
        nonperiodicMisaligned.diagnostic.elementIndex != 0 ||
        nonperiodicMisaligned.diagnostic.affectedCount != 2 ||
        nonperiodicMisaligned.diagnostic.value != 5) {
        return false;
    }

    const ty::CurveTopologyResult periodicMisaligned =
        ty::CanonicalizeCurveTopology(_MakeInput(
            HdTokens->cubic, HdTokens->bezier, HdTokens->periodic,
            VtIntArray{4}, 4));
    return _ExpectError(
        periodicMisaligned,
        ty::CurveTopologyError::misalignedCurveVertexCount,
        "Bezier periodic alignment");
}

bool
TestTokenAndCombinationValidation()
{
    const TfToken invalidType("invalidType");
    const ty::CurveTopologyResult typeResult =
        ty::CanonicalizeCurveTopology(_MakeInput(
            invalidType, HdTokens->bezier, HdTokens->nonperiodic,
            VtIntArray{4}, 4));
    if (!_ExpectError(
            typeResult,
            ty::CurveTopologyError::invalidCurveType,
            "invalid type") ||
        typeResult.diagnostic.token != invalidType) {
        return false;
    }

    const TfToken invalidWrap("invalidWrap");
    const ty::CurveTopologyResult wrapResult =
        ty::CanonicalizeCurveTopology(_MakeInput(
            HdTokens->cubic, HdTokens->bezier, invalidWrap,
            VtIntArray{4}, 4));
    if (!_ExpectError(
            wrapResult,
            ty::CurveTopologyError::invalidCurveWrap,
            "invalid wrap") ||
        wrapResult.diagnostic.token != invalidWrap) {
        return false;
    }

    const TfToken invalidBasis("invalidBasis");
    const ty::CurveTopologyResult basisResult =
        ty::CanonicalizeCurveTopology(_MakeInput(
            HdTokens->cubic, invalidBasis, HdTokens->nonperiodic,
            VtIntArray{4}, 4));
    if (!_ExpectError(
            basisResult,
            ty::CurveTopologyError::invalidCurveBasis,
            "invalid cubic basis") ||
        basisResult.diagnostic.token != invalidBasis) {
        return false;
    }

    const ty::CurveTopologyResult ignoredLinearBasis =
        ty::CanonicalizeCurveTopology(_MakeInput(
            HdTokens->linear, invalidBasis, HdTokens->nonperiodic,
            VtIntArray{3}, 3));
    if (!_ExpectValid(ignoredLinearBasis, "ignored linear basis") ||
        ignoredLinearBasis.curveBasis != ty::CurveBasis::none) {
        return false;
    }

    const ty::CurveTopologyResult linearPinned =
        ty::CanonicalizeCurveTopology(_MakeInput(
            HdTokens->linear, HdTokens->bspline, HdTokens->pinned,
            VtIntArray{3}, 3));
    if (!_ExpectError(
            linearPinned,
            ty::CurveTopologyError::invalidTypeBasisWrap,
            "linear pinned")) {
        return false;
    }

    const ty::CurveTopologyResult bezierPinned =
        ty::CanonicalizeCurveTopology(_MakeInput(
            HdTokens->cubic, HdTokens->bezier, HdTokens->pinned,
            VtIntArray{4}, 4));
    return _ExpectError(
        bezierPinned,
        ty::CurveTopologyError::invalidTypeBasisWrap,
        "Bezier pinned");
}

bool
TestCountValidationAndAggregation()
{
    const ty::CurveTopologyResult nonPositive =
        ty::CanonicalizeCurveTopology(_MakeInput(
            HdTokens->linear, TfToken(), HdTokens->nonperiodic,
            VtIntArray{0, -2, 3}, 1));
    if (!_ExpectError(
            nonPositive,
            ty::CurveTopologyError::nonPositiveCurveVertexCount,
            "non-positive counts") ||
        nonPositive.diagnostic.elementIndex != 0 ||
        nonPositive.diagnostic.affectedCount != 2 ||
        nonPositive.diagnostic.value != 0) {
        return false;
    }

    const ty::CurveTopologyResult insufficient =
        ty::CanonicalizeCurveTopology(_MakeInput(
            HdTokens->linear, TfToken(), HdTokens->nonperiodic,
            VtIntArray{2, 1, 3}, 6));
    return _ExpectError(
            insufficient,
            ty::CurveTopologyError::insufficientCurveVertexCount,
            "aggregated insufficient counts") &&
        insufficient.diagnostic.elementIndex == 0 &&
        insufficient.diagnostic.affectedCount == 2 &&
        insufficient.diagnostic.value == 2;
}

bool
TestLinearAndCubicControlReferences()
{
    const ty::CurveTopologyResult linear =
        ty::CanonicalizeCurveTopology(_MakeInput(
            HdTokens->linear, TfToken(), HdTokens->nonperiodic,
            VtIntArray{3}, 3));
    if (!_ExpectValid(linear, "linear controls") ||
        !_CheckControls(
            linear.segments[0],
            {{ty::CurveControlKind::authored, 0, 0},
             {ty::CurveControlKind::authored, 1, 1}},
            "linear segment 0") ||
        !_CheckControls(
            linear.segments[1],
            {{ty::CurveControlKind::authored, 1, 1},
             {ty::CurveControlKind::authored, 2, 2}},
            "linear segment 1")) {
        return false;
    }

    const ty::CurveTopologyResult linearPeriodic =
        ty::CanonicalizeCurveTopology(_MakeInput(
            HdTokens->linear, TfToken(), HdTokens->periodic,
            VtIntArray{4}, 4));
    if (!_ExpectValid(linearPeriodic, "linear periodic controls") ||
        !_CheckControls(
            linearPeriodic.segments[3],
            {{ty::CurveControlKind::authored, 3, 3},
             {ty::CurveControlKind::authored, 0, 0}},
            "linear periodic seam")) {
        return false;
    }

    const ty::CurveTopologyResult bezier =
        ty::CanonicalizeCurveTopology(_MakeInput(
            HdTokens->cubic, HdTokens->bezier, HdTokens->nonperiodic,
            VtIntArray{7}, 7));
    if (!_ExpectValid(bezier, "Bezier controls") ||
        !_CheckControls(
            bezier.segments[0],
            {{ty::CurveControlKind::authored, 0, 0},
             {ty::CurveControlKind::authored, 1, 1},
             {ty::CurveControlKind::authored, 2, 2},
             {ty::CurveControlKind::authored, 3, 3}},
            "Bezier segment 0") ||
        !_CheckControls(
            bezier.segments[1],
            {{ty::CurveControlKind::authored, 3, 3},
             {ty::CurveControlKind::authored, 4, 4},
             {ty::CurveControlKind::authored, 5, 5},
             {ty::CurveControlKind::authored, 6, 6}},
            "Bezier segment 1")) {
        return false;
    }

    const ty::CurveTopologyResult bezierPeriodic =
        ty::CanonicalizeCurveTopology(_MakeInput(
            HdTokens->cubic, HdTokens->bezier, HdTokens->periodic,
            VtIntArray{6}, 6));
    if (!_ExpectValid(bezierPeriodic, "periodic Bezier controls") ||
        !_CheckControls(
            bezierPeriodic.segments[1],
            {{ty::CurveControlKind::authored, 3, 3},
             {ty::CurveControlKind::authored, 4, 4},
             {ty::CurveControlKind::authored, 5, 5},
             {ty::CurveControlKind::authored, 0, 0}},
            "periodic Bezier seam")) {
        return false;
    }

    const ty::CurveTopologyResult bsplinePeriodic =
        ty::CanonicalizeCurveTopology(_MakeInput(
            HdTokens->cubic, HdTokens->bspline, HdTokens->periodic,
            VtIntArray{4}, 4));
    return _ExpectValid(bsplinePeriodic, "periodic B-spline controls") &&
        _CheckControls(
            bsplinePeriodic.segments[3],
            {{ty::CurveControlKind::authored, 3, 3},
             {ty::CurveControlKind::authored, 0, 0},
             {ty::CurveControlKind::authored, 1, 1},
             {ty::CurveControlKind::authored, 2, 2}},
            "periodic B-spline seam");
}

bool
TestPinnedPhantomDescriptors()
{
    const TfToken bases[] = {HdTokens->bspline, HdTokens->catmullRom};
    for (TfToken const& basis : bases) {
        const char* const context = basis == HdTokens->bspline
            ? "pinned B-spline"
            : "pinned Catmull-Rom";
        const ty::CurveTopologyResult result =
            ty::CanonicalizeCurveTopology(_MakeInput(
                HdTokens->cubic, basis, HdTokens->pinned,
                VtIntArray{3}, 3));
        if (!_ExpectValid(result, context) ||
            result.logicalControlDomainSize != 3 ||
            result.referencedPointDomainSize != 3 ||
            result.varyingDomainSize != 3 ||
            result.logicalToPhysicalPointIndices.size() != 3 ||
            !_CheckControls(
                result.segments[0],
                {{ty::CurveControlKind::pinnedStartPhantom, 0, 1},
                 {ty::CurveControlKind::authored, 0, 0},
                 {ty::CurveControlKind::authored, 1, 1},
                 {ty::CurveControlKind::authored, 2, 2}},
                context) ||
            !_CheckControls(
                result.segments[1],
                {{ty::CurveControlKind::authored, 0, 0},
                 {ty::CurveControlKind::authored, 1, 1},
                 {ty::CurveControlKind::authored, 2, 2},
                 {ty::CurveControlKind::pinnedEndPhantom, 2, 1}},
                context)) {
            return false;
        }
    }

    const ty::CurveTopologyResult minimum =
        ty::CanonicalizeCurveTopology(_MakeInput(
            HdTokens->cubic, HdTokens->bspline, HdTokens->pinned,
            VtIntArray{2}, 2));
    return _ExpectValid(minimum, "minimum pinned curve") &&
        minimum.segments.size() == 1 &&
        minimum.segments[0].metadata.isAuthoredCurveStart &&
        minimum.segments[0].metadata.isAuthoredCurveEnd &&
        _CheckControls(
            minimum.segments[0],
            {{ty::CurveControlKind::pinnedStartPhantom, 0, 1},
             {ty::CurveControlKind::authored, 0, 0},
             {ty::CurveControlKind::authored, 1, 1},
             {ty::CurveControlKind::pinnedEndPhantom, 1, 0}},
            "minimum pinned curve");
}

bool
TestMultiCurveDomainsAndMetadata()
{
    const ty::CurveTopologyResult result =
        ty::CanonicalizeCurveTopology(_MakeInput(
            HdTokens->cubic, HdTokens->bezier, HdTokens->nonperiodic,
            VtIntArray{4, 7}, 11));
    if (!_ExpectValid(result, "multi-curve domains") ||
        result.constantDomainSize != 1 ||
        result.uniformDomainSize != 2 ||
        result.varyingDomainSize != 5 ||
        result.logicalControlDomainSize != 11 ||
        result.referencedPointDomainSize != 11 ||
        result.authoredSegmentCount != 3 ||
        result.curves.size() != 2 ||
        result.segments.size() != 3) {
        return false;
    }

    const ty::CurveAuthoredDomain expectedFirst{0, 4, 0, 1, 0, 2};
    const ty::CurveAuthoredDomain expectedSecond{4, 7, 1, 2, 2, 3};
    if (!_SameDomain(result.curves[0], expectedFirst) ||
        !_SameDomain(result.curves[1], expectedSecond)) {
        std::printf("    multi-curve domains: offsets were incorrect\n");
        return false;
    }

    if (result.segments[0].metadata.authoredCurveId != 0 ||
        result.segments[0].metadata.authoredSegmentId != 0 ||
        !result.segments[0].metadata.isAuthoredCurveStart ||
        !result.segments[0].metadata.isAuthoredCurveEnd ||
        result.segments[1].metadata.authoredCurveId != 1 ||
        result.segments[1].metadata.authoredSegmentId != 0 ||
        !result.segments[1].metadata.isAuthoredCurveStart ||
        result.segments[1].metadata.isAuthoredCurveEnd ||
        result.segments[2].metadata.authoredCurveId != 1 ||
        result.segments[2].metadata.authoredSegmentId != 1 ||
        result.segments[2].metadata.isAuthoredCurveStart ||
        !result.segments[2].metadata.isAuthoredCurveEnd) {
        std::printf("    multi-curve metadata: authored IDs/ends were wrong\n");
        return false;
    }

    return _CheckControls(
            result.segments[1],
            {{ty::CurveControlKind::authored, 4, 4},
             {ty::CurveControlKind::authored, 5, 5},
             {ty::CurveControlKind::authored, 6, 6},
             {ty::CurveControlKind::authored, 7, 7}},
            "second curve segment 0") &&
        _CheckControls(
            result.segments[2],
            {{ty::CurveControlKind::authored, 7, 7},
             {ty::CurveControlKind::authored, 8, 8},
             {ty::CurveControlKind::authored, 9, 9},
             {ty::CurveControlKind::authored, 10, 10}},
            "second curve segment 1");
}

bool
TestIndexedAndUnindexedDomains()
{
    const ty::CurveTopologyResult unindexed =
        ty::CanonicalizeCurveTopology(_MakeInput(
            HdTokens->linear, TfToken(), HdTokens->nonperiodic,
            VtIntArray{3}, 3));
    if (!_ExpectValid(unindexed, "unindexed topology") ||
        unindexed.logicalToPhysicalPointIndices !=
            std::vector<size_t>({0, 1, 2})) {
        return false;
    }

    const ty::CurveTopologyResult indexed =
        ty::CanonicalizeCurveTopology(_MakeInput(
            HdTokens->linear, TfToken(), HdTokens->nonperiodic,
            VtIntArray{3}, 5, VtIntArray{4, 0, 4}));
    if (!_ExpectValid(indexed, "indexed topology") ||
        indexed.physicalPointCount != 5 ||
        indexed.logicalControlDomainSize != 3 ||
        indexed.referencedPointDomainSize != 5 ||
        indexed.logicalToPhysicalPointIndices !=
            std::vector<size_t>({4, 0, 4})) {
        return false;
    }

    const ty::CurveTopologyResult indexedWithUnusedPoints =
        ty::CanonicalizeCurveTopology(_MakeInput(
            HdTokens->linear, TfToken(), HdTokens->nonperiodic,
            VtIntArray{3}, 6, VtIntArray{1, 2, 4}));
    if (!_ExpectValid(indexedWithUnusedPoints,
                      "indexed topology with unused points") ||
        indexedWithUnusedPoints.logicalControlDomainSize != 3 ||
        indexedWithUnusedPoints.referencedPointDomainSize != 5 ||
        indexedWithUnusedPoints.physicalPointCount != 6 ||
        indexedWithUnusedPoints.logicalToPhysicalPointIndices !=
            std::vector<size_t>({1, 2, 4})) {
        return false;
    }

    const ty::CurveTopologyResult unindexedShort =
        ty::CanonicalizeCurveTopology(_MakeInput(
            HdTokens->linear, TfToken(), HdTokens->nonperiodic,
            VtIntArray{3}, 2));
    if (!_ExpectError(
            unindexedShort,
            ty::CurveTopologyError::pointCountMismatch,
            "unindexed point count short") ||
        unindexedShort.diagnostic.expectedCount != 3 ||
        unindexedShort.diagnostic.actualCount != 2) {
        return false;
    }

    const ty::CurveTopologyResult unindexedLong =
        ty::CanonicalizeCurveTopology(_MakeInput(
            HdTokens->linear, TfToken(), HdTokens->nonperiodic,
            VtIntArray{3}, 4));
    if (!_ExpectError(
            unindexedLong,
            ty::CurveTopologyError::pointCountMismatch,
            "unindexed point count long")) {
        return false;
    }

    const ty::CurveTopologyResult indexedShort =
        ty::CanonicalizeCurveTopology(_MakeInput(
            HdTokens->linear, TfToken(), HdTokens->nonperiodic,
            VtIntArray{3}, 3, VtIntArray{0, 1}));
    if (!_ExpectError(
            indexedShort,
            ty::CurveTopologyError::curveIndexCountMismatch,
            "index count short") ||
        indexedShort.diagnostic.expectedCount != 3 ||
        indexedShort.diagnostic.actualCount != 2) {
        return false;
    }

    const ty::CurveTopologyResult indexedLong =
        ty::CanonicalizeCurveTopology(_MakeInput(
            HdTokens->linear, TfToken(), HdTokens->nonperiodic,
            VtIntArray{3}, 3, VtIntArray{0, 1, 2, 0}));
    if (!_ExpectError(
            indexedLong,
            ty::CurveTopologyError::curveIndexCountMismatch,
            "index count long")) {
        return false;
    }

    const ty::CurveTopologyResult invalidIndices =
        ty::CanonicalizeCurveTopology(_MakeInput(
            HdTokens->linear, TfToken(), HdTokens->nonperiodic,
            VtIntArray{3}, 5, VtIntArray{-1, 5, 0}));
    return _ExpectError(
            invalidIndices,
            ty::CurveTopologyError::invalidCurvePointIndex,
            "invalid point indices") &&
        invalidIndices.diagnostic.elementIndex == 0 &&
        invalidIndices.diagnostic.affectedCount == 2 &&
        invalidIndices.diagnostic.value == -1;
}

bool
TestInvisibleCurvesPreserveAuthoredDomains()
{
    const ty::CurveTopologyInput baselineInput = _MakeInput(
        HdTokens->linear, TfToken(), HdTokens->nonperiodic,
        VtIntArray{3, 4, 3}, 10);
    const ty::CurveTopologyResult baseline =
        ty::CanonicalizeCurveTopology(baselineInput);

    ty::CurveTopologyInput filteredInput = baselineInput;
    filteredInput.invisibleCurves = VtIntArray{1, 1, -1, 3, 99};
    const ty::CurveTopologyResult filtered =
        ty::CanonicalizeCurveTopology(filteredInput);
    if (!_ExpectValid(baseline, "visible curve baseline") ||
        !_ExpectValid(filtered, "invisible curves") ||
        filtered.recovery.ignoredInvisibleCurveCount != 3 ||
        filtered.recovery.ignoredInvisiblePointCount != 0 ||
        filtered.constantDomainSize != baseline.constantDomainSize ||
        filtered.uniformDomainSize != baseline.uniformDomainSize ||
        filtered.varyingDomainSize != baseline.varyingDomainSize ||
        filtered.logicalControlDomainSize !=
            baseline.logicalControlDomainSize ||
        filtered.referencedPointDomainSize !=
            baseline.referencedPointDomainSize ||
        filtered.authoredSegmentCount != baseline.authoredSegmentCount ||
        filtered.curves.size() != baseline.curves.size() ||
        filtered.segments.size() != 4) {
        return false;
    }
    for (size_t curveIndex = 0;
         curveIndex < filtered.curves.size();
         ++curveIndex) {
        if (!_SameDomain(
                filtered.curves[curveIndex], baseline.curves[curveIndex])) {
            std::printf(
                "    invisible curves: authored domain %zu changed\n",
                curveIndex);
            return false;
        }
    }

    const size_t expectedCurveIds[] = {0, 0, 2, 2};
    const size_t expectedSegmentIds[] = {0, 1, 0, 1};
    for (size_t i = 0; i < filtered.segments.size(); ++i) {
        ty::CurveSegmentMetadata const& metadata =
            filtered.segments[i].metadata;
        if (metadata.authoredCurveId != expectedCurveIds[i] ||
            metadata.authoredSegmentId != expectedSegmentIds[i]) {
            std::printf(
                "    invisible curves: segment %zu was renumbered\n", i);
            return false;
        }

        bool foundBaseline = false;
        for (ty::CanonicalCurveSegment const& candidate : baseline.segments) {
            if (candidate.metadata.authoredCurveId ==
                    metadata.authoredCurveId &&
                candidate.metadata.authoredSegmentId ==
                    metadata.authoredSegmentId) {
                foundBaseline = _SameMetadata(candidate.metadata, metadata);
                break;
            }
        }
        if (!foundBaseline) {
            std::printf(
                "    invisible curves: metadata %zu changed after filtering\n",
                i);
            return false;
        }
    }
    return true;
}

bool
TestInvisiblePointsUsePhysicalDomain()
{
    const ty::CurveTopologyResult indexed =
        ty::CanonicalizeCurveTopology(_MakeInput(
            HdTokens->linear, TfToken(), HdTokens->nonperiodic,
            VtIntArray{4}, 3, VtIntArray{2, 0, 2, 1},
            VtIntArray(), VtIntArray{0, 0, -1, 3, 8}));
    if (!_ExpectValid(indexed, "indexed invisible points") ||
        indexed.recovery.ignoredInvisiblePointCount != 3 ||
        indexed.logicalControlDomainSize != 4 ||
        indexed.referencedPointDomainSize != 3 ||
        indexed.physicalPointCount != 3 ||
        indexed.segments.size() != 1 ||
        indexed.segments[0].metadata.authoredSegmentId != 2 ||
        indexed.segments[0].metadata.isAuthoredCurveStart ||
        !indexed.segments[0].metadata.isAuthoredCurveEnd) {
        return false;
    }

    const ty::CurveTopologyResult cubic =
        ty::CanonicalizeCurveTopology(_MakeInput(
            HdTokens->cubic, HdTokens->bspline, HdTokens->nonperiodic,
            VtIntArray{5}, 5, VtIntArray(), VtIntArray(), VtIntArray{0}));
    if (!_ExpectValid(cubic, "cubic invisible span") ||
        cubic.segments.size() != 1 ||
        cubic.segments[0].metadata.authoredSegmentId != 1 ||
        cubic.segments[0].metadata.isAuthoredCurveStart ||
        !cubic.segments[0].metadata.isAuthoredCurveEnd) {
        return false;
    }

    const ty::CurveTopologyResult periodic =
        ty::CanonicalizeCurveTopology(_MakeInput(
            HdTokens->linear, TfToken(), HdTokens->periodic,
            VtIntArray{4}, 4, VtIntArray(), VtIntArray(), VtIntArray{0}));
    if (!_ExpectValid(periodic, "periodic seam visibility") ||
        periodic.segments.size() != 2 ||
        periodic.segments[0].metadata.authoredSegmentId != 1 ||
        periodic.segments[1].metadata.authoredSegmentId != 2) {
        return false;
    }
    for (ty::CanonicalCurveSegment const& segment : periodic.segments) {
        if (segment.metadata.isAuthoredCurveStart ||
            segment.metadata.isAuthoredCurveEnd) {
            std::printf(
                "    periodic seam visibility: created an authored end\n");
            return false;
        }
    }
    return true;
}

bool
TestFatalValidationReturnsNoPartialOutput()
{
    const ty::CurveTopologyResult laterInvalidCurve =
        ty::CanonicalizeCurveTopology(_MakeInput(
            HdTokens->linear, TfToken(), HdTokens->nonperiodic,
            VtIntArray{3, 2, 4}, 9));
    if (!_ExpectError(
            laterInvalidCurve,
            ty::CurveTopologyError::insufficientCurveVertexCount,
            "later invalid curve") ||
        laterInvalidCurve.diagnostic.elementIndex != 1) {
        return false;
    }

    const ty::CurveTopologyResult laterInvalidIndex =
        ty::CanonicalizeCurveTopology(_MakeInput(
            HdTokens->linear, TfToken(), HdTokens->nonperiodic,
            VtIntArray{3}, 3, VtIntArray{0, 1, 9}));
    return _ExpectError(
            laterInvalidIndex,
            ty::CurveTopologyError::invalidCurvePointIndex,
            "later invalid index") &&
        laterInvalidIndex.diagnostic.elementIndex == 2;
}

bool
TestInputBoundaryBehavior()
{
    const TfToken invalidWrap("invalidWrap");
    const UsdStageRefPtr stage = UsdStage::CreateInMemory();
    if (!_Require(static_cast<bool>(stage), "input boundary",
                  "could not create an in-memory stage")) {
        return false;
    }

    const UsdGeomBasisCurves curves =
        UsdGeomBasisCurves::Define(stage, SdfPath("/Curves"));
    if (!_Require(static_cast<bool>(curves), "input boundary",
                  "could not define BasisCurves") ||
        !_Require(curves.CreateTypeAttr().Set(UsdGeomTokens->linear),
                  "input boundary", "could not author type") ||
        !_Require(curves.CreateWrapAttr().Set(invalidWrap),
                  "input boundary", "could not author invalid wrap") ||
        !_Require(curves.CreateCurveVertexCountsAttr().Set(VtIntArray{3}),
                  "input boundary", "could not author counts")) {
        return false;
    }

    _StageGlobals stageGlobals;
    UsdImagingBasisCurvesAdapter adapter;
    const HdContainerDataSourceHandle primDataSource =
        adapter.GetImagingSubprimData(
            curves.GetPrim(), TfToken(), stageGlobals);
    const HdBasisCurvesTopologySchema rawTopology =
        HdBasisCurvesSchema::GetFromParent(primDataSource).GetTopology();
    const HdTokenDataSourceHandle rawWrapDataSource = rawTopology.GetWrap();
    if (!_Require(static_cast<bool>(rawWrapDataSource), "input boundary",
                  "scene-index wrap data source was missing")) {
        return false;
    }
    const TfToken rawWrap = rawWrapDataSource->GetTypedValue(0.0f);
    const ty::CurveTopologyResult rawResult =
        ty::CanonicalizeCurveTopology(_MakeInput(
            HdTokens->linear, TfToken(), rawWrap, VtIntArray{3}, 3));
    if (rawWrap != invalidWrap ||
        !_ExpectError(
            rawResult,
            ty::CurveTopologyError::invalidCurveWrap,
            "raw scene-index wrap")) {
        return false;
    }

    TfDiagnosticTrap trap;
    const VtValue legacyValue = adapter.GetTopology(
        curves.GetPrim(), curves.GetPath(), UsdTimeCode::Default());
    trap.Clear();
    if (!_Require(
            legacyValue.IsHolding<HdBasisCurvesTopology>(),
            "input boundary",
            "legacy adapter did not return BasisCurves topology")) {
        return false;
    }
    const HdBasisCurvesTopology legacyTopology =
        legacyValue.Get<HdBasisCurvesTopology>();
    const ty::CurveTopologyResult legacyResult =
        ty::CanonicalizeCurveTopology(_MakeInput(
            legacyTopology.GetCurveType(),
            legacyTopology.GetCurveBasis(),
            legacyTopology.GetCurveWrap(),
            legacyTopology.GetCurveVertexCounts(),
            3,
            legacyTopology.GetCurveIndices()));
    if (!_ExpectValid(legacyResult, "legacy sanitized wrap") ||
        legacyTopology.GetCurveWrap() != HdTokens->nonperiodic ||
        legacyResult.curveWrap != ty::CurveWrap::nonperiodic) {
        return false;
    }

    const HdDataSourceBaseHandle wrongTypeCounts =
        HdRetainedTypedSampledDataSource<VtFloatArray>::New(
            VtFloatArray{3.0f});
    const HdContainerDataSourceHandle wrongTypeTopology =
        HdRetainedContainerDataSource::New(
            HdBasisCurvesTopologySchemaTokens->curveVertexCounts,
            wrongTypeCounts);
    const HdBasisCurvesTopologySchema wrongTypeSchema(wrongTypeTopology);
    return _Require(
        !wrongTypeSchema.GetCurveVertexCounts(),
        "input boundary",
        "wrong-type counts unexpectedly crossed the typed boundary");
}

bool
TestDefaultResultIsNotCanonicalized()
{
    const ty::CurveTopologyResult result;
    return _ExpectError(
        result,
        ty::CurveTopologyError::notCanonicalized,
        "default result");
}

bool
TestDiagnosticResultIsPure()
{
    TfDiagnosticTrap trap;
    const ty::CurveTopologyResult result =
        ty::CanonicalizeCurveTopology(_MakeInput(
            TfToken("badType"), HdTokens->bezier,
            HdTokens->nonperiodic, VtIntArray{4}, 4));
    const bool clean = trap.IsClean();
    trap.Clear();
    return _ExpectError(
            result,
            ty::CurveTopologyError::invalidCurveType,
            "pure diagnostic result") &&
        _Require(clean, "pure diagnostic result",
                 "canonicalization emitted a Tf diagnostic");
}

bool
TestGeometryPointValidation()
{
    const ty::CurveTopologyResult topology =
        ty::CanonicalizeCurveTopology(_MakeInput(
            HdTokens->linear,
            TfToken(),
            HdTokens->nonperiodic,
            VtIntArray{3},
            3));
    if (!_ExpectValid(topology, "geometry point topology")) {
        return false;
    }

    ty::CurveGeometryInput wrongType;
    wrongType.points = VtValue(VtFloatArray{0.0f, 1.0f, 2.0f});
    if (!_ExpectGeometryError(
            ty::BuildCurveGeometry(topology, wrongType),
            ty::CurveGeometryError::unsupportedPointType,
            "wrong point type")) {
        return false;
    }

    const ty::CurveGeometryResult wrongCount = ty::BuildCurveGeometry(
        topology,
        _MakeGeometryInput(VtVec3fArray{
            GfVec3f(0.0f), GfVec3f(1.0f)}));
    if (!_ExpectGeometryError(
            wrongCount,
            ty::CurveGeometryError::pointCountMismatch,
            "wrong point count") ||
        wrongCount.diagnostic.expectedCount != 3 ||
        wrongCount.diagnostic.actualCount != 2) {
        return false;
    }

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const ty::CurveGeometryResult nonFinite = ty::BuildCurveGeometry(
        topology,
        _MakeGeometryInput(VtVec3fArray{
            GfVec3f(0.0f), GfVec3f(nan, 0.0f, 0.0f),
            GfVec3f(2.0f, 0.0f, 0.0f)}));
    if (!_ExpectGeometryError(
            nonFinite,
            ty::CurveGeometryError::nonFinitePoint,
            "non-finite referenced point") ||
        nonFinite.diagnostic.elementIndex != 1) {
        return false;
    }

    const ty::CurveTopologyResult indexedTopology =
        ty::CanonicalizeCurveTopology(_MakeInput(
            HdTokens->linear,
            TfToken(),
            HdTokens->nonperiodic,
            VtIntArray{3},
            4,
            VtIntArray{2, 0, 1}));
    ty::CurveGeometryInput indexedInput = _MakeGeometryInput(
        VtVec3fArray{
            GfVec3f(1.0f, 0.0f, 0.0f),
            GfVec3f(2.0f, 0.0f, 0.0f),
            GfVec3f(0.0f, 0.0f, 0.0f),
            GfVec3f(nan, nan, nan)},
        0.1f);
    const ty::CurveGeometryResult indexed = ty::BuildCurveGeometry(
        indexedTopology, indexedInput);
    return _ExpectGeometryValid(indexed, "unreferenced non-finite point") &&
        indexed.primitives.size() == 2;
}

bool
TestNativeAndHermiteBasisAgreement()
{
    struct _BasisCase
    {
        TfToken token;
        ty::CurveGeometryRepresentation representation;
    };
    const _BasisCase cases[] = {
        {HdTokens->bezier,
         ty::CurveGeometryRepresentation::nativeBezier},
        {HdTokens->bspline,
         ty::CurveGeometryRepresentation::nativeBspline},
        {HdTokens->catmullRom,
         ty::CurveGeometryRepresentation::nativeCatmullRom},
    };
    const VtVec3fArray points = {
        GfVec3f(0.0f, 0.0f, 0.0f),
        GfVec3f(0.4f, 1.2f, 0.2f),
        GfVec3f(1.6f, -0.5f, 0.4f),
        GfVec3f(2.0f, 0.3f, 1.0f)};

    for (_BasisCase const& testCase : cases) {
        const ty::CurveTopologyResult topology =
            ty::CanonicalizeCurveTopology(_MakeInput(
                HdTokens->cubic,
                testCase.token,
                HdTokens->nonperiodic,
                VtIntArray{4},
                4));
        ty::CurveGeometryInput nativeInput = _MakeGeometryInput(points, 0.0f);
        nativeInput.builtInWidths = _MakePrimvar(
            VtValue(VtFloatArray{0.4f, 0.8f, 1.2f, 1.6f}),
            TfToken("vertex"));
        const ty::CurveGeometryResult nativeResult =
            ty::BuildCurveGeometry(topology, nativeInput);

        ty::CurveGeometryInput hermiteInput = _MakeGeometryInput(points, 0.0f);
        hermiteInput.builtInWidths = _MakePrimvar(
            VtValue(VtFloatArray{0.4f, 1.6f}),
            TfToken("varying"));
        const ty::CurveGeometryResult hermiteResult =
            ty::BuildCurveGeometry(topology, hermiteInput);
        if (!_ExpectGeometryValid(nativeResult, "native basis") ||
            !_ExpectGeometryValid(hermiteResult, "Hermite basis") ||
            nativeResult.primitives.size() != 1 ||
            hermiteResult.primitives.size() != 1 ||
            nativeResult.primitives[0].representation !=
                testCase.representation ||
            hermiteResult.primitives[0].representation !=
                ty::CurveGeometryRepresentation::hermite) {
            return false;
        }

        constexpr float parameters[] = {0.0f, 0.17f, 0.5f, 0.83f, 1.0f};
        for (const float u : parameters) {
            if (!_Close(
                    ty::EvaluateCurvePosition(nativeResult.primitives[0], u),
                    ty::EvaluateCurvePosition(hermiteResult.primitives[0], u),
                    2.0e-5f) ||
                !_Close(
                    ty::EvaluateCurveTangent(nativeResult.primitives[0], u),
                    ty::EvaluateCurveTangent(hermiteResult.primitives[0], u),
                    3.0e-5f)) {
                std::printf(
                    "    basis %s disagreed with exact Hermite at %g\n",
                    testCase.token.GetText(),
                    static_cast<double>(u));
                return false;
            }
        }
    }
    return true;
}

bool
TestLinearRibbonIsStraightHermite()
{
    const ty::CurveTopologyResult topology =
        ty::CanonicalizeCurveTopology(_MakeInput(
            HdTokens->linear,
            TfToken(),
            HdTokens->nonperiodic,
            VtIntArray{3},
            3));
    ty::CurveGeometryInput input = _MakeGeometryInput(
        VtVec3fArray{
            GfVec3f(0.0f, 0.0f, 0.0f),
            GfVec3f(1.0f, 0.0f, 0.0f),
            GfVec3f(2.0f, 0.0f, 0.0f)},
        0.0f);
    input.builtInWidths = _MakePrimvar(
        VtValue(VtFloatArray{0.2f}), TfToken("constant"));
    input.builtInNormals = _MakePrimvar(
        VtValue(VtVec3fArray{
            GfVec3f(0.0f, 1.0f, 0.0f),
            GfVec3f(0.0f, 1.0f, 0.0f),
            GfVec3f(0.0f, 1.0f, 0.0f)}),
        TfToken("vertex"));
    const ty::CurveGeometryResult result =
        ty::BuildCurveGeometry(topology, input);
    if (!_ExpectGeometryValid(result, "linear ribbon") ||
        result.primitives.size() != 2) {
        return false;
    }
    for (size_t segment = 0; segment < result.primitives.size(); ++segment) {
        ty::CurveGeometryPrimitive const& primitive =
            result.primitives[segment];
        if (primitive.representation !=
                ty::CurveGeometryRepresentation::hermite ||
            primitive.shape != ty::CurveGeometryShape::ribbon) {
            return false;
        }
        constexpr float parameters[] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
        for (const float u : parameters) {
            const GfVec3f expected(
                static_cast<float>(segment) + u, 0.0f, 0.0f);
            if (!_Close(ty::EvaluateCurvePosition(primitive, u), expected) ||
                !_Close(
                    ty::EvaluateCurveTangent(primitive, u),
                    GfVec3f(1.0f, 0.0f, 0.0f)) ||
                !_Close(
                    ty::EvaluateCurveNormal(primitive, u),
                    GfVec3f(0.0f, 1.0f, 0.0f))) {
                return false;
            }
        }
    }
    return true;
}

bool
TestPinnedGeometryUsesResolvedLogicalControls()
{
    const TfToken bases[] = {HdTokens->bspline, HdTokens->catmullRom};
    for (TfToken const& basis : bases) {
        const ty::CurveTopologyResult topology =
            ty::CanonicalizeCurveTopology(_MakeInput(
                HdTokens->cubic,
                basis,
                HdTokens->pinned,
                VtIntArray{3},
                3,
                VtIntArray{2, 0, 1}));
        ty::CurveGeometryInput input = _MakeGeometryInput(
            VtVec3fArray{
                GfVec3f(1.0f, 0.0f, 0.0f),
                GfVec3f(2.0f, 1.0f, 0.0f),
                GfVec3f(0.0f, 0.0f, 0.0f)},
            0.0f);
        input.builtInWidths = _MakePrimvar(
            VtValue(VtFloatArray{2.0f, 3.0f, 1.0f}),
            TfToken("vertex"));
        input.builtInNormals = _MakePrimvar(
            VtValue(VtVec3fArray{
                GfVec3f(0.0f, 0.0f, 1.0f),
                GfVec3f(0.0f, 0.0f, 1.0f),
                GfVec3f(0.0f, 0.0f, 1.0f)}),
            TfToken("vertex"));
        const ty::CurveGeometryResult result =
            ty::BuildCurveGeometry(topology, input);
        if (!_ExpectGeometryValid(result, "pinned geometry") ||
            result.primitives.size() != 2 ||
            !_Close(
                ty::EvaluateCurvePosition(result.primitives.front(), 0.0f),
                GfVec3f(0.0f, 0.0f, 0.0f)) ||
            !_Close(
                ty::EvaluateCurvePosition(result.primitives.back(), 1.0f),
                GfVec3f(2.0f, 1.0f, 0.0f)) ||
            !_Close(
                ty::EvaluateCurveRadius(result.primitives.front(), 0.0f),
                0.5f) ||
            !_Close(
                ty::EvaluateCurveRadius(result.primitives.back(), 1.0f),
                1.5f)) {
            return false;
        }
    }
    return true;
}

bool
TestWidthPrecedenceAndStructuralFallback()
{
    const ty::CurveTopologyResult topology =
        ty::CanonicalizeCurveTopology(_MakeInput(
            HdTokens->linear,
            TfToken(),
            HdTokens->nonperiodic,
            VtIntArray{3},
            3));
    ty::CurveGeometryInput input = _MakeGeometryInput(
        VtVec3fArray{
            GfVec3f(0.0f, 0.0f, 0.0f),
            GfVec3f(1.0f, 0.0f, 0.0f),
            GfVec3f(2.0f, 0.0f, 0.0f)},
        0.2f);
    input.primvarWidths = _MakePrimvar(
        VtValue(VtVec3fArray{GfVec3f(4.0f)}),
        TfToken("constant"));
    input.builtInWidths = _MakePrimvar(
        VtValue(VtFloatArray{0.8f}), TfToken("constant"));
    const ty::CurveGeometryResult invalidHighPriority =
        ty::BuildCurveGeometry(topology, input);
    if (!_ExpectGeometryValid(
            invalidHighPriority, "invalid high-priority width") ||
        invalidHighPriority.widthStatus.selectedSource !=
            ty::CurveAttributeSource::primvar ||
        invalidHighPriority.widthStatus.effectiveSource !=
            ty::CurveAttributeSource::minimum ||
        invalidHighPriority.widthStatus.error !=
            ty::CurvePrimvarError::unsupportedValueType ||
        invalidHighPriority.recovery.invalidWidthFallbackCount != 1 ||
        invalidHighPriority.primitives.empty() ||
        !_Close(
            ty::EvaluateCurveRadius(
                invalidHighPriority.primitives[0], 0.5f),
            0.1f)) {
        return false;
    }

    ty::CurveGeometryInput missing = _MakeGeometryInput(
        input.points.UncheckedGet<VtVec3fArray>(), 0.3f);
    const ty::CurveGeometryResult missingResult =
        ty::BuildCurveGeometry(topology, missing);
    if (!_ExpectGeometryValid(missingResult, "missing width") ||
        missingResult.widthStatus.selectedSource !=
            ty::CurveAttributeSource::none ||
        missingResult.widthStatus.effectiveSource !=
            ty::CurveAttributeSource::minimum ||
        missingResult.recovery.missingWidthFallbackCount != 1 ||
        !_Close(
            ty::EvaluateCurveRadius(missingResult.primitives[0], 0.5f),
            0.15f)) {
        return false;
    }

    ty::CurveGeometryInput indexed = _MakeGeometryInput(
        input.points.UncheckedGet<VtVec3fArray>(), 0.0f);
    indexed.builtInWidths = _MakePrimvar(
        VtValue(VtFloatArray{0.2f, 0.6f, 1.0f}),
        TfToken("vertex"),
        VtIntArray{2, 1, 0});
    const ty::CurveGeometryResult indexedResult =
        ty::BuildCurveGeometry(topology, indexed);
    if (!_ExpectGeometryValid(indexedResult, "indexed width") ||
        !_Close(
            ty::EvaluateCurveRadius(indexedResult.primitives[0], 0.0f),
            0.5f) ||
        !_Close(
            ty::EvaluateCurveRadius(indexedResult.primitives[1], 1.0f),
            0.1f)) {
        return false;
    }

    ty::CurveGeometryInput invalidInterpolation = _MakeGeometryInput(
        input.points.UncheckedGet<VtVec3fArray>(), 0.4f);
    invalidInterpolation.primvarWidths = _MakePrimvar(
        VtValue(VtFloatArray{0.8f}), TfToken("faceVarying"));
    invalidInterpolation.builtInWidths = _MakePrimvar(
        VtValue(VtFloatArray{1.2f}), TfToken("constant"));
    invalidInterpolation.primvarNormals = _MakePrimvar(
        VtValue(VtVec3fArray{GfVec3f(0.0f, 0.0f, 1.0f)}),
        TfToken("faceVarying"));
    invalidInterpolation.builtInNormals = _MakePrimvar(
        VtValue(VtVec3fArray{GfVec3f(0.0f, 0.0f, 1.0f)}),
        TfToken("constant"));
    const ty::CurveGeometryResult interpolationResult =
        ty::BuildCurveGeometry(topology, invalidInterpolation);
    return _ExpectGeometryValid(
               interpolationResult, "invalid interpolation") &&
        interpolationResult.widthStatus.selectedSource ==
            ty::CurveAttributeSource::primvar &&
        interpolationResult.widthStatus.effectiveSource ==
            ty::CurveAttributeSource::minimum &&
        interpolationResult.widthStatus.error ==
            ty::CurvePrimvarError::invalidInterpolation &&
        interpolationResult.normalStatus.selectedSource ==
            ty::CurveAttributeSource::primvar &&
        interpolationResult.normalStatus.effectiveSource ==
            ty::CurveAttributeSource::none &&
        interpolationResult.normalStatus.error ==
            ty::CurvePrimvarError::invalidInterpolation &&
        interpolationResult.recovery.invalidWidthFallbackCount == 1 &&
        interpolationResult.recovery.invalidNormalSourceCount == 1 &&
        !interpolationResult.primitives.empty() &&
        interpolationResult.primitives[0].shape ==
            ty::CurveGeometryShape::tube &&
        _Close(
            ty::EvaluateCurveRadius(
                interpolationResult.primitives[0], 0.5f),
            0.2f);
}

bool
TestStructuralPrimvarErrorMatrix()
{
    const ty::CurveTopologyResult topology =
        ty::CanonicalizeCurveTopology(_MakeInput(
            HdTokens->linear,
            TfToken(),
            HdTokens->nonperiodic,
            VtIntArray{3},
            3));
    const VtVec3fArray points = {
        GfVec3f(0.0f, 0.0f, 0.0f),
        GfVec3f(1.0f, 0.0f, 0.0f),
        GfVec3f(2.0f, 0.0f, 0.0f)};
    struct _Case
    {
        char const* name;
        ty::CurvePrimvarError error;
        ty::CurvePrimvarInput width;
        ty::CurvePrimvarInput normal;
    };
    const _Case cases[] = {
        {
            "unsupported type",
            ty::CurvePrimvarError::unsupportedValueType,
            _MakePrimvar(
                VtValue(VtVec3fArray{GfVec3f(1.0f)}),
                TfToken("constant")),
            _MakePrimvar(
                VtValue(VtFloatArray{1.0f}), TfToken("constant")),
        },
        {
            "invalid interpolation",
            ty::CurvePrimvarError::invalidInterpolation,
            _MakePrimvar(
                VtValue(VtFloatArray{1.0f}), TfToken("faceVarying")),
            _MakePrimvar(
                VtValue(VtVec3fArray{GfVec3f(0.0f, 0.0f, 1.0f)}),
                TfToken("faceVarying")),
        },
        {
            "element count mismatch",
            ty::CurvePrimvarError::elementCountMismatch,
            _MakePrimvar(
                VtValue(VtFloatArray{1.0f, 2.0f}),
                TfToken("constant")),
            _MakePrimvar(
                VtValue(VtVec3fArray{
                    GfVec3f(0.0f, 0.0f, 1.0f),
                    GfVec3f(0.0f, 0.0f, 1.0f)}),
                TfToken("constant")),
        },
        {
            "index count mismatch",
            ty::CurvePrimvarError::indexCountMismatch,
            _MakePrimvar(
                VtValue(VtFloatArray{1.0f}),
                TfToken("constant"),
                VtIntArray{0, 0}),
            _MakePrimvar(
                VtValue(VtVec3fArray{GfVec3f(0.0f, 0.0f, 1.0f)}),
                TfToken("constant"),
                VtIntArray{0, 0}),
        },
        {
            "empty index array",
            ty::CurvePrimvarError::indexCountMismatch,
            _MakePrimvar(
                VtValue(VtFloatArray{1.0f}),
                TfToken("constant"),
                VtIntArray(),
                true),
            _MakePrimvar(
                VtValue(VtVec3fArray{GfVec3f(0.0f, 0.0f, 1.0f)}),
                TfToken("constant"),
                VtIntArray(),
                true),
        },
        {
            "invalid index",
            ty::CurvePrimvarError::invalidIndex,
            _MakePrimvar(
                VtValue(VtFloatArray{1.0f}),
                TfToken("constant"),
                VtIntArray{1}),
            _MakePrimvar(
                VtValue(VtVec3fArray{GfVec3f(0.0f, 0.0f, 1.0f)}),
                TfToken("constant"),
                VtIntArray{1}),
        },
    };

    for (_Case const& testCase : cases) {
        ty::CurveGeometryInput input = _MakeGeometryInput(points, 0.4f);
        input.primvarWidths = testCase.width;
        input.builtInWidths = _MakePrimvar(
            VtValue(VtFloatArray{2.0f}), TfToken("constant"));
        input.primvarNormals = testCase.normal;
        input.builtInNormals = _MakePrimvar(
            VtValue(VtVec3fArray{GfVec3f(0.0f, 0.0f, 1.0f)}),
            TfToken("constant"));
        const ty::CurveGeometryResult result =
            ty::BuildCurveGeometry(topology, input);
        if (!_ExpectGeometryValid(result, testCase.name) ||
            result.widthStatus.selectedSource !=
                ty::CurveAttributeSource::primvar ||
            result.widthStatus.effectiveSource !=
                ty::CurveAttributeSource::minimum ||
            result.widthStatus.error != testCase.error ||
            result.normalStatus.selectedSource !=
                ty::CurveAttributeSource::primvar ||
            result.normalStatus.effectiveSource !=
                ty::CurveAttributeSource::none ||
            result.normalStatus.error != testCase.error ||
            result.recovery.invalidWidthFallbackCount != 1 ||
            result.recovery.invalidNormalSourceCount != 1 ||
            result.primitives.size() != 2) {
            return false;
        }
        for (ty::CurveGeometryPrimitive const& primitive :
             result.primitives) {
            if (primitive.shape != ty::CurveGeometryShape::tube ||
                !_Close(
                    ty::EvaluateCurveRadius(primitive, 0.5f), 0.2f)) {
                return false;
            }
        }
    }
    return true;
}

bool
TestAttributeInterpolationDomains()
{
    const ty::CurveTopologyResult topology =
        ty::CanonicalizeCurveTopology(_MakeInput(
            HdTokens->linear,
            TfToken(),
            HdTokens->nonperiodic,
            VtIntArray{3, 3},
            6));
    const VtVec3fArray points = {
        GfVec3f(0.0f, 0.0f, 0.0f),
        GfVec3f(1.0f, 0.0f, 0.0f),
        GfVec3f(2.0f, 0.0f, 0.0f),
        GfVec3f(0.0f, 1.0f, 0.0f),
        GfVec3f(1.0f, 1.0f, 0.0f),
        GfVec3f(2.0f, 1.0f, 0.0f)};
    ty::CurveGeometryInput uniform = _MakeGeometryInput(points, 0.0f);
    uniform.builtInWidths = _MakePrimvar(
        VtValue(VtFloatArray{0.6f, 0.2f}),
        TfToken("uniform"),
        VtIntArray{1, 0});
    uniform.builtInNormals = _MakePrimvar(
        VtValue(VtVec3fArray{
            GfVec3f(0.0f, 0.0f, 1.0f),
            GfVec3f(0.0f, 1.0f, 0.0f)}),
        TfToken("uniform"));
    const ty::CurveGeometryResult uniformResult =
        ty::BuildCurveGeometry(topology, uniform);
    if (!_ExpectGeometryValid(uniformResult, "uniform attributes") ||
        uniformResult.primitives.size() != 4 ||
        !_Close(
            ty::EvaluateCurveRadius(uniformResult.primitives[0], 0.5f),
            0.1f) ||
        !_Close(
            ty::EvaluateCurveRadius(uniformResult.primitives[2], 0.5f),
            0.3f) ||
        !_Close(
            ty::EvaluateCurveNormal(uniformResult.primitives[0], 0.5f),
            GfVec3f(0.0f, 0.0f, 1.0f)) ||
        !_Close(
            ty::EvaluateCurveNormal(uniformResult.primitives[2], 0.5f),
            GfVec3f(0.0f, 1.0f, 0.0f))) {
        return false;
    }

    ty::CurveGeometryInput indexedVertex = _MakeGeometryInput(points, 0.1f);
    indexedVertex.builtInNormals = _MakePrimvar(
        VtValue(VtVec3fArray{
            GfVec3f(0.0f, 0.0f, 1.0f),
            GfVec3f(0.0f, 1.0f, 0.0f)}),
        TfToken(),
        VtIntArray{0, 0, 0, 1, 1, 1});
    const ty::CurveGeometryResult indexedResult =
        ty::BuildCurveGeometry(topology, indexedVertex);
    return _ExpectGeometryValid(indexedResult, "indexed vertex normals") &&
        indexedResult.normalStatus.interpolation ==
            ty::CurveInterpolation::vertex &&
        _Close(
            ty::EvaluateCurveNormal(indexedResult.primitives[0], 0.5f),
            GfVec3f(0.0f, 0.0f, 1.0f)) &&
        _Close(
            ty::EvaluateCurveNormal(indexedResult.primitives[2], 0.5f),
            GfVec3f(0.0f, 1.0f, 0.0f));
}

bool
TestWidthSampleRecovery()
{
    const ty::CurveTopologyResult topology =
        ty::CanonicalizeCurveTopology(_MakeInput(
            HdTokens->linear,
            TfToken(),
            HdTokens->nonperiodic,
            VtIntArray{3},
            3));
    const VtVec3fArray points = {
        GfVec3f(0.0f, 0.0f, 0.0f),
        GfVec3f(1.0f, 0.0f, 0.0f),
        GfVec3f(2.0f, 0.0f, 0.0f)};
    ty::CurveGeometryInput input = _MakeGeometryInput(points, 0.2f);
    input.builtInWidths = _MakePrimvar(
        VtValue(VtFloatArray{
            -1.0f,
            std::numeric_limits<float>::quiet_NaN(),
            std::numeric_limits<float>::infinity()}),
        TfToken("varying"));
    const ty::CurveGeometryResult result =
        ty::BuildCurveGeometry(topology, input);
    if (!_ExpectGeometryValid(result, "width sample recovery") ||
        result.recovery.negativeWidthCount != 1 ||
        result.recovery.nonFiniteWidthCount != 2 ||
        result.primitives.empty()) {
        return false;
    }
    for (ty::CurveGeometryPrimitive const& primitive : result.primitives) {
        for (size_t i = 0; i <= 16; ++i) {
            if (ty::EvaluateCurveRadius(
                    primitive, static_cast<float>(i) / 16.0f) <
                0.1f - 1.0e-6f) {
                return false;
            }
        }
    }

    ty::CurveGeometryInput negativeMinimum = _MakeGeometryInput(points, -1.0f);
    const ty::CurveGeometryResult minimumResult =
        ty::BuildCurveGeometry(topology, negativeMinimum);
    return _ExpectGeometryValid(minimumResult, "negative minimum") &&
        minimumResult.minimumWidth == 0.0f &&
        minimumResult.recovery.negativeMinimumWidthCount == 1 &&
        minimumResult.primitives.empty();
}

bool
TestContinuousMinimumAndZeroWidth()
{
    const ty::CurveTopologyResult topology =
        ty::CanonicalizeCurveTopology(_MakeInput(
            HdTokens->cubic,
            HdTokens->catmullRom,
            HdTokens->nonperiodic,
            VtIntArray{4},
            4));
    ty::CurveGeometryInput input = _MakeGeometryInput(
        VtVec3fArray{
            GfVec3f(-1.0f, 0.0f, 0.0f),
            GfVec3f(0.0f, 0.0f, 0.0f),
            GfVec3f(1.0f, 0.2f, 0.0f),
            GfVec3f(2.0f, 0.0f, 0.0f)},
        0.75f);
    // All controls exceed the minimum, but the Catmull-Rom polynomial
    // undershoots it between controls one and two.
    input.builtInWidths = _MakePrimvar(
        VtValue(VtFloatArray{10.0f, 1.0f, 1.0f, 10.0f}),
        TfToken("vertex"));
    const ty::CurveGeometryResult clamped =
        ty::BuildCurveGeometry(topology, input);
    if (!_ExpectGeometryValid(clamped, "continuous minimum") ||
        clamped.primitives.size() < 3 ||
        clamped.primitives.front().metadata.authoredU0 != 0.0f ||
        clamped.primitives.back().metadata.authoredU1 != 1.0f) {
        return false;
    }
    float previousU = 0.0f;
    for (ty::CurveGeometryPrimitive const& primitive : clamped.primitives) {
        if (primitive.representation !=
                ty::CurveGeometryRepresentation::hermite ||
            !_Close(primitive.metadata.authoredU0, previousU, 2.0e-5f)) {
            return false;
        }
        for (size_t i = 0; i <= 32; ++i) {
            const float u = static_cast<float>(i) / 32.0f;
            if (ty::EvaluateCurveRadius(primitive, u) < 0.375f - 2.0e-5f) {
                std::printf("    continuous minimum was violated\n");
                return false;
            }
        }
        previousU = primitive.metadata.authoredU1;
    }

    const ty::CurveTopologyResult linearTopology =
        ty::CanonicalizeCurveTopology(_MakeInput(
            HdTokens->linear,
            TfToken(),
            HdTokens->nonperiodic,
            VtIntArray{3},
            3));
    ty::CurveGeometryInput zeroInput = _MakeGeometryInput(
        VtVec3fArray{
            GfVec3f(0.0f, 0.0f, 0.0f),
            GfVec3f(1.0f, 0.0f, 0.0f),
            GfVec3f(2.0f, 0.0f, 0.0f)},
        0.0f);
    zeroInput.builtInWidths = _MakePrimvar(
        VtValue(VtFloatArray{0.0f}), TfToken("constant"));
    const ty::CurveGeometryResult zero =
        ty::BuildCurveGeometry(linearTopology, zeroInput);
    if (!_ExpectGeometryValid(zero, "all-zero width") ||
        !zero.primitives.empty() ||
        zero.recovery.allEffectiveWidthsZeroCount != 1) {
        return false;
    }

    ty::CurveGeometryInput tipInput = _MakeGeometryInput(
        zeroInput.points.UncheckedGet<VtVec3fArray>(), 0.0f);
    tipInput.builtInWidths = _MakePrimvar(
        VtValue(VtFloatArray{1.0f, 0.5f, 0.0f}),
        TfToken("varying"));
    const ty::CurveGeometryResult tip =
        ty::BuildCurveGeometry(linearTopology, tipInput);
    if (!_ExpectGeometryValid(tip, "zero-width tip") ||
        tip.primitives.size() != 2 ||
        !_Close(
            ty::EvaluateCurveRadius(tip.primitives.back(), 1.0f), 0.0f)) {
        return false;
    }

    ty::CurveGeometryInput exactCrossing = _MakeGeometryInput(
        zeroInput.points.UncheckedGet<VtVec3fArray>(), 1.0f);
    exactCrossing.builtInWidths = _MakePrimvar(
        VtValue(VtFloatArray{0.0f, 2.0f, 2.0f}),
        TfToken("varying"));
    const ty::CurveGeometryResult crossing =
        ty::BuildCurveGeometry(linearTopology, exactCrossing);
    return _ExpectGeometryValid(crossing, "exact minimum crossing") &&
        crossing.primitives.size() == 3 &&
        crossing.primitives[0].metadata.authoredSegmentId == 0 &&
        crossing.primitives[1].metadata.authoredSegmentId == 0 &&
        _Close(crossing.primitives[0].metadata.authoredU1, 0.5f) &&
        _Close(crossing.primitives[1].metadata.authoredU0, 0.5f) &&
        _Close(ty::EvaluateCurveRadius(crossing.primitives[0], 0.5f), 0.5f) &&
        _Close(ty::EvaluateCurveRadius(crossing.primitives[1], 1.0f), 1.0f);
}

bool
TestNormalFallbackAndRepair()
{
    const ty::CurveTopologyResult topology =
        ty::CanonicalizeCurveTopology(_MakeInput(
            HdTokens->linear,
            TfToken(),
            HdTokens->nonperiodic,
            VtIntArray{3},
            3));
    const VtVec3fArray points = {
        GfVec3f(0.0f, 0.0f, 0.0f),
        GfVec3f(1.0f, 0.0f, 0.0f),
        GfVec3f(2.0f, 0.0f, 0.0f)};

    ty::CurveGeometryInput structural = _MakeGeometryInput(points, 0.1f);
    structural.primvarNormals = _MakePrimvar(
        VtValue(VtFloatArray{1.0f}), TfToken("constant"));
    structural.builtInNormals = _MakePrimvar(
        VtValue(VtVec3fArray{
            GfVec3f(0.0f, 0.0f, 1.0f),
            GfVec3f(0.0f, 0.0f, 1.0f),
            GfVec3f(0.0f, 0.0f, 1.0f)}),
        TfToken("vertex"));
    const ty::CurveGeometryResult structuralResult =
        ty::BuildCurveGeometry(topology, structural);
    if (!_ExpectGeometryValid(structuralResult, "structural normal") ||
        structuralResult.normalStatus.selectedSource !=
            ty::CurveAttributeSource::primvar ||
        structuralResult.normalStatus.effectiveSource !=
            ty::CurveAttributeSource::none ||
        structuralResult.recovery.invalidNormalSourceCount != 1) {
        return false;
    }
    for (ty::CurveGeometryPrimitive const& primitive :
         structuralResult.primitives) {
        if (primitive.shape != ty::CurveGeometryShape::tube) {
            return false;
        }
    }

    ty::CurveGeometryInput repair = _MakeGeometryInput(points, 0.1f);
    repair.builtInNormals = _MakePrimvar(
        VtValue(VtVec3fArray{
            GfVec3f(0.0f, 0.0f, 1.0f),
            GfVec3f(0.0f),
            GfVec3f(0.0f, 0.0f, 1.0f)}),
        TfToken("vertex"));
    const ty::CurveGeometryResult repaired =
        ty::BuildCurveGeometry(topology, repair);
    if (!_ExpectGeometryValid(repaired, "local normal repair") ||
        repaired.recovery.repairedNormalSpanCount != 2 ||
        repaired.recovery.tubeFallbackSpanCount != 0) {
        return false;
    }
    for (ty::CurveGeometryPrimitive const& primitive : repaired.primitives) {
        if (primitive.shape != ty::CurveGeometryShape::ribbon ||
            ty::EvaluateCurveNormal(primitive, 0.5f).GetLength() < 0.99f) {
            return false;
        }
    }

    ty::CurveGeometryInput nonFinite = _MakeGeometryInput(points, 0.1f);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    nonFinite.builtInNormals = _MakePrimvar(
        VtValue(VtVec3fArray{
            GfVec3f(0.0f, 0.0f, 1.0f),
            GfVec3f(nan, 0.0f, 1.0f),
            GfVec3f(0.0f, 0.0f, 1.0f)}),
        TfToken("vertex"));
    const ty::CurveGeometryResult nonFiniteResult =
        ty::BuildCurveGeometry(topology, nonFinite);
    if (!_ExpectGeometryValid(nonFiniteResult, "non-finite normal repair") ||
        nonFiniteResult.recovery.invalidNormalSampleCount != 1 ||
        nonFiniteResult.recovery.repairedNormalSpanCount != 2 ||
        nonFiniteResult.recovery.tubeFallbackSpanCount != 0) {
        return false;
    }

    ty::CurveGeometryInput antipodal = _MakeGeometryInput(points, 0.1f);
    antipodal.builtInNormals = _MakePrimvar(
        VtValue(VtVec3fArray{
            GfVec3f(0.0f, 0.0f, 1.0f),
            GfVec3f(0.0f, 0.0f, -1.0f),
            GfVec3f(0.0f, 0.0f, -1.0f)}),
        TfToken("varying"));
    const ty::CurveGeometryResult antipodalResult =
        ty::BuildCurveGeometry(topology, antipodal);
    if (!_ExpectGeometryValid(antipodalResult, "antipodal normal") ||
        antipodalResult.primitives.size() != 2 ||
        antipodalResult.primitives[0].shape !=
            ty::CurveGeometryShape::tube ||
        antipodalResult.primitives[1].shape !=
            ty::CurveGeometryShape::ribbon ||
        antipodalResult.recovery.tubeFallbackSpanCount != 1) {
        return false;
    }
    return true;
}

bool
TestNormalEndpointAndCyclicRepair()
{
    const ty::CurveTopologyResult nonperiodic =
        ty::CanonicalizeCurveTopology(_MakeInput(
            HdTokens->linear,
            TfToken(),
            HdTokens->nonperiodic,
            VtIntArray{3},
            3));
    const VtVec3fArray linePoints = {
        GfVec3f(0.0f, 0.0f, 0.0f),
        GfVec3f(1.0f, 0.0f, 0.0f),
        GfVec3f(2.0f, 0.0f, 0.0f)};
    ty::CurveGeometryInput oneSided = _MakeGeometryInput(linePoints, 0.1f);
    oneSided.builtInNormals = _MakePrimvar(
        VtValue(VtVec3fArray{
            GfVec3f(0.0f),
            GfVec3f(0.0f, 0.0f, 1.0f),
            GfVec3f(0.0f, 0.0f, 1.0f)}),
        TfToken("vertex"));
    const ty::CurveGeometryResult oneSidedResult =
        ty::BuildCurveGeometry(nonperiodic, oneSided);
    if (!_ExpectGeometryValid(oneSidedResult, "one-sided normal repair") ||
        oneSidedResult.recovery.repairedNormalSpanCount != 1 ||
        oneSidedResult.primitives[0].shape !=
            ty::CurveGeometryShape::ribbon) {
        return false;
    }

    ty::CurveGeometryInput noAnchor = _MakeGeometryInput(linePoints, 0.1f);
    noAnchor.builtInNormals = _MakePrimvar(
        VtValue(VtVec3fArray{
            GfVec3f(0.0f), GfVec3f(0.0f), GfVec3f(0.0f)}),
        TfToken("vertex"));
    const ty::CurveGeometryResult noAnchorResult =
        ty::BuildCurveGeometry(nonperiodic, noAnchor);
    if (!_ExpectGeometryValid(noAnchorResult, "no-anchor normal fallback") ||
        noAnchorResult.recovery.tubeFallbackSpanCount != 2) {
        return false;
    }
    for (ty::CurveGeometryPrimitive const& primitive :
         noAnchorResult.primitives) {
        if (primitive.shape != ty::CurveGeometryShape::tube) {
            return false;
        }
    }

    const ty::CurveTopologyResult periodic =
        ty::CanonicalizeCurveTopology(_MakeInput(
            HdTokens->linear,
            TfToken(),
            HdTokens->periodic,
            VtIntArray{4},
            4));
    ty::CurveGeometryInput cyclic = _MakeGeometryInput(
        VtVec3fArray{
            GfVec3f(0.0f, 0.0f, 0.0f),
            GfVec3f(1.0f, 0.0f, 0.0f),
            GfVec3f(1.0f, 1.0f, 0.0f),
            GfVec3f(0.0f, 1.0f, 0.0f)},
        0.1f);
    cyclic.builtInNormals = _MakePrimvar(
        VtValue(VtVec3fArray{
            GfVec3f(0.0f),
            GfVec3f(0.0f, 0.0f, 1.0f),
            GfVec3f(0.0f, 0.0f, 1.0f),
            GfVec3f(0.0f, 0.0f, 1.0f)}),
        TfToken("vertex"));
    const ty::CurveGeometryResult cyclicResult =
        ty::BuildCurveGeometry(periodic, cyclic);
    if (!_ExpectGeometryValid(cyclicResult, "cyclic normal repair") ||
        cyclicResult.recovery.tubeFallbackSpanCount != 0 ||
        cyclicResult.recovery.repairedNormalSpanCount != 2) {
        return false;
    }
    for (ty::CurveGeometryPrimitive const& primitive :
         cyclicResult.primitives) {
        if (primitive.shape != ty::CurveGeometryShape::ribbon) {
            return false;
        }
    }
    return true;
}

bool
TestHermiteSelectionForVaryingNormal()
{
    const ty::CurveTopologyResult topology =
        ty::CanonicalizeCurveTopology(_MakeInput(
            HdTokens->cubic,
            HdTokens->bspline,
            HdTokens->nonperiodic,
            VtIntArray{5},
            5));
    const VtVec3fArray points = {
        GfVec3f(-1.0f, 0.0f, 0.0f),
        GfVec3f(0.0f, 0.0f, 0.0f),
        GfVec3f(1.0f, 0.2f, 0.0f),
        GfVec3f(2.0f, 0.2f, 0.0f),
        GfVec3f(3.0f, 0.0f, 0.0f)};
    ty::CurveGeometryInput varying = _MakeGeometryInput(points, 0.2f);
    varying.builtInNormals = _MakePrimvar(
        VtValue(VtVec3fArray{
            GfVec3f(0.0f, 0.0f, 1.0f),
            GfVec3f(0.0f, 1.0f, 1.0f),
            GfVec3f(0.0f, 0.0f, 1.0f)}),
        TfToken("varying"));
    const ty::CurveGeometryResult varyingResult =
        ty::BuildCurveGeometry(topology, varying);
    if (!_ExpectGeometryValid(varyingResult, "varying normal Hermite") ||
        varyingResult.primitives.size() != 2) {
        return false;
    }
    for (ty::CurveGeometryPrimitive const& primitive :
         varyingResult.primitives) {
        if (primitive.representation !=
                ty::CurveGeometryRepresentation::hermite ||
            primitive.shape != ty::CurveGeometryShape::ribbon) {
            return false;
        }
    }

    ty::CurveGeometryInput vertex = _MakeGeometryInput(points, 0.2f);
    vertex.builtInNormals = _MakePrimvar(
        VtValue(VtVec3fArray{
            GfVec3f(0.0f, 0.0f, 1.0f),
            GfVec3f(0.0f, 0.0f, 1.0f),
            GfVec3f(0.0f, 0.0f, 1.0f),
            GfVec3f(0.0f, 0.0f, 1.0f),
            GfVec3f(0.0f, 0.0f, 1.0f)}),
        TfToken("vertex"));
    const ty::CurveGeometryResult vertexResult =
        ty::BuildCurveGeometry(topology, vertex);
    if (!_ExpectGeometryValid(vertexResult, "vertex normal native basis") ||
        vertexResult.primitives.size() != 2) {
        return false;
    }
    for (ty::CurveGeometryPrimitive const& primitive :
         vertexResult.primitives) {
        if (primitive.representation !=
                ty::CurveGeometryRepresentation::nativeBspline ||
            primitive.shape != ty::CurveGeometryShape::ribbon) {
            return false;
        }
    }
    return true;
}

bool
TestInteriorNormalDegeneracyRepair()
{
    const ty::CurveTopologyResult topology =
        ty::CanonicalizeCurveTopology(_MakeInput(
            HdTokens->cubic,
            HdTokens->bezier,
            HdTokens->nonperiodic,
            VtIntArray{4},
            4));
    ty::CurveGeometryInput input = _MakeGeometryInput(
        VtVec3fArray{
            GfVec3f(0.0f, 0.0f, 0.0f),
            GfVec3f(1.0f / 3.0f),
            GfVec3f(2.0f / 3.0f),
            GfVec3f(1.0f)},
        0.1f);
    // The curve has tangent T=(1,1,1), while these controls form
    // n(u)=T+3*(u-5/32)^2*(1,-1,0). The normal becomes parallel to T without
    // changing orientation at a parameter not covered by fixed probes.
    input.builtInNormals = _MakePrimvar(
        VtValue(VtVec3fArray{
            GfVec3f(1.0732421875f, 0.9267578125f, 1.0f),
            GfVec3f(0.7607421875f, 1.2392578125f, 1.0f),
            GfVec3f(1.4482421875f, 0.5517578125f, 1.0f),
            GfVec3f(3.1357421875f, -1.1357421875f, 1.0f)}),
        TfToken("vertex"));
    const ty::CurveGeometryResult result =
        ty::BuildCurveGeometry(topology, input);
    const GfVec3f repairedNormal = result.primitives.empty()
        ? GfVec3f(0.0f)
        : ty::EvaluateCurveNormal(
            result.primitives[0], 5.0f / 32.0f);
    const GfVec3f tangent = result.primitives.empty()
        ? GfVec3f(0.0f)
        : ty::EvaluateCurveTangent(
            result.primitives[0], 5.0f / 32.0f).GetNormalized();
    const bool valid =
        _ExpectGeometryValid(result, "interior normal degeneracy") &&
        result.primitives.size() == 1 &&
        result.primitives[0].representation ==
            ty::CurveGeometryRepresentation::hermite &&
        result.primitives[0].shape == ty::CurveGeometryShape::ribbon &&
        result.recovery.localNormalIssueSpanCount == 1 &&
        result.recovery.repairedNormalSpanCount == 1 &&
        repairedNormal.GetLength() > 0.999f &&
        std::abs(GfDot(repairedNormal, tangent)) < 2.0e-4f;
    if (!valid) {
        const int representation = result.primitives.empty()
            ? -1
            : static_cast<int>(result.primitives[0].representation);
        const int shape = result.primitives.empty()
            ? -1
            : static_cast<int>(result.primitives[0].shape);
        const GfVec3f normal = result.primitives.empty()
            ? GfVec3f(0.0f)
            : ty::EvaluateCurveNormal(result.primitives[0], 0.3f);
        std::printf(
            "    interior normal: plans=%zu rep=%d shape=%d issues=%zu "
            "repaired=%zu tube=%zu normal=(%g,%g,%g)\n",
            result.primitives.size(),
            representation,
            shape,
            result.recovery.localNormalIssueSpanCount,
            result.recovery.repairedNormalSpanCount,
            result.recovery.tubeFallbackSpanCount,
            static_cast<double>(normal[0]),
            static_cast<double>(normal[1]),
            static_cast<double>(normal[2]));
    }
    return valid;
}

bool
TestCenterlineDegeneracyFallback()
{
    const ty::CurveTopologyResult topology =
        ty::CanonicalizeCurveTopology(_MakeInput(
            HdTokens->cubic,
            HdTokens->bezier,
            HdTokens->nonperiodic,
            VtIntArray{4},
            4));
    ty::CurveGeometryInput cusp = _MakeGeometryInput(
        VtVec3fArray{
            GfVec3f(0.0f, 0.0f, 0.0f),
            GfVec3f(1.0f, 0.0f, 0.0f),
            GfVec3f(-1.0f, 0.0f, 0.0f),
            GfVec3f(0.0f, 0.0f, 0.0f)},
        0.2f);
    const ty::CurveGeometryResult cuspResult =
        ty::BuildCurveGeometry(topology, cusp);
    if (!_ExpectGeometryValid(cuspResult, "cusp fallback") ||
        cuspResult.primitives.empty() ||
        cuspResult.recovery.linearizedSpanCount != 1 ||
        cuspResult.recovery.spherePointSpanCount != 0) {
        return false;
    }
    float previousU = 0.0f;
    for (ty::CurveGeometryPrimitive const& primitive :
         cuspResult.primitives) {
        if (primitive.representation !=
                ty::CurveGeometryRepresentation::roundLinear ||
            primitive.shape != ty::CurveGeometryShape::tube ||
            primitive.metadata.authoredU0 < previousU ||
            primitive.metadata.authoredU1 <= primitive.metadata.authoredU0) {
            return false;
        }
        previousU = primitive.metadata.authoredU1;
    }

    ty::CurveGeometryInput endpointTangent = _MakeGeometryInput(
        VtVec3fArray{
            GfVec3f(0.0f, 0.0f, 0.0f),
            GfVec3f(0.0f, 0.0f, 0.0f),
            GfVec3f(1.0f, 0.0f, 0.0f),
            GfVec3f(2.0f, 0.0f, 0.0f)},
        0.2f);
    const ty::CurveGeometryResult endpointResult =
        ty::BuildCurveGeometry(topology, endpointTangent);
    if (!_ExpectGeometryValid(endpointResult, "zero endpoint tangent") ||
        endpointResult.primitives.empty() ||
        endpointResult.recovery.linearizedSpanCount != 1 ||
        endpointResult.recovery.spherePointSpanCount != 0 ||
        endpointResult.primitives.front().metadata.authoredU0 != 0.0f ||
        endpointResult.primitives.back().metadata.authoredU1 != 1.0f) {
        return false;
    }
    for (ty::CurveGeometryPrimitive const& primitive :
         endpointResult.primitives) {
        if (primitive.representation !=
                ty::CurveGeometryRepresentation::roundLinear ||
            primitive.shape != ty::CurveGeometryShape::tube) {
            return false;
        }
    }

    // The width threshold isolates a tiny retraced loop whose endpoints are
    // indistinguishable at the centerline tolerance. Adaptive linearization
    // must discard that generated zero-length piece rather than emit a cap.
    ty::CurveGeometryInput zeroLength = _MakeGeometryInput(
        VtVec3fArray{
            GfVec3f(0.0f, 0.0f, 0.0f),
            GfVec3f(0.0211870405f, 0.0f, 0.0f),
            GfVec3f(-0.0416645474f, 0.0f, 0.0f),
            GfVec3f(0.14477857f, 0.0f, 0.0f)},
        0.2f);
    zeroLength.builtInWidths = _MakePrimvar(
        VtValue(VtFloatArray{
            0.263476563f,
            0.0955078125f,
            0.260872396f,
            0.759570313f}),
        TfToken("vertex"));
    const ty::CurveGeometryResult zeroLengthResult =
        ty::BuildCurveGeometry(topology, zeroLength);
    if (!_ExpectGeometryValid(
            zeroLengthResult, "zero-length generated segment") ||
        zeroLengthResult.primitives.empty() ||
        zeroLengthResult.recovery.linearizedSpanCount != 1 ||
        zeroLengthResult.recovery.removedZeroLengthSegmentCount == 0 ||
        zeroLengthResult.recovery.spherePointSpanCount != 0) {
        return false;
    }

    ty::CurveGeometryInput collapsed = _MakeGeometryInput(
        VtVec3fArray{
            GfVec3f(2.0f, 3.0f, 4.0f),
            GfVec3f(2.0f, 3.0f, 4.0f),
            GfVec3f(2.0f, 3.0f, 4.0f),
            GfVec3f(2.0f, 3.0f, 4.0f)},
        0.2f);
    const ty::CurveGeometryResult collapsedResult =
        ty::BuildCurveGeometry(topology, collapsed);
    return _ExpectGeometryValid(collapsedResult, "collapsed fallback") &&
        collapsedResult.primitives.size() == 1 &&
        collapsedResult.primitives[0].representation ==
            ty::CurveGeometryRepresentation::spherePoint &&
        collapsedResult.recovery.spherePointSpanCount == 1 &&
        _Close(
            ty::EvaluateCurvePosition(
                collapsedResult.primitives[0], 0.5f),
            GfVec3f(2.0f, 3.0f, 4.0f));
}

bool
TestScaleInvariantGeometryClassification()
{
    const ty::CurveTopologyResult topology =
        ty::CanonicalizeCurveTopology(_MakeInput(
            HdTokens->cubic,
            HdTokens->bezier,
            HdTokens->nonperiodic,
            VtIntArray{4},
            4));
    ty::CurveGeometryInput input = _MakeGeometryInput(
        VtVec3fArray{
            GfVec3f(0.0f, 0.0f, 0.0f),
            GfVec3f(1.0e-12f, 0.0f, 0.0f),
            GfVec3f(2.0e-12f, 0.0f, 0.0f),
            GfVec3f(3.0e-12f, 0.0f, 0.0f)},
        0.2f);
    input.builtInNormals = _MakePrimvar(
        VtValue(VtVec3fArray{
            GfVec3f(0.0f, 0.0f, 1.0e-12f),
            GfVec3f(0.0f, 0.0f, 1.0e-12f),
            GfVec3f(0.0f, 0.0f, 1.0e-12f),
            GfVec3f(0.0f, 0.0f, 1.0e-12f)}),
        TfToken("vertex"));
    const ty::CurveGeometryResult result =
        ty::BuildCurveGeometry(topology, input);
    return _ExpectGeometryValid(result, "small finite geometry") &&
        result.primitives.size() == 1 &&
        result.primitives[0].representation ==
            ty::CurveGeometryRepresentation::nativeBezier &&
        result.primitives[0].shape == ty::CurveGeometryShape::ribbon &&
        result.recovery.linearizedSpanCount == 0 &&
        result.recovery.spherePointSpanCount == 0 &&
        _Close(
            ty::EvaluateCurveNormal(result.primitives[0], 0.5f),
            GfVec3f(0.0f, 0.0f, 1.0f));
}

ty::CurveGeometryRecordData const*
_FindRecord(
    ty::CurveGeometryRecordBuildResult const& result,
    ty::CurveGeometryShape shape,
    ty::CurveGeometryRepresentation representation)
{
    for (std::unique_ptr<ty::CurveGeometryRecordData> const& record :
         result.records) {
        if (record->GetShape() == shape &&
            record->GetRepresentation() == representation) {
            return record.get();
        }
    }
    return nullptr;
}

bool
TestCurveEmbreeRecords()
{
    struct _TypeCase
    {
        ty::CurveGeometryShape shape;
        ty::CurveGeometryRepresentation representation;
        RTCGeometryType expected;
    };
    const _TypeCase typeCases[] = {
        {ty::CurveGeometryShape::tube,
         ty::CurveGeometryRepresentation::nativeBezier,
         RTC_GEOMETRY_TYPE_ROUND_BEZIER_CURVE},
        {ty::CurveGeometryShape::ribbon,
         ty::CurveGeometryRepresentation::nativeBezier,
         RTC_GEOMETRY_TYPE_NORMAL_ORIENTED_BEZIER_CURVE},
        {ty::CurveGeometryShape::tube,
         ty::CurveGeometryRepresentation::nativeBspline,
         RTC_GEOMETRY_TYPE_ROUND_BSPLINE_CURVE},
        {ty::CurveGeometryShape::ribbon,
         ty::CurveGeometryRepresentation::nativeBspline,
         RTC_GEOMETRY_TYPE_NORMAL_ORIENTED_BSPLINE_CURVE},
        {ty::CurveGeometryShape::tube,
         ty::CurveGeometryRepresentation::nativeCatmullRom,
         RTC_GEOMETRY_TYPE_ROUND_CATMULL_ROM_CURVE},
        {ty::CurveGeometryShape::ribbon,
         ty::CurveGeometryRepresentation::nativeCatmullRom,
         RTC_GEOMETRY_TYPE_NORMAL_ORIENTED_CATMULL_ROM_CURVE},
        {ty::CurveGeometryShape::tube,
         ty::CurveGeometryRepresentation::hermite,
         RTC_GEOMETRY_TYPE_ROUND_HERMITE_CURVE},
        {ty::CurveGeometryShape::ribbon,
         ty::CurveGeometryRepresentation::hermite,
         RTC_GEOMETRY_TYPE_NORMAL_ORIENTED_HERMITE_CURVE},
        {ty::CurveGeometryShape::tube,
         ty::CurveGeometryRepresentation::roundLinear,
         RTC_GEOMETRY_TYPE_ROUND_LINEAR_CURVE},
        {ty::CurveGeometryShape::tube,
         ty::CurveGeometryRepresentation::spherePoint,
         RTC_GEOMETRY_TYPE_SPHERE_POINT},
    };
    for (_TypeCase const& typeCase : typeCases) {
        RTCGeometryType actual = RTC_GEOMETRY_TYPE_TRIANGLE;
        if (!ty::GetEmbreeCurveGeometryType(
                typeCase.shape, typeCase.representation, &actual) ||
            actual != typeCase.expected) {
            std::printf("    curve Embree type mapping was incorrect\n");
            return false;
        }
    }
    RTCGeometryType invalidType = RTC_GEOMETRY_TYPE_TRIANGLE;
    if (ty::GetEmbreeCurveGeometryType(
            ty::CurveGeometryShape::ribbon,
            ty::CurveGeometryRepresentation::roundLinear,
            &invalidType) ||
        ty::GetEmbreeCurveGeometryType(
            ty::CurveGeometryShape::ribbon,
            ty::CurveGeometryRepresentation::spherePoint,
            &invalidType) ||
        ty::GetEmbreeCurveGeometryType(
            ty::CurveGeometryShape::tube,
            ty::CurveGeometryRepresentation::hermite,
            nullptr)) {
        std::printf("    invalid curve Embree mapping was accepted\n");
        return false;
    }

    VtVec3fArray const linearPoints = {
        GfVec3f(0.0f, 0.0f, 0.0f),
        GfVec3f(1.0f, 0.0f, 0.0f),
        GfVec3f(2.0f, 0.0f, 0.0f)};
    ty::CurveTopologyResult const linearTopology =
        ty::CanonicalizeCurveTopology(_MakeInput(
            HdTokens->linear,
            HdTokens->bezier,
            HdTokens->nonperiodic,
            VtIntArray{3},
            linearPoints.size()));
    ty::CurveGeometryInput linearInput = _MakeGeometryInput(linearPoints);
    linearInput.builtInWidths = _MakePrimvar(
        VtFloatArray{0.4f}, HdPrimvarSchemaTokens->constant);
    ty::CurveGeometryResult const linearGeometry =
        ty::BuildCurveGeometry(linearTopology, linearInput);
    ty::CurveGeometryRecordBuildResult const linearRecords =
        ty::BuildCurveGeometryRecords(linearTopology, linearGeometry);
    ty::CurveGeometryRecordData const* const linearRecord = _FindRecord(
        linearRecords,
        ty::CurveGeometryShape::tube,
        ty::CurveGeometryRepresentation::roundLinear);
    if (!linearRecords.IsValid() || linearRecords.records.size() != 1 ||
        linearRecord == nullptr || linearRecord->GetPrimitiveCount() != 2 ||
        linearRecord->GetVertices().size() != 8 ||
        linearRecord->GetIndices() !=
            std::vector<unsigned int>({1u, 5u}) ||
        linearRecord->GetCurveFlags().size() != 2 ||
        linearRecord->GetCurveFlags()[0] !=
            static_cast<std::uint32_t>(RTC_CURVE_FLAG_NEIGHBOR_RIGHT) ||
        linearRecord->GetCurveFlags()[1] !=
            static_cast<std::uint32_t>(RTC_CURVE_FLAG_NEIGHBOR_LEFT) ||
        linearRecord->GetContext().geometryKind !=
            ty::GeometryKind::roundCurve ||
        linearRecord->GetContext().curvePrimitiveMetadata.size() != 2 ||
        !_Close(linearRecord->GetVertices()[1][3], 0.2f) ||
        !_Close(linearRecord->GetVertices()[2][3], 0.2f)) {
        std::printf("    round-linear record layout was incorrect\n");
        return false;
    }

    ty::CurveGeometryInput ribbonInput = linearInput;
    ribbonInput.builtInNormals = _MakePrimvar(
        VtVec3fArray{GfVec3f(0.0f, 1.0f, 0.0f)},
        HdPrimvarSchemaTokens->constant);
    ty::CurveGeometryResult const ribbonGeometry =
        ty::BuildCurveGeometry(linearTopology, ribbonInput);
    ty::CurveGeometryRecordBuildResult const ribbonRecords =
        ty::BuildCurveGeometryRecords(linearTopology, ribbonGeometry);
    ty::CurveGeometryRecordData const* const ribbonRecord = _FindRecord(
        ribbonRecords,
        ty::CurveGeometryShape::ribbon,
        ty::CurveGeometryRepresentation::hermite);
    if (!ribbonRecords.IsValid() || ribbonRecords.records.size() != 1 ||
        ribbonRecord == nullptr || ribbonRecord->GetPrimitiveCount() != 2 ||
        ribbonRecord->GetGeometryType() !=
            RTC_GEOMETRY_TYPE_NORMAL_ORIENTED_HERMITE_CURVE ||
        ribbonRecord->GetVertices().size() != 4 ||
        ribbonRecord->GetTangents().size() != 4 ||
        ribbonRecord->GetNormals().size() != 4 ||
        ribbonRecord->GetNormalDerivatives().size() != 4 ||
        ribbonRecord->GetContext().geometryKind !=
            ty::GeometryKind::orientedRibbon) {
        std::printf("    oriented Hermite record layout was incorrect\n");
        return false;
    }

    VtVec3fArray const collapsedPoints = {
        GfVec3f(1.0f, 2.0f, 3.0f),
        GfVec3f(1.0f, 2.0f, 3.0f),
        GfVec3f(1.0f, 2.0f, 3.0f)};
    ty::CurveGeometryInput collapsedInput =
        _MakeGeometryInput(collapsedPoints);
    collapsedInput.builtInWidths = linearInput.builtInWidths;
    ty::CurveGeometryResult const collapsedGeometry =
        ty::BuildCurveGeometry(linearTopology, collapsedInput);
    ty::CurveGeometryRecordBuildResult const collapsedRecords =
        ty::BuildCurveGeometryRecords(linearTopology, collapsedGeometry);
    ty::CurveGeometryRecordData const* const sphereRecord = _FindRecord(
        collapsedRecords,
        ty::CurveGeometryShape::tube,
        ty::CurveGeometryRepresentation::spherePoint);
    if (!collapsedRecords.IsValid() || sphereRecord == nullptr ||
        sphereRecord->GetGeometryType() != RTC_GEOMETRY_TYPE_SPHERE_POINT ||
        sphereRecord->GetVertices().size() != 2 ||
        !sphereRecord->GetIndices().empty() ||
        !sphereRecord->GetTangents().empty() ||
        !sphereRecord->GetCurveFlags().empty()) {
        std::printf("    sphere-point record layout was incorrect\n");
        return false;
    }

    ty::DecodedCurveHit decoded;
    std::vector<ty::CurveSegmentMetadata> metadata(1);
    metadata[0].authoredCurveId = 7;
    metadata[0].authoredSegmentId = 11;
    metadata[0].authoredU0 = 0.25f;
    metadata[0].authoredU1 = 0.75f;
    if (!ty::DecodeCurveHit(metadata, 0, 0.4f, &decoded) ||
        decoded.authoredCurveId != 7 ||
        decoded.authoredSegmentId != 11 ||
        !_Close(decoded.authoredU, 0.45f) ||
        ty::DecodeCurveHit(metadata, 1, 0.5f, &decoded) ||
        ty::DecodeCurveHit(metadata, 0, -0.1f, &decoded) ||
        ty::DecodeCurveHit(metadata, 0, 0.5f, nullptr)) {
        std::printf("    generated curve hit decode was incorrect\n");
        return false;
    }
    return true;
}

bool
_SampleFloat(
    ty::CurvePrimvarSamplerResult const& result,
    unsigned int element,
    float u,
    float expected,
    char const* context)
{
    float value = -999.0f;
    if (!result.IsValid() ||
        !result.sampler->Sample(element, u, 0.0f, &value) ||
        !_Close(value, expected)) {
        std::printf(
            "    %s: expected %g, got %g (%s)\n",
            context,
            expected,
            value,
            ty::GetCurveSamplerErrorName(result.diagnostic.error));
        return false;
    }
    return true;
}

bool
TestCurvePrimvarSamplers()
{
    ty::CurveTopologyResult const topology =
        ty::CanonicalizeCurveTopology(_MakeInput(
            HdTokens->cubic,
            HdTokens->bezier,
            HdTokens->nonperiodic,
            VtIntArray{4, 4},
            8));
    if (!_ExpectValid(topology, "curve sampler topology") ||
        topology.segments.size() != 2) {
        return false;
    }
    std::vector<ty::CurveSegmentMetadata> metadata = {
        topology.segments[0].metadata,
        topology.segments[1].metadata};
    metadata[0].authoredU0 = 0.25f;
    metadata[0].authoredU1 = 0.75f;

    ty::CurvePrimvarSamplerResult const constant =
        ty::CreateCurvePrimvarSampler(
            TfToken("constantTest"),
            _MakePrimvar(
                VtFloatArray{3.0f},
                HdPrimvarSchemaTokens->constant),
            topology,
            metadata);
    ty::CurvePrimvarSamplerResult const uniform =
        ty::CreateCurvePrimvarSampler(
            TfToken("uniformTest"),
            _MakePrimvar(
                VtFloatArray{10.0f, 20.0f},
                HdPrimvarSchemaTokens->uniform),
            topology,
            metadata);
    ty::CurvePrimvarSamplerResult const varying =
        ty::CreateCurvePrimvarSampler(
            TfToken("varyingTest"),
            _MakePrimvar(
                VtFloatArray{0.0f, 10.0f, 20.0f, 30.0f},
                HdPrimvarSchemaTokens->varying),
            topology,
            metadata);
    ty::CurvePrimvarSamplerResult const vertex =
        ty::CreateCurvePrimvarSampler(
            TfToken("vertexTest"),
            _MakePrimvar(
                VtFloatArray{
                    0.0f, 0.0f, 0.0f, 8.0f,
                    10.0f, 20.0f, 30.0f, 40.0f},
                HdPrimvarSchemaTokens->vertex),
            topology,
            metadata);
    ty::CurvePrimvarSamplerResult const indexed =
        ty::CreateCurvePrimvarSampler(
            TfToken("indexedTest"),
            _MakePrimvar(
                VtFloatArray{100.0f, 200.0f},
                HdPrimvarSchemaTokens->uniform,
                VtIntArray{1, 0},
                true),
            topology,
            metadata);
    if (!_SampleFloat(constant, 1, 0.3f, 3.0f, "constant") ||
        !_SampleFloat(uniform, 1, 0.3f, 20.0f, "uniform") ||
        !_SampleFloat(varying, 0, 0.5f, 5.0f, "varying remap") ||
        !_SampleFloat(varying, 1, 0.25f, 22.5f, "varying curve") ||
        !_SampleFloat(vertex, 0, 0.5f, 1.0f, "Bezier vertex remap") ||
        !_SampleFloat(indexed, 0, 0.5f, 200.0f, "indexed uniform 0") ||
        !_SampleFloat(indexed, 1, 0.5f, 100.0f, "indexed uniform 1")) {
        return false;
    }

    ty::CurveTopologyResult const periodicTopology =
        ty::CanonicalizeCurveTopology(_MakeInput(
            HdTokens->linear,
            HdTokens->bezier,
            HdTokens->periodic,
            VtIntArray{4},
            4));
    std::vector<ty::CurveSegmentMetadata> periodicMetadata;
    for (ty::CanonicalCurveSegment const& segment :
         periodicTopology.segments) {
        periodicMetadata.push_back(segment.metadata);
    }
    ty::CurvePrimvarSamplerResult const periodic =
        ty::CreateCurvePrimvarSampler(
            TfToken("periodicVarying"),
            _MakePrimvar(
                VtFloatArray{0.0f, 10.0f, 20.0f, 30.0f},
                HdPrimvarSchemaTokens->varying),
            periodicTopology,
            periodicMetadata);
    if (!_SampleFloat(periodic, 3, 0.5f, 15.0f, "periodic seam")) {
        return false;
    }

    ty::CurveTopologyResult const pinnedTopology =
        ty::CanonicalizeCurveTopology(_MakeInput(
            HdTokens->cubic,
            HdTokens->bspline,
            HdTokens->pinned,
            VtIntArray{2},
            2));
    std::vector<ty::CurveSegmentMetadata> const pinnedMetadata = {
        pinnedTopology.segments[0].metadata};
    ty::CurvePrimvarSamplerResult const pinned =
        ty::CreateCurvePrimvarSampler(
            TfToken("pinnedVertex"),
            _MakePrimvar(
                VtFloatArray{2.0f, 4.0f},
                HdPrimvarSchemaTokens->vertex),
            pinnedTopology,
            pinnedMetadata);
    if (!_SampleFloat(pinned, 0, 0.0f, 2.0f, "pinned start") ||
        !_SampleFloat(pinned, 0, 0.5f, 3.0f, "pinned middle") ||
        !_SampleFloat(pinned, 0, 1.0f, 4.0f, "pinned end")) {
        return false;
    }

    // Topology indices select physical vertex slots first. Primvar indices
    // independently flatten the authored value source into those slots.
    ty::CurveTopologyResult const indexedTopology =
        ty::CanonicalizeCurveTopology(_MakeInput(
            HdTokens->cubic,
            HdTokens->bezier,
            HdTokens->nonperiodic,
            VtIntArray{4},
            5,
            VtIntArray{3, 2, 1, 0}));
    std::vector<ty::CurveSegmentMetadata> const indexedMetadata = {
        indexedTopology.segments[0].metadata};
    ty::CurvePrimvarSamplerResult const indexedVertex =
        ty::CreateCurvePrimvarSampler(
            TfToken("independentIndices"),
            _MakePrimvar(
                VtFloatArray{10.0f, 20.0f, 30.0f, 40.0f, 50.0f},
                HdPrimvarSchemaTokens->vertex,
                VtIntArray{4, 3, 2, 1, 0},
                true),
            indexedTopology,
            indexedMetadata);
    if (!_SampleFloat(
            indexedVertex,
            0,
            0.0f,
            20.0f,
            "independent topology/primvar indices")) {
        return false;
    }

    ty::CurvePrimvarSamplerResult const faceVarying =
        ty::CreateCurvePrimvarSampler(
            TfToken("faceVaryingTest"),
            _MakePrimvar(VtFloatArray{1.0f}, TfToken("faceVarying")),
            topology,
            metadata);
    ty::CurvePrimvarSamplerResult const badCount =
        ty::CreateCurvePrimvarSampler(
            TfToken("badCount"),
            _MakePrimvar(
                VtFloatArray{1.0f}, HdPrimvarSchemaTokens->uniform),
            topology,
            metadata);
    ty::CurvePrimvarSamplerResult const badIndexCount =
        ty::CreateCurvePrimvarSampler(
            TfToken("badIndexCount"),
            _MakePrimvar(
                VtFloatArray{1.0f},
                HdPrimvarSchemaTokens->uniform,
                VtIntArray{0},
                true),
            topology,
            metadata);
    ty::CurvePrimvarSamplerResult const badIndex =
        ty::CreateCurvePrimvarSampler(
            TfToken("badIndex"),
            _MakePrimvar(
                VtFloatArray{1.0f},
                HdPrimvarSchemaTokens->uniform,
                VtIntArray{0, 2},
                true),
            topology,
            metadata);
    std::vector<ty::CurveSegmentMetadata> badMetadata = metadata;
    badMetadata[0].authoredCurveId = 99;
    ty::CurvePrimvarSamplerResult const invalidMetadata =
        ty::CreateCurvePrimvarSampler(
            TfToken("badMetadata"),
            _MakePrimvar(
                VtFloatArray{1.0f}, HdPrimvarSchemaTokens->constant),
            topology,
            badMetadata);
    if (faceVarying.diagnostic.error !=
            ty::CurveSamplerError::invalidInterpolation ||
        faceVarying.sampler != nullptr ||
        badCount.diagnostic.error !=
            ty::CurveSamplerError::elementCountMismatch ||
        badIndexCount.diagnostic.error !=
            ty::CurveSamplerError::indexCountMismatch ||
        badIndex.diagnostic.error != ty::CurveSamplerError::invalidIndex ||
        badIndex.diagnostic.elementIndex != 1 ||
        invalidMetadata.diagnostic.error !=
            ty::CurveSamplerError::invalidPrimitiveMetadata ||
        !constant.IsValid()) {
        std::printf("    curve sampler error isolation was incorrect\n");
        return false;
    }
    return true;
}

struct _CurveRprimTestContext
{
    _CurveRprimTestContext()
        : renderIndex(HdRenderIndex::New(
              &renderDelegate, HdDriverVector()))
    {
    }

    HdEmbreeRenderDelegate renderDelegate;
    std::unique_ptr<HdRenderIndex> renderIndex;
};

struct _FactoryRprimDeleter
{
    HdEmbreeRenderDelegate* renderDelegate = nullptr;

    void operator()(HdRprim* rprim) const
    {
        if (!rprim || !renderDelegate) {
            return;
        }
        rprim->Finalize(renderDelegate->GetRenderParam());
        renderDelegate->DestroyRprim(rprim);
    }
};

HdMaterialNetwork2
_MakeCurveGeomPropMaterialNetwork(TfToken const& geomPropName)
{
    HdMaterialNetwork2 network;
    SdfPath const geomPropPath("/Material/GeomProp");
    HdMaterialNode2 geomProp;
    geomProp.nodeTypeId = TfToken("ND_geompropvalue_float");
    geomProp.parameters[TfToken("geomprop")] =
        VtValue(geomPropName.GetString());
    geomProp.parameters[TfToken("default")] = VtValue(0.5f);
    network.nodes[geomPropPath] = geomProp;

    SdfPath const surfacePath("/Material/Surface");
    HdMaterialNode2 surface;
    surface.nodeTypeId = TfToken("UsdPreviewSurface");
    surface.inputConnections[TfToken("roughness")] = {
        HdMaterialConnection2{geomPropPath, TfToken("out")}};
    network.nodes[surfacePath] = surface;
    network.terminals[TfToken("surface")] =
        HdMaterialConnection2{surfacePath, TfToken("out")};
    return network;
}

HdMaterialNetwork2
_MakeCurveEmissiveGeomPropMaterialNetwork(TfToken const& geomPropName)
{
    HdMaterialNetwork2 network;
    SdfPath const geomPropPath("/Material/GeomProp");
    HdMaterialNode2 geomProp;
    geomProp.nodeTypeId = TfToken("ND_geompropvalue_float");
    geomProp.parameters[TfToken("geomprop")] =
        VtValue(geomPropName.GetString());
    geomProp.parameters[TfToken("default")] = VtValue(0.05f);
    network.nodes[geomPropPath] = geomProp;

    SdfPath const convertPath("/Material/Convert");
    HdMaterialNode2 convert;
    convert.nodeTypeId = TfToken("ND_convert_float_color3");
    convert.inputConnections[TfToken("in")] = {
        HdMaterialConnection2{geomPropPath, TfToken("out")}};
    network.nodes[convertPath] = convert;

    SdfPath const surfacePath("/Material/Surface");
    HdMaterialNode2 surface;
    surface.nodeTypeId = TfToken("UsdPreviewSurface");
    surface.parameters[TfToken("diffuseColor")] =
        VtValue(GfVec3f(0.0f));
    surface.inputConnections[TfToken("emissiveColor")] = {
        HdMaterialConnection2{convertPath, TfToken("out")}};
    network.nodes[surfacePath] = surface;
    network.terminals[TfToken("surface")] =
        HdMaterialConnection2{surfacePath, TfToken("out")};
    return network;
}

class _CurveRprimSceneDelegate final : public HdSceneDelegate
{
public:
    explicit _CurveRprimSceneDelegate(HdRenderIndex* renderIndex)
        : HdSceneDelegate(renderIndex, SdfPath::AbsoluteRootPath())
    {
    }

    void SetCurve(SdfPath const& id)
    {
        _curveId = id;
        _points = {
            GfVec3f(-1.0f, 0.0f, 0.0f),
            GfVec3f( 0.0f, 0.5f, 0.0f),
            GfVec3f( 1.0f, 0.0f, 0.0f)};
        _topology = HdBasisCurvesTopology(
            HdTokens->linear, HdTokens->bezier,
            HdTokens->nonperiodic, VtIntArray{3}, VtIntArray());
        dirtyBits = HdChangeTracker::AllDirty;
    }

    HdBasisCurvesTopology GetBasisCurvesTopology(
        SdfPath const& id) override
    {
        ++topologyCalls;
        return id == _curveId ? _topology : HdBasisCurvesTopology();
    }

    HdPrimvarDescriptorVector GetPrimvarDescriptors(
        SdfPath const& id,
        HdInterpolation interpolation) override
    {
        ++primvarDescriptorCalls;
        if (_InstancerState const* const instancer =
                _FindInstancer(id)) {
            if (interpolation == HdInterpolationInstance &&
                !instancer->translations.empty()) {
                return {HdPrimvarDescriptor(
                    HdInstancerTokens->instanceTranslations,
                    interpolation)};
            }
            return {};
        }
        if (id != _curveId) {
            return {};
        }
        HdPrimvarDescriptorVector result;
        if (interpolation == HdInterpolationVertex) {
            result.emplace_back(
                HdTokens->points, interpolation,
                HdPrimvarRoleTokens->point);
        }
        if (_normalsPresent && interpolation == _normalsInterpolation) {
            result.emplace_back(
                HdTokens->normals, interpolation,
                HdPrimvarRoleTokens->normal);
        }
        if (_primvarsNormalsPresent &&
            interpolation == _primvarsNormalsInterpolation) {
            result.emplace_back(
                _primvarsNormalsName, interpolation,
                HdPrimvarRoleTokens->normal);
        }
        if (_customPresent && interpolation == _customInterpolation) {
            result.emplace_back(
                _customName, interpolation, TfToken(), _customIndexed);
        }
        if (interpolation == HdInterpolationConstant) {
            result.emplace_back(
                HdTokens->displayColor, interpolation,
                HdPrimvarRoleTokens->color);
            result.emplace_back(HdTokens->displayOpacity, interpolation);
            if (_widthsPresent) {
                result.emplace_back(HdTokens->widths, interpolation);
            }
            if (_primvarsWidthsPresent) {
                result.emplace_back(_primvarsWidthsName, interpolation);
            }
        }
        return result;
    }

    HdExtComputationPrimvarDescriptorVector
    GetExtComputationPrimvarDescriptors(
        SdfPath const& id,
        HdInterpolation interpolation) override
    {
        ++computedDescriptorCalls;
        if (!_computedPresent || id != _curveId ||
            interpolation != _computedInterpolation) {
            return {};
        }
        return {HdExtComputationPrimvarDescriptor(
            _computedName, interpolation, HdPrimvarRoleTokens->none,
            _computationId, _computedOutputName,
            _computedValueType)};
    }

    TfTokenVector GetExtComputationSceneInputNames(
        SdfPath const&) override
    {
        return {};
    }

    HdExtComputationInputDescriptorVector
    GetExtComputationInputDescriptors(SdfPath const&) override
    {
        return {};
    }

    HdExtComputationOutputDescriptorVector
    GetExtComputationOutputDescriptors(
        SdfPath const& computationId) override
    {
        if (computationId != _computationId) {
            return {};
        }
        return {HdExtComputationOutputDescriptor(
            _computedOutputName, _computedValueType)};
    }

    void InvokeExtComputation(
        SdfPath const& computationId,
        HdExtComputationContext* computationContext) override
    {
        if (computationId == _computationId && computationContext) {
            ++computationInvocations;
            computationContext->SetOutputValue(
                _computedOutputName, _computedValue);
        }
    }

    VtValue Get(SdfPath const& id, TfToken const& key) override
    {
        ++valueCalls;
        if (_InstancerState const* const instancer =
                _FindInstancer(id)) {
            if (key == HdInstancerTokens->instanceTranslations) {
                return VtValue(instancer->translations);
            }
            return VtValue();
        }
        if (id != _curveId) {
            return VtValue();
        }
        if (key == HdTokens->points) {
            return VtValue(_points);
        }
        if (key == HdTokens->displayColor) {
            return VtValue(
                VtVec3fArray{GfVec3f(0.2f, 0.4f, 0.6f)});
        }
        if (key == HdTokens->displayOpacity) {
            return VtValue(VtFloatArray{1.0f});
        }
        if (key == HdTokens->widths && _widthsPresent) {
            return VtValue(_widths);
        }
        if (key == _primvarsWidthsName && _primvarsWidthsPresent) {
            ++primvarsWidthsValueCalls;
            return VtValue(_primvarsWidths);
        }
        if (key == HdTokens->normals && _normalsPresent) {
            ++normalsValueCalls;
            return VtValue(_normals);
        }
        if (key == _primvarsNormalsName && _primvarsNormalsPresent) {
            ++primvarsNormalsValueCalls;
            return VtValue(_primvarsNormals);
        }
        if (key == _customName && _customPresent) {
            ++customValueCalls;
            return VtValue(_customValues);
        }
        return VtValue();
    }

    VtValue GetIndexedPrimvar(
        SdfPath const& id,
        TfToken const& key,
        VtIntArray* indices) override
    {
        ++indexedValueCalls;
        if (indices) {
            *indices = key == _customName
                ? _customIndices
                : VtIntArray();
        }
        if (id == _curveId && key == _customName && _customPresent) {
            ++customIndexedValueCalls;
            return VtValue(_customValues);
        }
        return Get(id, key);
    }

    GfMatrix4d GetTransform(SdfPath const& id) override
    {
        return id == _curveId
            ? GfMatrix4d(_transform)
            : GfMatrix4d(1.0);
    }

    bool GetVisible(SdfPath const& id) override
    {
        if (_InstancerState const* const instancer =
                _FindInstancer(id)) {
            return instancer->visible;
        }
        return _visible;
    }

    bool GetDoubleSided(SdfPath const&) override
    {
        return false;
    }

    HdCullStyle GetCullStyle(SdfPath const&) override
    {
        return HdCullStyleDontCare;
    }

    VtArray<TfToken> GetCategories(SdfPath const& id) override
    {
        if (_InstancerState const* const instancer =
                _FindInstancer(id)) {
            return instancer->categories;
        }
        return id == _curveId ? _curveCategories : VtArray<TfToken>();
    }

    std::vector<VtArray<TfToken>> GetInstanceCategories(
        SdfPath const& instancerId) override
    {
        _InstancerState const* const instancer =
            _FindInstancer(instancerId);
        return instancer
            ? instancer->instanceCategories
            : std::vector<VtArray<TfToken>>();
    }

    SdfPath GetMaterialId(SdfPath const& id) override
    {
        return id == _curveId ? _materialId : SdfPath();
    }

    VtValue GetMaterialResource(SdfPath const& materialId) override
    {
        return materialId == _materialId
            ? _materialResource
            : VtValue();
    }

    SdfPath GetInstancerId(SdfPath const& primId) override
    {
        if (primId == _curveId) {
            return _curveInstancerId;
        }
        if (_InstancerState const* const instancer =
                _FindInstancer(primId)) {
            return instancer->parentId;
        }
        return SdfPath();
    }

    VtIntArray GetInstanceIndices(
        SdfPath const& instancerId,
        SdfPath const& prototypeId) override
    {
        _InstancerState const* const instancer =
            _FindInstancer(instancerId);
        return instancer && instancer->prototypeId == prototypeId
            ? instancer->indices
            : VtIntArray();
    }

    GfMatrix4d GetInstancerTransform(
        SdfPath const& instancerId) override
    {
        _InstancerState const* const instancer =
            _FindInstancer(instancerId);
        return instancer ? instancer->transform : GfMatrix4d(1.0);
    }

    SdfPathVector GetInstancerPrototypes(
        SdfPath const& instancerId) override
    {
        _InstancerState const* const instancer =
            _FindInstancer(instancerId);
        return instancer
            ? SdfPathVector{instancer->prototypeId}
            : SdfPathVector();
    }

    void AddInstancer(
        SdfPath const& id,
        SdfPath const& parentId,
        SdfPath const& prototypeId)
    {
        _InstancerState state;
        state.id = id;
        state.parentId = parentId;
        state.prototypeId = prototypeId;
        _instancers.push_back(std::move(state));
        GetRenderIndex().InsertInstancer(this, id);
    }

    void SetCurveInstancer(SdfPath const& instancerId)
    {
        _curveInstancerId = instancerId;
        dirtyBits |= HdChangeTracker::DirtyInstancer;
    }

    void SetCurveCategories(VtArray<TfToken> const& categories)
    {
        _curveCategories = categories;
        dirtyBits |= HdChangeTracker::DirtyCategories;
    }

    void SetMaterial(
        SdfPath const& materialId,
        VtValue const& materialResource)
    {
        _materialId = materialId;
        _materialResource = materialResource;
        dirtyBits |= HdChangeTracker::DirtyMaterialId;
    }

    void SetMaterialResource(VtValue const& materialResource)
    {
        _materialResource = materialResource;
    }

    void SetInstancerTransform(
        SdfPath const& id,
        GfMatrix4d const& transform)
    {
        if (_InstancerState* const instancer = _FindInstancer(id)) {
            instancer->transform = transform;
        }
    }

    void SetInstancerInstances(
        SdfPath const& id,
        VtIntArray const& indices,
        VtVec3fArray const& translations)
    {
        if (_InstancerState* const instancer = _FindInstancer(id)) {
            instancer->indices = indices;
            instancer->translations = translations;
        }
    }

    void SetInstancerCategories(
        SdfPath const& id,
        VtArray<TfToken> const& categories,
        std::vector<VtArray<TfToken>> const& instanceCategories)
    {
        if (_InstancerState* const instancer = _FindInstancer(id)) {
            instancer->categories = categories;
            instancer->instanceCategories = instanceCategories;
        }
    }

    void MarkInstancerDirty(SdfPath const& id, HdDirtyBits bits)
    {
        GetRenderIndex().GetChangeTracker().MarkInstancerDirty(id, bits);
    }

    void SetCurvePoints(
        SdfPath const& id,
        VtVec3fArray const& points)
    {
        _curveId = id;
        _points = points;
        dirtyBits |= HdChangeTracker::DirtyPoints;
    }

    void SetCurveTopology(HdBasisCurvesTopology const& topology)
    {
        _topology = topology;
        dirtyBits |= HdChangeTracker::DirtyTopology;
    }

    void SetCurveTransform(
        SdfPath const& id,
        GfMatrix4f const& transform)
    {
        _curveId = id;
        _transform = transform;
        dirtyBits |= HdChangeTracker::DirtyTransform;
    }

    void SetVisible(bool visible)
    {
        _visible = visible;
        dirtyBits |= HdChangeTracker::DirtyVisibility;
    }

    void SetWidths(VtFloatArray const& widths)
    {
        _widthsPresent = true;
        _widths = widths;
        dirtyBits |= HdChangeTracker::DirtyWidths;
    }

    void RemoveWidths()
    {
        _widthsPresent = false;
        _widths.clear();
        dirtyBits |= HdChangeTracker::DirtyWidths;
    }

    void SetPrimvarsWidths(VtFloatArray const& widths)
    {
        _primvarsWidthsPresent = true;
        _primvarsWidths = widths;
        dirtyBits |= HdChangeTracker::DirtyWidths;
    }

    void RemovePrimvarsWidths()
    {
        _primvarsWidthsPresent = false;
        _primvarsWidths.clear();
        dirtyBits |= HdChangeTracker::DirtyWidths;
    }

    void SetNormals(
        VtVec3fArray const& normals,
        HdInterpolation interpolation = HdInterpolationVertex)
    {
        _normalsPresent = true;
        _normals = normals;
        _normalsInterpolation = interpolation;
        dirtyBits |= HdChangeTracker::DirtyNormals;
    }

    void SetPrimvarsNormals(
        VtVec3fArray const& normals,
        HdInterpolation interpolation = HdInterpolationVertex)
    {
        _primvarsNormalsPresent = true;
        _primvarsNormals = normals;
        _primvarsNormalsInterpolation = interpolation;
        dirtyBits |= HdChangeTracker::DirtyNormals;
    }

    void RemovePrimvarsNormals()
    {
        _primvarsNormalsPresent = false;
        _primvarsNormals.clear();
        dirtyBits |= HdChangeTracker::DirtyNormals;
    }

    void SetCustomPrimvar(
        VtFloatArray const& values,
        VtIntArray const& indices,
        bool indexed,
        HdInterpolation interpolation = HdInterpolationVarying)
    {
        _customPresent = true;
        _customValues = values;
        _customIndices = indices;
        _customIndexed = indexed;
        _customInterpolation = interpolation;
        dirtyBits |= HdChangeTracker::DirtyPrimvar;
    }

    void SetCustomIndices(VtIntArray const& indices)
    {
        _customIndices = indices;
        dirtyBits |= HdChangeTracker::DirtyPrimvar;
    }

    void RemoveCustomPrimvar()
    {
        _customPresent = false;
        _customValues.clear();
        _customIndices.clear();
        dirtyBits |= HdChangeTracker::DirtyPrimvar;
    }

    void SetComputedPrimvar(
        TfToken const& name,
        HdInterpolation interpolation,
        VtValue const& value,
        HdDirtyBits dirtyBit)
    {
        _computedPresent = true;
        _computedName = name;
        _computedInterpolation = interpolation;
        _computedValue = value;
        _computedValueType = value.IsHolding<VtVec3fArray>()
            ? HdTupleType{HdTypeFloatVec3, 1}
            : HdTupleType{HdTypeFloat, 1};
        dirtyBits |= dirtyBit;
    }

    void RemoveComputedPrimvar()
    {
        _computedPresent = false;
        _computedValue = VtValue();
        dirtyBits |= HdChangeTracker::DirtyComputationPrimvarDesc;
    }

    SdfPath const& GetComputationId() const
    {
        return _computationId;
    }

    void ResetPullCounts()
    {
        topologyCalls = 0;
        primvarDescriptorCalls = 0;
        computedDescriptorCalls = 0;
        valueCalls = 0;
        indexedValueCalls = 0;
        customValueCalls = 0;
        customIndexedValueCalls = 0;
        primvarsWidthsValueCalls = 0;
        normalsValueCalls = 0;
        primvarsNormalsValueCalls = 0;
        computationInvocations = 0;
    }

    void MarkDirty(HdDirtyBits bits)
    {
        dirtyBits |= bits;
    }

    HdDirtyBits dirtyBits = HdChangeTracker::Clean;
    size_t topologyCalls = 0;
    size_t primvarDescriptorCalls = 0;
    size_t computedDescriptorCalls = 0;
    size_t valueCalls = 0;
    size_t indexedValueCalls = 0;
    size_t customValueCalls = 0;
    size_t customIndexedValueCalls = 0;
    size_t primvarsWidthsValueCalls = 0;
    size_t normalsValueCalls = 0;
    size_t primvarsNormalsValueCalls = 0;
    size_t computationInvocations = 0;

private:
    struct _InstancerState
    {
        SdfPath id;
        SdfPath parentId;
        SdfPath prototypeId;
        GfMatrix4d transform{1.0};
        VtIntArray indices;
        VtVec3fArray translations;
        VtArray<TfToken> categories;
        std::vector<VtArray<TfToken>> instanceCategories;
        bool visible = true;
    };

    _InstancerState* _FindInstancer(SdfPath const& id)
    {
        std::vector<_InstancerState>::iterator const it = std::find_if(
            _instancers.begin(), _instancers.end(),
            [&id](_InstancerState const& state) {
                return state.id == id;
            });
        return it == _instancers.end() ? nullptr : &*it;
    }

    _InstancerState const* _FindInstancer(SdfPath const& id) const
    {
        std::vector<_InstancerState>::const_iterator const it =
            std::find_if(
                _instancers.begin(), _instancers.end(),
                [&id](_InstancerState const& state) {
                    return state.id == id;
                });
        return it == _instancers.end() ? nullptr : &*it;
    }

    SdfPath _curveId;
    HdBasisCurvesTopology _topology;
    VtVec3fArray _points;
    VtFloatArray _widths{0.2f};
    TfToken const _primvarsWidthsName{"primvars:widths"};
    VtFloatArray _primvarsWidths;
    TfToken const _primvarsNormalsName{"primvars:normals"};
    VtVec3fArray _normals;
    VtVec3fArray _primvarsNormals;
    TfToken const _customName{"custom"};
    VtFloatArray _customValues;
    VtIntArray _customIndices;
    SdfPath const _computationId{"/curveComputation"};
    TfToken const _computedOutputName{"output"};
    TfToken _computedName;
    HdInterpolation _computedInterpolation = HdInterpolationConstant;
    VtValue _computedValue;
    HdTupleType _computedValueType{HdTypeFloat, 1};
    GfMatrix4f _transform{1.0f};
    SdfPath _curveInstancerId;
    SdfPath _materialId;
    VtValue _materialResource;
    VtArray<TfToken> _curveCategories;
    std::vector<_InstancerState> _instancers;
    bool _widthsPresent = true;
    bool _customPresent = false;
    bool _customIndexed = false;
    HdInterpolation _customInterpolation = HdInterpolationVarying;
    bool _computedPresent = false;
    bool _primvarsWidthsPresent = false;
    bool _normalsPresent = false;
    bool _primvarsNormalsPresent = false;
    HdInterpolation _normalsInterpolation = HdInterpolationVertex;
    HdInterpolation _primvarsNormalsInterpolation = HdInterpolationVertex;
    bool _visible = true;
};

void
_AddRprimTestCurve(
    _CurveRprimSceneDelegate* sceneDelegate,
    SdfPath const& id)
{
    sceneDelegate->SetCurve(id);
}

void
_SyncCurveRprim(
    HdEmbreeBasisCurves* curve,
    _CurveRprimSceneDelegate* sceneDelegate,
    HdRenderParam* renderParam,
    TfToken const& reprToken = HdReprTokens->smoothHull,
    bool propagateDirtyBits = true)
{
    HdDirtyBits dirtyBits = sceneDelegate->dirtyBits;
    if ((dirtyBits &
            (HdChangeTracker::InitRepr |
             HdChangeTracker::DirtyRepr)) != 0) {
        curve->UpdateReprSelector(sceneDelegate, &dirtyBits);
        curve->InitRepr(sceneDelegate, reprToken, &dirtyBits);
        dirtyBits &= ~HdChangeTracker::InitRepr;
    }
    if (propagateDirtyBits) {
        dirtyBits = curve->PropagateRprimDirtyBits(dirtyBits);
    }
    curve->Sync(sceneDelegate, renderParam, &dirtyBits, reprToken);
    sceneDelegate->dirtyBits = HdChangeTracker::Clean;
}

class _RetainedCurveComputation final
    : public HdExtComputationCpuCallback
{
public:
    _RetainedCurveComputation(
        TfToken const& outputName,
        VtValue const& outputValue)
        : _outputName(outputName)
        , _outputValue(outputValue)
    {
    }

    void Compute(HdExtComputationContext* context) override
    {
        if (context) {
            context->SetOutputValue(_outputName, _outputValue);
        }
    }

private:
    TfToken const _outputName;
    VtValue const _outputValue;
};

HdContainerDataSourceHandle
_BuildRetainedCurvePrimvar(
    HdSampledDataSourceHandle const& value,
    TfToken const& interpolation,
    TfToken const& role = TfToken())
{
    return HdPrimvarSchema::Builder()
        .SetPrimvarValue(value)
        .SetInterpolation(
            HdPrimvarSchema::BuildInterpolationDataSource(interpolation))
        .SetRole(HdPrimvarSchema::BuildRoleDataSource(role))
        .Build();
}

HdContainerDataSourceHandle
_BuildRetainedIndexedCurvePrimvar(
    HdSampledDataSourceHandle const& value,
    VtIntArray const& indices,
    TfToken const& interpolation)
{
    return HdPrimvarSchema::Builder()
        .SetIndexedPrimvarValue(value)
        .SetIndices(
            HdRetainedTypedSampledDataSource<VtIntArray>::New(indices))
        .SetInterpolation(
            HdPrimvarSchema::BuildInterpolationDataSource(interpolation))
        .Build();
}

HdContainerDataSourceHandle
_BuildRetainedCurveDataContractPrim(
    VtVec3fArray const& points,
    VtIntArray const& curveIndices,
    bool includeComputed,
    TfToken const& computedName,
    SdfPath const& computationId,
    TfToken const& computationOutputName)
{
    HdContainerDataSourceHandle const topology =
        HdBasisCurvesTopologySchema::Builder()
            .SetCurveVertexCounts(
                HdRetainedTypedSampledDataSource<VtIntArray>::New(
                    VtIntArray{4, 4, 4}))
            .SetCurveIndices(
                HdRetainedTypedSampledDataSource<VtIntArray>::New(
                    curveIndices))
            .SetBasis(
                HdRetainedTypedSampledDataSource<TfToken>::New(
                    HdTokens->bspline))
            .SetType(
                HdRetainedTypedSampledDataSource<TfToken>::New(
                    HdTokens->cubic))
            .SetWrap(
                HdRetainedTypedSampledDataSource<TfToken>::New(
                    HdTokens->pinned))
            .Build();

    TfToken const primvarNames[] = {
        HdPrimvarsSchemaTokens->points,
        HdPrimvarsSchemaTokens->widths,
        TfToken("custom")};
    HdDataSourceBaseHandle const primvarValues[] = {
        _BuildRetainedCurvePrimvar(
            HdRetainedTypedSampledDataSource<VtVec3fArray>::New(points),
            HdPrimvarSchemaTokens->vertex,
            HdPrimvarSchemaTokens->point),
        _BuildRetainedCurvePrimvar(
            HdRetainedTypedSampledDataSource<VtFloatArray>::New(
                VtFloatArray{0.4f}),
            HdPrimvarSchemaTokens->constant),
        _BuildRetainedIndexedCurvePrimvar(
            HdRetainedTypedSampledDataSource<VtFloatArray>::New(
                VtFloatArray{30.0f, 10.0f, 20.0f}),
            VtIntArray{1, 2, 0},
            HdPrimvarSchemaTokens->uniform)};
    HdContainerDataSourceHandle const primvars =
        HdPrimvarsSchema::BuildRetained(
            std::size(primvarNames), primvarNames, primvarValues);

    HdContainerDataSourceHandle computedPrimvars;
    if (includeComputed) {
        HdDataSourceBaseHandle const computedValue =
            HdExtComputationPrimvarSchema::Builder()
                .SetInterpolation(
                    HdExtComputationPrimvarSchema::
                        BuildInterpolationDataSource(
                            HdPrimvarSchemaTokens->uniform))
                .SetSourceComputation(
                    HdRetainedTypedSampledDataSource<SdfPath>::New(
                        computationId))
                .SetSourceComputationOutputName(
                    HdRetainedTypedSampledDataSource<TfToken>::New(
                        computationOutputName))
                .SetValueType(
                    HdRetainedTypedSampledDataSource<HdTupleType>::New(
                        HdTupleType{HdTypeFloat, 1}))
                .Build();
        computedPrimvars =
            HdExtComputationPrimvarsSchema::BuildRetained(
                1, &computedName, &computedValue);
    }

    TfToken const names[] = {
        HdBasisCurvesSchema::GetSchemaToken(),
        HdPrimvarsSchema::GetSchemaToken(),
        HdExtComputationPrimvarsSchema::GetSchemaToken()};
    HdDataSourceBaseHandle const values[] = {
        HdBasisCurvesSchema::Builder()
            .SetTopology(topology)
            .Build(),
        primvars,
        computedPrimvars};
    return HdRetainedContainerDataSource::New(
        std::size(names), names, values);
}

HdContainerDataSourceHandle
_BuildRetainedCurveComputationPrim(
    TfToken const& outputName,
    VtFloatArray const& values)
{
    HdDataSourceBaseHandle const output =
        HdExtComputationOutputSchema::Builder()
            .SetValueType(
                HdRetainedTypedSampledDataSource<HdTupleType>::New(
                    HdTupleType{HdTypeFloat, 1}))
            .Build();
    HdContainerDataSourceHandle const outputs =
        HdExtComputationOutputContainerSchema::BuildRetained(
            1, &outputName, &output);
    HdExtComputationCpuCallbackSharedPtr const callback =
        std::make_shared<_RetainedCurveComputation>(
            outputName, VtValue(values));
    return HdRetainedContainerDataSource::New(
        HdExtComputationSchema::GetSchemaToken(),
        HdExtComputationSchema::Builder()
            .SetOutputs(outputs)
            .SetCpuCallback(
                HdRetainedTypedSampledDataSource<
                    HdExtComputationCpuCallbackSharedPtr>::New(callback))
            .Build());
}

HdContainerDataSourceHandle
_BuildInvisibleCurveSubset(
    TfToken const& type,
    VtIntArray const& indices)
{
    return HdRetainedContainerDataSource::New(
        HdGeomSubsetSchema::GetSchemaToken(),
        HdGeomSubsetSchema::Builder()
            .SetType(HdGeomSubsetSchema::BuildTypeDataSource(type))
            .SetIndices(
                HdRetainedTypedSampledDataSource<VtIntArray>::New(indices))
            .Build(),
        HdVisibilitySchema::GetSchemaToken(),
        HdVisibilitySchema::Builder()
            .SetVisibility(
                HdRetainedTypedSampledDataSource<bool>::New(false))
            .Build());
}

void
_SyncSceneIndexCurve(
    HdEmbreeBasisCurves* curve,
    HdSceneDelegate* sceneDelegate,
    HdRenderParam* renderParam)
{
    HdDirtyBits dirtyBits = curve->GetInitialDirtyBitsMask();
    curve->UpdateReprSelector(sceneDelegate, &dirtyBits);
    curve->InitRepr(sceneDelegate, HdReprTokens->smoothHull, &dirtyBits);
    dirtyBits &= ~HdChangeTracker::InitRepr;
    dirtyBits = curve->PropagateRprimDirtyBits(dirtyBits);
    curve->Sync(
        sceneDelegate, renderParam, &dirtyBits, HdReprTokens->smoothHull);
}

struct _CurveDataContractSample
{
    ty::CurveSegmentMetadata metadata;
    float custom = 0.0f;
    float computed = 0.0f;
};

bool
_CollectCurveDataContractSamples(
    HdEmbreeBasisCurves const& curve,
    TfToken const& computedName,
    std::vector<_CurveDataContractSample>* samples)
{
    if (!samples) {
        return false;
    }
    samples->clear();
    for (size_t recordIndex = 0;
         recordIndex < curve.GetCurveGeometryRecordCount();
         ++recordIndex) {
        ty::PrototypeContext const* const context =
            curve.GetPrototypeContext(recordIndex);
        if (!context) {
            return false;
        }
        auto const customIt = context->primvarMap.find(TfToken("custom"));
        auto const computedIt = context->primvarMap.find(computedName);
        if (customIt == context->primvarMap.end() ||
            computedIt == context->primvarMap.end()) {
            return false;
        }
        for (size_t primitiveIndex = 0;
             primitiveIndex < context->curvePrimitiveMetadata.size();
             ++primitiveIndex) {
            _CurveDataContractSample sample;
            sample.metadata =
                context->curvePrimitiveMetadata[primitiveIndex];
            if (!customIt->second->Sample(
                    static_cast<unsigned int>(primitiveIndex),
                    0.5f, 0.0f, &sample.custom) ||
                !computedIt->second->Sample(
                    static_cast<unsigned int>(primitiveIndex),
                    0.5f, 0.0f, &sample.computed)) {
                return false;
            }
            samples->push_back(sample);
        }
    }
    std::sort(
        samples->begin(), samples->end(),
        [](_CurveDataContractSample const& lhs,
           _CurveDataContractSample const& rhs) {
            if (lhs.metadata.authoredCurveId !=
                rhs.metadata.authoredCurveId) {
                return lhs.metadata.authoredCurveId <
                    rhs.metadata.authoredCurveId;
            }
            if (lhs.metadata.authoredSegmentId !=
                rhs.metadata.authoredSegmentId) {
                return lhs.metadata.authoredSegmentId <
                    rhs.metadata.authoredSegmentId;
            }
            return lhs.metadata.authoredU0 < rhs.metadata.authoredU0;
        });
    return true;
}

bool
_CurveDataContractSamplesEqual(
    std::vector<_CurveDataContractSample> const& first,
    std::vector<_CurveDataContractSample> const& second)
{
    if (first.size() != second.size()) {
        return false;
    }
    for (size_t index = 0; index < first.size(); ++index) {
        _CurveDataContractSample const& lhs = first[index];
        _CurveDataContractSample const& rhs = second[index];
        if (lhs.metadata.authoredCurveId !=
                rhs.metadata.authoredCurveId ||
            lhs.metadata.authoredSegmentId !=
                rhs.metadata.authoredSegmentId ||
            !_Close(lhs.metadata.authoredU0, rhs.metadata.authoredU0) ||
            !_Close(lhs.metadata.authoredU1, rhs.metadata.authoredU1) ||
            lhs.metadata.isAuthoredCurveStart !=
                rhs.metadata.isAuthoredCurveStart ||
            lhs.metadata.isAuthoredCurveEnd !=
                rhs.metadata.isAuthoredCurveEnd ||
            !_Close(lhs.custom, rhs.custom) ||
            !_Close(lhs.computed, rhs.computed)) {
            return false;
        }
    }
    return true;
}

bool
_IntersectRprimRoot(
    HdEmbreeRenderDelegate* renderDelegate,
    GfVec3f const& origin)
{
    HdEmbreeRenderParam* const renderParam =
        static_cast<HdEmbreeRenderParam*>(
            renderDelegate->GetRenderParam());
    RTCScene const rootScene = renderParam->AcquireSceneForEdit();
    rtcCommitScene(rootScene);

    RTCRayHit rayHit = {};
    rayHit.ray.org_x = origin[0];
    rayHit.ray.org_y = origin[1];
    rayHit.ray.org_z = origin[2];
    rayHit.ray.dir_x = 0.0f;
    rayHit.ray.dir_y = 0.0f;
    rayHit.ray.dir_z = -1.0f;
    rayHit.ray.tnear = 0.0f;
    rayHit.ray.tfar = 100.0f;
    rayHit.ray.mask = std::numeric_limits<unsigned int>::max();
    rayHit.hit.geomID = RTC_INVALID_GEOMETRY_ID;
    rayHit.hit.primID = RTC_INVALID_GEOMETRY_ID;
    for (unsigned int level = 0;
         level < RTC_MAX_INSTANCE_LEVEL_COUNT;
         ++level) {
        rayHit.hit.instID[level] = RTC_INVALID_GEOMETRY_ID;
    }
    ty::Intersect1(rootScene, &rayHit);
    return rayHit.hit.geomID != RTC_INVALID_GEOMETRY_ID;
}

bool
TestBasisCurvesAdvertisedFactoryContract()
{
    HdEmbreeRenderDelegate renderDelegate;
    TfTokenVector const& supported =
        renderDelegate.GetSupportedRprimTypes();
    if (!_Require(
            std::find(
                supported.begin(), supported.end(),
                HdPrimTypeTokens->basisCurves) != supported.end(),
            "BasisCurves factory", "basisCurves was not advertised")) {
        return false;
    }

    HdRprim* const rprim = renderDelegate.CreateRprim(
        HdPrimTypeTokens->basisCurves, SdfPath("/factoryCurve"));
    HdEmbreeBasisCurves* const curve =
        dynamic_cast<HdEmbreeBasisCurves*>(rprim);
    if (!_Require(curve != nullptr, "BasisCurves factory",
                  "factory did not create HdEmbreeBasisCurves")) {
        renderDelegate.DestroyRprim(rprim);
        return false;
    }

    constexpr HdDirtyBits requiredBits =
        HdChangeTracker::InitRepr |
        HdChangeTracker::DirtyRepr |
        HdChangeTracker::DirtyPoints |
        HdChangeTracker::DirtyTopology |
        HdChangeTracker::DirtyWidths |
        HdChangeTracker::DirtyNormals |
        HdChangeTracker::DirtyPrimvar |
        HdChangeTracker::DirtyComputationPrimvarDesc |
        HdChangeTracker::DirtyMaterialId |
        HdChangeTracker::DirtyTransform |
        HdChangeTracker::DirtyVisibility |
        HdChangeTracker::DirtyCullStyle |
        HdChangeTracker::DirtyDoubleSided |
        HdChangeTracker::DirtyInstancer |
        HdChangeTracker::DirtyInstanceIndex |
        HdChangeTracker::DirtyCategories |
        HdChangeTracker::DirtyPrimID;
    bool const maskIsComplete =
        (curve->GetInitialDirtyBitsMask() & requiredBits) == requiredBits;
    renderDelegate.DestroyRprim(rprim);
    return _Require(maskIsComplete, "BasisCurves factory",
                    "initial dirty mask was incomplete");
}

bool
TestBasisCurvesMinimumWidthSettingRebuild()
{
    _CurveRprimTestContext context;
    if (!context.renderIndex) {
        return false;
    }

    _CurveRprimSceneDelegate sceneDelegate(context.renderIndex.get());
    SdfPath const curveId("/minimumWidthCurve");
    SdfPath const instancerId("/minimumWidthInstancer");
    _AddRprimTestCurve(&sceneDelegate, curveId);
    sceneDelegate.RemoveWidths();
    sceneDelegate.AddInstancer(instancerId, SdfPath(), curveId);
    sceneDelegate.SetInstancerInstances(
        instancerId,
        VtIntArray{0, 1},
        VtVec3fArray{
            GfVec3f(-2.0f, 0.0f, 0.0f),
            GfVec3f( 2.0f, 0.0f, 0.0f)});
    sceneDelegate.SetCurveInstancer(instancerId);

    // A curve created after a non-default direct setting must receive that
    // snapshot before its very first Sync.
    context.renderDelegate.SetRenderSetting(
        HdEmbreeRenderSettingsTokens->minCurveWidth, VtValue(0.03f));
    std::unique_ptr<HdRprim, _FactoryRprimDeleter> curveOwner(
        context.renderDelegate.CreateRprim(
            HdPrimTypeTokens->basisCurves, curveId),
        _FactoryRprimDeleter{&context.renderDelegate});
    HdEmbreeBasisCurves* curve =
        dynamic_cast<HdEmbreeBasisCurves*>(curveOwner.get());
    if (!_Require(curve != nullptr, "BasisCurves minimum setting",
                  "factory curve was not created")) {
        return false;
    }

    sceneDelegate.dirtyBits = curve->GetInitialDirtyBitsMask();
    TfDiagnosticTrap initialTrap;
    _SyncCurveRprim(
        curve, &sceneDelegate, context.renderDelegate.GetRenderParam());
    const bool initialSyncHasNoErrors = initialTrap.GetErrors().empty();
    initialTrap.Clear();
    if (!_Require(initialSyncHasNoErrors,
                  "BasisCurves minimum setting",
                  "first Sync rejected the injected minimum") ||
        !_Require(
            curve->GetCurveGeometryRecordCount() == 1 &&
                curve->GetInstanceCount() == 2 &&
                _Close(curve->GetCurveGeometryRadius(0, 0), 0.015f),
            "BasisCurves minimum setting",
            "first Sync did not use the current object-space diameter")) {
        return false;
    }

    const std::uint64_t initialGeneration =
        curve->GetGeometryGeneration();
    ty::InstanceContext const* const initialInstance =
        curve->GetInstanceContext(0);
    sceneDelegate.ResetPullCounts();

    // RenderPass calls this boundary after Hydra Sync. It must rebuild the
    // prototype synchronously from cached input and only recommit instances.
    context.renderDelegate.SetRenderSetting(
        HdEmbreeRenderSettingsTokens->minCurveWidth, VtValue(0.05f));
    const float synchronizedMinimum =
        context.renderDelegate.SynchronizeBasisCurvesMinimumWidth();
    const bool sceneDelegateWasNotPulled =
        sceneDelegate.topologyCalls == 0 &&
        sceneDelegate.primvarDescriptorCalls == 0 &&
        sceneDelegate.computedDescriptorCalls == 0 &&
        sceneDelegate.valueCalls == 0 &&
        sceneDelegate.indexedValueCalls == 0 &&
        sceneDelegate.computationInvocations == 0;
    if (!_Require(
            synchronizedMinimum == 0.05f &&
                curve->GetGeometryGeneration() == initialGeneration + 1 &&
                curve->GetInstanceCount() == 2 &&
                curve->GetInstanceContext(0) == initialInstance &&
                _Close(curve->GetCurveGeometryRadius(0, 0), 0.025f),
            "BasisCurves minimum setting",
            "same-frame cached rebuild or instance recommit failed") ||
        !_Require(sceneDelegateWasNotPulled,
                  "BasisCurves minimum setting",
                  "setting rebuild pulled the SceneDelegate")) {
        return false;
    }

    const unsigned int changedSettingsVersion =
        context.renderDelegate.GetRenderSettingsVersion();
    const std::uint64_t changedGeneration =
        curve->GetGeometryGeneration();
    context.renderDelegate.SetRenderSetting(
        HdEmbreeRenderSettingsTokens->minCurveWidth, VtValue(0.05f));
    context.renderDelegate.SynchronizeBasisCurvesMinimumWidth();
    if (!_Require(
            context.renderDelegate.GetRenderSettingsVersion() ==
                    changedSettingsVersion &&
                curve->GetGeometryGeneration() == changedGeneration,
            "BasisCurves minimum setting",
            "unchanged effective minimum rebuilt geometry")) {
        return false;
    }

    // Remove the registered curve, then update the setting. A later curve
    // must start with the new snapshot; the old pointer must not be visited.
    curveOwner.reset();
    context.renderDelegate.SetRenderSetting(
        HdEmbreeRenderSettingsTokens->minCurveWidth, VtValue(0.04f));
    if (!_Require(
            context.renderDelegate.SynchronizeBasisCurvesMinimumWidth() ==
                0.04f,
            "BasisCurves minimum setting",
            "setting update after curve destruction failed")) {
        return false;
    }

    _AddRprimTestCurve(&sceneDelegate, curveId);
    sceneDelegate.RemoveWidths();
    curveOwner.reset(context.renderDelegate.CreateRprim(
        HdPrimTypeTokens->basisCurves, curveId));
    curve = dynamic_cast<HdEmbreeBasisCurves*>(curveOwner.get());
    if (!curve) {
        return false;
    }
    sceneDelegate.dirtyBits = curve->GetInitialDirtyBitsMask();
    TfDiagnosticTrap recreatedTrap;
    _SyncCurveRprim(
        curve, &sceneDelegate, context.renderDelegate.GetRenderParam());
    const bool recreatedHasNoErrors = recreatedTrap.GetErrors().empty();
    recreatedTrap.Clear();
    return _Require(
        recreatedHasNoErrors &&
            _Close(curve->GetCurveGeometryRadius(0, 0), 0.02f),
        "BasisCurves minimum setting",
        "recreated curve did not inherit the current minimum");
}

bool
TestBasisCurvesRprimLifecycleAndRecovery()
{
    _CurveRprimTestContext context;
    if (!context.renderIndex) {
        return false;
    }
    _CurveRprimSceneDelegate sceneDelegate(context.renderIndex.get());
    SdfPath const id("/rprimCurve");
    _AddRprimTestCurve(&sceneDelegate, id);

    std::unique_ptr<HdEmbreeBasisCurves> curveOwner =
        std::make_unique<HdEmbreeBasisCurves>(id);
    HdEmbreeBasisCurves* const curve = curveOwner.get();
    sceneDelegate.dirtyBits = curve->GetInitialDirtyBitsMask();

    TfDiagnosticTrap missingMinimumTrap;
    _SyncCurveRprim(
        curve, &sceneDelegate, context.renderDelegate.GetRenderParam());
    std::vector<TfError> const missingMinimumErrors =
        missingMinimumTrap.GetErrors();
    bool const missingMinimumIsActionable =
        missingMinimumErrors.size() == 1 &&
        missingMinimumErrors[0].GetCommentary().find(id.GetString()) !=
            std::string::npos &&
        missingMinimumErrors[0].GetCommentary().find(
            "cause=notInjected") != std::string::npos &&
        missingMinimumErrors[0].GetCommentary().find(
            "fallback=emptyPrototype") != std::string::npos;
    missingMinimumTrap.Clear();
    if (!_Require(missingMinimumIsActionable, "BasisCurves lifecycle",
                  "missing minimum-width diagnostic was not actionable") ||
        !_Require(curve->GetCurveGeometryRecordCount() == 0,
                  "BasisCurves lifecycle",
                  "missing minimum width published geometry") ||
        !_Require(curve->GetInstanceCount() == 1,
                  "BasisCurves lifecycle",
                  "empty prototype did not retain its identity instance")) {
        return false;
    }

    TfDiagnosticTrap cleanRetryTrap;
    _SyncCurveRprim(
        curve, &sceneDelegate, context.renderDelegate.GetRenderParam());
    bool const cleanRetryStayedQuiet =
        cleanRetryTrap.GetErrors().empty() &&
        cleanRetryTrap.GetWarnings().empty();
    cleanRetryTrap.Clear();
    if (!_Require(cleanRetryStayedQuiet, "BasisCurves lifecycle",
                  "clean Sync repeated a cached validation diagnostic")) {
        return false;
    }

    curve->SetMinimumWidth(0.01f, 1);
    sceneDelegate.MarkDirty(HdChangeTracker::DirtyWidths);
    TfDiagnosticTrap validTrap;
    _SyncCurveRprim(
        curve, &sceneDelegate, context.renderDelegate.GetRenderParam());
    bool const validSyncHasNoErrors = validTrap.GetErrors().empty();
    validTrap.Clear();
    ty::PrototypeContext const* const initialPrototype =
        curve->GetPrototypeContext(0);
    if (!_Require(validSyncHasNoErrors, "BasisCurves lifecycle",
                  "valid Sync emitted an error") ||
        !_Require(curve->GetCurveGeometryRecordCount() == 1,
                  "BasisCurves lifecycle",
                  "valid linear curve did not publish one record") ||
        !_Require(initialPrototype != nullptr,
                  "BasisCurves lifecycle", "prototype context was absent") ||
        !_Require(initialPrototype->curvePrimitiveMetadata.size() == 2,
                  "BasisCurves lifecycle",
                  "authored segment metadata was not retained")) {
        return false;
    }

    std::uint64_t const initialGeneration = curve->GetGeometryGeneration();
    GfVec3f const probeOrigin(-0.5f, 0.25f, 2.0f);
    if (!_Require(
            _IntersectRprimRoot(&context.renderDelegate, probeOrigin),
            "BasisCurves lifecycle",
            "identity instance did not publish intersectable geometry")) {
        return false;
    }

    sceneDelegate.SetVisible(false);
    _SyncCurveRprim(
        curve, &sceneDelegate, context.renderDelegate.GetRenderParam());
    if (!_Require(
            !_IntersectRprimRoot(&context.renderDelegate, probeOrigin) &&
                curve->GetGeometryGeneration() == initialGeneration &&
                curve->GetPrototypeContext(0) == initialPrototype,
            "BasisCurves lifecycle",
            "visibility did not update only the identity-instance mask")) {
        return false;
    }
    sceneDelegate.SetVisible(true);
    _SyncCurveRprim(
        curve, &sceneDelegate, context.renderDelegate.GetRenderParam());
    if (!_Require(
            _IntersectRprimRoot(&context.renderDelegate, probeOrigin),
            "BasisCurves lifecycle",
            "visibility recovery did not restore the identity instance")) {
        return false;
    }

    sceneDelegate.SetCurveTransform(
        id, GfMatrix4f().SetTranslate(GfVec3f(3.0f, 2.0f, 1.0f)));
    _SyncCurveRprim(
        curve, &sceneDelegate, context.renderDelegate.GetRenderParam());
    if (!_Require(
            curve->GetGeometryGeneration() == initialGeneration &&
                curve->GetPrototypeContext(0) == initialPrototype,
            "BasisCurves lifecycle",
            "transform-only Sync replaced prototype state")) {
        return false;
    }

    sceneDelegate.MarkDirty(HdChangeTracker::DirtyRepr);
    _SyncCurveRprim(
        curve, &sceneDelegate, context.renderDelegate.GetRenderParam(),
        HdReprTokens->points);
    if (!_Require(
            curve->GetGeometryGeneration() == initialGeneration &&
                curve->GetPrototypeContext(0) == initialPrototype,
            "BasisCurves lifecycle",
            "points repr did not deterministically reuse surface geometry")) {
        return false;
    }

    sceneDelegate.MarkDirty(HdChangeTracker::DirtyRepr);
    _SyncCurveRprim(
        curve, &sceneDelegate, context.renderDelegate.GetRenderParam(),
        HdReprTokens->wire);
    if (!_Require(
            curve->GetGeometryGeneration() == initialGeneration &&
                curve->GetPrototypeContext(0) == initialPrototype,
            "BasisCurves lifecycle",
            "wire repr did not deterministically reuse surface geometry")) {
        return false;
    }

    curve->SetMinimumWidth(0.0f, 2);
    sceneDelegate.SetWidths(VtFloatArray{0.0f});
    TfDiagnosticTrap zeroWidthTrap;
    _SyncCurveRprim(
        curve, &sceneDelegate, context.renderDelegate.GetRenderParam());
    bool hasZeroWidthWarning = false;
    for (TfWarning const& warning : zeroWidthTrap.GetWarnings()) {
        std::string const& commentary = warning.GetCommentary();
        hasZeroWidthWarning |=
            commentary.find(id.GetString()) != std::string::npos &&
            commentary.find("cause=allEffectiveWidthsZero") !=
                std::string::npos &&
            commentary.find("fallback=emptyPrototype") !=
                std::string::npos;
    }
    zeroWidthTrap.Clear();
    if (!_Require(hasZeroWidthWarning, "BasisCurves lifecycle",
                  "zero-width empty fallback was not reported") ||
        !_Require(curve->GetCurveGeometryRecordCount() == 0,
                  "BasisCurves lifecycle",
                  "all-zero width retained stale geometry")) {
        return false;
    }

    sceneDelegate.SetWidths(VtFloatArray{0.2f});
    _SyncCurveRprim(
        curve, &sceneDelegate, context.renderDelegate.GetRenderParam());
    if (!_Require(curve->GetCurveGeometryRecordCount() == 1,
                  "BasisCurves lifecycle",
                  "nonzero width did not recover the same Rprim")) {
        return false;
    }

    sceneDelegate.SetCurvePoints(
        id, VtVec3fArray{
            GfVec3f(-1.0f, 0.0f, 0.0f),
            GfVec3f( 1.0f, 0.0f, 0.0f)});
    TfDiagnosticTrap badPointsTrap;
    _SyncCurveRprim(
        curve, &sceneDelegate, context.renderDelegate.GetRenderParam());
    bool const badPointsReported =
        badPointsTrap.GetErrors().size() == 1 &&
        badPointsTrap.GetErrors()[0].GetCommentary().find(
            "fallback=emptyPrototype") != std::string::npos;
    badPointsTrap.Clear();
    if (!_Require(badPointsReported, "BasisCurves lifecycle",
                  "invalid points were not reported once") ||
        !_Require(curve->GetCurveGeometryRecordCount() == 0,
                  "BasisCurves lifecycle",
                  "invalid points retained stale geometry")) {
        return false;
    }

    sceneDelegate.MarkDirty(HdChangeTracker::DirtyPoints);
    TfDiagnosticTrap revalidatedBadPointsTrap;
    _SyncCurveRprim(
        curve, &sceneDelegate, context.renderDelegate.GetRenderParam());
    bool const revalidationReportedOnce =
        revalidatedBadPointsTrap.GetErrors().size() == 1;
    revalidatedBadPointsTrap.Clear();
    if (!_Require(
            revalidationReportedOnce,
            "BasisCurves lifecycle",
            "dirty invalid points were not reported once on revalidation")) {
        return false;
    }

    sceneDelegate.SetCurvePoints(
        id,
        VtVec3fArray{
            GfVec3f(-1.0f, 0.0f, 0.0f),
            GfVec3f( 0.0f, 0.5f, 0.0f),
            GfVec3f( 1.0f, 0.0f, 0.0f)});
    _SyncCurveRprim(
        curve, &sceneDelegate, context.renderDelegate.GetRenderParam());
    bool const recovered = _Require(
        curve->GetCurveGeometryRecordCount() == 1,
        "BasisCurves lifecycle",
        "valid points did not recover the same Rprim");
    curve->Finalize(context.renderDelegate.GetRenderParam());
    return recovered;
}

bool
TestBasisCurvesRprimOwnsMultipleRecords()
{
    _CurveRprimTestContext context;
    if (!context.renderIndex) {
        return false;
    }
    _CurveRprimSceneDelegate sceneDelegate(context.renderIndex.get());
    SdfPath const id("/mixedRecordCurve");
    _AddRprimTestCurve(&sceneDelegate, id);
    VtVec3fArray const mixedNormals{
        GfVec3f(0.0f, 0.0f, 1.0f),
        GfVec3f(0.0f, 0.0f, -1.0f),
        GfVec3f(0.0f, 0.0f, -1.0f)};
    sceneDelegate.SetNormals(mixedNormals, HdInterpolationVarying);

    std::unique_ptr<HdEmbreeBasisCurves> curve =
        std::make_unique<HdEmbreeBasisCurves>(id);
    curve->SetMinimumWidth(0.0f, 1);
    sceneDelegate.dirtyBits = curve->GetInitialDirtyBitsMask();
    TfDiagnosticTrap mixedTrap;
    _SyncCurveRprim(
        curve.get(), &sceneDelegate,
        context.renderDelegate.GetRenderParam());
    bool const diagnosticsAggregated = mixedTrap.GetWarnings().size() == 1;
    bool tubeFallbackReported = false;
    for (TfWarning const& warning : mixedTrap.GetWarnings()) {
        std::string const& commentary = warning.GetCommentary();
        tubeFallbackReported |=
            commentary.find("category=normal") != std::string::npos &&
            commentary.find("cause=tubeFallbackSpan") !=
                std::string::npos;
    }
    mixedTrap.Clear();

    size_t metadataCount = 0;
    bool hasTube = false;
    bool hasRibbon = false;
    for (size_t recordIndex = 0;
         recordIndex < curve->GetCurveGeometryRecordCount();
         ++recordIndex) {
        ty::PrototypeContext const* const prototype =
            curve->GetPrototypeContext(recordIndex);
        if (!prototype) {
            return false;
        }
        metadataCount += prototype->curvePrimitiveMetadata.size();
        hasTube |= prototype->geometryKind == ty::GeometryKind::roundCurve;
        hasRibbon |= prototype->geometryKind ==
            ty::GeometryKind::orientedRibbon;
    }
    if (!_Require(
            curve->GetCurveGeometryRecordCount() == 2 &&
                curve->GetInstanceCount() == 1 && hasTube && hasRibbon &&
                metadataCount == 2 && diagnosticsAggregated &&
                tubeFallbackReported,
            "BasisCurves mixed records",
            "one Rprim did not publish its tube/ribbon record mixture")) {
        return false;
    }

    sceneDelegate.SetNormals(
        VtVec3fArray(3, GfVec3f(0.0f, 0.0f, 1.0f)),
        HdInterpolationVarying);
    _SyncCurveRprim(
        curve.get(), &sceneDelegate,
        context.renderDelegate.GetRenderParam());
    ty::PrototypeContext const* const ribbonOnly =
        curve->GetPrototypeContext(0);
    if (!_Require(
            curve->GetCurveGeometryRecordCount() == 1 && ribbonOnly &&
                ribbonOnly->geometryKind ==
                    ty::GeometryKind::orientedRibbon,
            "BasisCurves mixed records",
            "record replacement did not collapse to one ribbon record")) {
        return false;
    }

    sceneDelegate.SetNormals(mixedNormals, HdInterpolationVarying);
    TfDiagnosticTrap recoveredMixedTrap;
    _SyncCurveRprim(
        curve.get(), &sceneDelegate,
        context.renderDelegate.GetRenderParam());
    recoveredMixedTrap.Clear();
    bool const recoveredMixture = _Require(
        curve->GetCurveGeometryRecordCount() == 2,
        "BasisCurves mixed records",
        "record replacement did not recover the mixed representation");
    curve->Finalize(context.renderDelegate.GetRenderParam());
    return recoveredMixture;
}

bool
TestBasisCurvesPointInstancing()
{
    _CurveRprimTestContext context;
    if (!context.renderIndex) {
        return false;
    }
    _CurveRprimSceneDelegate sceneDelegate(context.renderIndex.get());
    SdfPath const curveId("/instancedCurve");
    SdfPath const parentId("/parentInstancer");
    SdfPath const childId("/childInstancer");
    _AddRprimTestCurve(&sceneDelegate, curveId);

    sceneDelegate.AddInstancer(parentId, SdfPath(), childId);
    sceneDelegate.AddInstancer(childId, parentId, curveId);
    VtVec3fArray const parentTranslations{
        GfVec3f(0.0f, 5.0f, 0.0f),
        GfVec3f(0.0f, -5.0f, 0.0f)};
    VtVec3fArray const childTranslations{
        GfVec3f(0.0f, 0.0f, 0.0f),
        GfVec3f(3.0f, 0.0f, 0.0f)};
    sceneDelegate.SetInstancerInstances(
        parentId, VtIntArray{0, 1}, parentTranslations);
    // Start with a valid zero-instance leaf while retaining its two source
    // transforms for the following indices-only update.
    sceneDelegate.SetInstancerInstances(
        childId, VtIntArray(), childTranslations);

    GfMatrix4d childScale(1.0);
    childScale.SetScale(GfVec3d(2.0, 3.0, 4.0));
    sceneDelegate.SetInstancerTransform(childId, childScale);

    TfToken const parentCategory("parentCategory");
    TfToken const parentZeroCategory("parentZeroCategory");
    TfToken const parentOneCategory("parentOneCategory");
    TfToken const childCategory("childCategory");
    TfToken const childZeroCategory("childZeroCategory");
    TfToken const childOneCategory("childOneCategory");
    TfToken const curveCategory("curveCategory");
    sceneDelegate.SetInstancerCategories(
        parentId, VtArray<TfToken>{parentCategory},
        std::vector<VtArray<TfToken>>{
            VtArray<TfToken>{parentZeroCategory},
            VtArray<TfToken>{parentOneCategory}});
    sceneDelegate.SetInstancerCategories(
        childId, VtArray<TfToken>{childCategory},
        std::vector<VtArray<TfToken>>{
            VtArray<TfToken>{childZeroCategory},
            VtArray<TfToken>{childOneCategory}});
    sceneDelegate.SetCurveCategories(VtArray<TfToken>{curveCategory});
    sceneDelegate.SetCurveInstancer(childId);

    std::unique_ptr<HdEmbreeBasisCurves> curve =
        std::make_unique<HdEmbreeBasisCurves>(curveId);
    curve->SetMinimumWidth(0.0f, 1);
    sceneDelegate.dirtyBits = curve->GetInitialDirtyBitsMask();
    TfDiagnosticTrap initialTrap;
    _SyncCurveRprim(
        curve.get(), &sceneDelegate,
        context.renderDelegate.GetRenderParam());
    bool const initialWasClean = initialTrap.GetErrors().empty();
    initialTrap.Clear();
    ty::PrototypeContext const* const prototype =
        curve->GetPrototypeContext(0);
    std::uint64_t const geometryGeneration =
        curve->GetGeometryGeneration();
    if (!_Require(initialWasClean, "BasisCurves instancing",
                  "valid zero-instance Sync emitted an error") ||
        !_Require(curve->GetCurveGeometryRecordCount() == 1,
                  "BasisCurves instancing",
                  "zero instances removed prototype geometry") ||
        !_Require(curve->GetInstanceCount() == 0,
                  "BasisCurves instancing",
                  "zero point instances published a root instance")) {
        return false;
    }

    // This focused fixture owns the Rprim directly rather than inserting it
    // into the RenderIndex. Remove its synthetic dependency so test-driven
    // instancer dirtiness does not ask the tracker to dirty an unregistered
    // Rprim; the test supplies the corresponding Rprim bit explicitly below.
    context.renderIndex->GetChangeTracker()
        .RemoveInstancerRprimDependency(childId, curveId);

    sceneDelegate.SetInstancerInstances(
        childId, VtIntArray{0, 1}, childTranslations);
    sceneDelegate.MarkInstancerDirty(
        childId, HdChangeTracker::DirtyInstanceIndex);
    sceneDelegate.MarkDirty(HdChangeTracker::DirtyInstanceIndex);
    _SyncCurveRprim(
        curve.get(), &sceneDelegate,
        context.renderDelegate.GetRenderParam());
    if (curve->GetInstanceCount() != 4) {
        std::printf(
            "    BasisCurves instancing: expected 4 instances, got %zu\n",
            curve->GetInstanceCount());
    }
    if (!_Require(curve->GetInstanceCount() == 4,
                  "BasisCurves instancing",
                  "nested two-by-two instances were not flattened") ||
        !_Require(
            curve->GetGeometryGeneration() == geometryGeneration &&
                curve->GetPrototypeContext(0) == prototype,
            "BasisCurves instancing",
            "indices-only instance update replaced the prototype")) {
        return false;
    }

    GfMatrix4d parentMatrices[2] = {
        GfMatrix4d(1.0), GfMatrix4d(1.0)};
    GfMatrix4d childMatrices[2] = {
        GfMatrix4d(1.0), GfMatrix4d(1.0)};
    for (size_t index = 0; index < 2; ++index) {
        parentMatrices[index].SetTranslate(
            GfVec3d(parentTranslations[index]));
        childMatrices[index].SetTranslate(
            GfVec3d(childTranslations[index]));
        childMatrices[index] = childMatrices[index] * childScale;
    }

    TfToken const parentInstanceCategories[2] = {
        parentZeroCategory, parentOneCategory};
    TfToken const childInstanceCategories[2] = {
        childZeroCategory, childOneCategory};
    ty::InstanceContext const* firstInstance = nullptr;
    for (size_t parentIndex = 0; parentIndex < 2; ++parentIndex) {
        for (size_t childIndex = 0; childIndex < 2; ++childIndex) {
            size_t const flattenedIndex = parentIndex * 2 + childIndex;
            ty::InstanceContext const* const instance =
                curve->GetInstanceContext(flattenedIndex);
            GfMatrix4f const expectedTransform(
                childMatrices[childIndex] * parentMatrices[parentIndex]);
            ty::CategorySet const expectedCategories{
                parentCategory,
                parentInstanceCategories[parentIndex],
                childCategory,
                childInstanceCategories[childIndex],
                curveCategory};
            if (!_Require(
                    instance &&
                        instance->instanceId ==
                            static_cast<int32_t>(flattenedIndex) &&
                        instance->rootScene != nullptr &&
                        _Close(
                            instance->objectToWorldMatrix,
                            expectedTransform) &&
                        instance->categories == expectedCategories,
                    "BasisCurves instancing",
                    "flattened transform, ID, or categories were incorrect")) {
                return false;
            }
            if (flattenedIndex == 0) {
                firstInstance = instance;
            }
        }
    }

    GfMatrix4f const firstExpected(
        childMatrices[0] * parentMatrices[0]);
    GfVec3f const worldProbe = firstExpected.Transform(
        GfVec3f(-0.5f, 0.25f, 2.0f));
    if (!_Require(
            _IntersectRprimRoot(&context.renderDelegate, worldProbe),
            "BasisCurves instancing",
            "non-uniform nested instance was not intersectable")) {
        return false;
    }

    GfMatrix4d updatedChildScale(1.0);
    updatedChildScale.SetScale(GfVec3d(0.5, 2.0, 3.0));
    sceneDelegate.SetInstancerTransform(childId, updatedChildScale);
    sceneDelegate.MarkInstancerDirty(
        childId, HdChangeTracker::DirtyTransform);
    sceneDelegate.MarkDirty(
        HdChangeTracker::DirtyInstancer |
        HdChangeTracker::DirtyTransform);
    _SyncCurveRprim(
        curve.get(), &sceneDelegate,
        context.renderDelegate.GetRenderParam());
    GfMatrix4d updatedChildZero(1.0);
    updatedChildZero.SetTranslate(GfVec3d(childTranslations[0]));
    GfMatrix4f const updatedExpected(
        updatedChildZero * updatedChildScale * parentMatrices[0]);
    ty::InstanceContext const* const updatedFirstInstance =
        curve->GetInstanceContext(0);
    if (!_Require(
            updatedFirstInstance == firstInstance &&
                _Close(
                    updatedFirstInstance->objectToWorldMatrix,
                    updatedExpected) &&
                curve->GetGeometryGeneration() == geometryGeneration &&
                curve->GetPrototypeContext(0) == prototype,
            "BasisCurves instancing",
            "instancer-transform update replaced stable prototype state")) {
        return false;
    }

    sceneDelegate.SetInstancerInstances(
        childId, VtIntArray(), childTranslations);
    sceneDelegate.MarkInstancerDirty(
        childId, HdChangeTracker::DirtyInstanceIndex);
    sceneDelegate.MarkDirty(HdChangeTracker::DirtyInstanceIndex);
    _SyncCurveRprim(
        curve.get(), &sceneDelegate,
        context.renderDelegate.GetRenderParam());
    if (!_Require(curve->GetInstanceCount() == 0,
                  "BasisCurves instancing",
                  "instance removal retained root geometry") ||
        !_Require(
            !_IntersectRprimRoot(&context.renderDelegate, worldProbe),
            "BasisCurves instancing",
            "zero-instance update retained a stale hit") ||
        !_Require(
            curve->GetGeometryGeneration() == geometryGeneration &&
                curve->GetPrototypeContext(0) == prototype,
            "BasisCurves instancing",
            "instance removal replaced the prototype")) {
        return false;
    }

    sceneDelegate.SetInstancerInstances(
        childId, VtIntArray{0, 1}, childTranslations);
    sceneDelegate.MarkInstancerDirty(
        childId, HdChangeTracker::DirtyInstanceIndex);
    sceneDelegate.MarkDirty(HdChangeTracker::DirtyInstanceIndex);
    _SyncCurveRprim(
        curve.get(), &sceneDelegate,
        context.renderDelegate.GetRenderParam());
    bool const recovered = _Require(
        curve->GetInstanceCount() == 4 &&
            curve->GetGeometryGeneration() == geometryGeneration &&
            curve->GetPrototypeContext(0) == prototype,
        "BasisCurves instancing",
        "multiple instances did not recover after an empty update");
    curve->Finalize(context.renderDelegate.GetRenderParam());
    return recovered &&
        _Require(
            curve->GetInstanceCount() == 0 &&
                curve->GetCurveGeometryRecordCount() == 0,
            "BasisCurves instancing",
            "Finalize retained Embree ownership records");
}

bool
_SampleRprimFloatPrimvar(
    HdEmbreeBasisCurves const& curve,
    TfToken const& name,
    unsigned int primitiveId,
    float u,
    float expected)
{
    ty::PrototypeContext const* const context =
        curve.GetPrototypeContext(0);
    if (!context) {
        return false;
    }
    std::unordered_map<
        TfToken,
        std::unique_ptr<ty::PrimvarSampler>,
        TfToken::HashFunctor>::const_iterator const samplerIt =
            context->primvarMap.find(name);
    if (samplerIt == context->primvarMap.end()) {
        return false;
    }
    float value = 0.0f;
    return samplerIt->second->Sample(primitiveId, u, 0.0f, &value) &&
        _Close(value, expected);
}

bool
TestBasisCurvesDirtyPullAndIndexedTransitions()
{
    _CurveRprimTestContext context;
    if (!context.renderIndex) {
        return false;
    }
    _CurveRprimSceneDelegate sceneDelegate(context.renderIndex.get());
    SdfPath const id("/dirtyCurve");
    _AddRprimTestCurve(&sceneDelegate, id);
    std::unique_ptr<HdEmbreeBasisCurves> curve =
        std::make_unique<HdEmbreeBasisCurves>(id);
    curve->SetMinimumWidth(0.0f, 1);
    sceneDelegate.dirtyBits = curve->GetInitialDirtyBitsMask();
    _SyncCurveRprim(
        curve.get(), &sceneDelegate,
        context.renderDelegate.GetRenderParam(),
        HdReprTokens->smoothHull, false);
    if (!_Require(curve->GetCurveGeometryRecordCount() == 1,
                  "BasisCurves dirty pulls",
                  "initial curve geometry was absent")) {
        return false;
    }

    // Verify the propagation hook independently, then pass the raw topology
    // bit to this direct-Rprim fixture. A real RenderIndex retains exactly
    // this raw mask while passing the propagated mask to Sync.
    HdDirtyBits const propagatedTopology =
        curve->PropagateRprimDirtyBits(HdChangeTracker::DirtyTopology);
    if (!_Require(
            (propagatedTopology & HdChangeTracker::DirtyWidths) != 0,
            "BasisCurves dirty pulls",
            "topology did not propagate to widths")) {
        return false;
    }
    sceneDelegate.ResetPullCounts();
    sceneDelegate.MarkDirty(HdChangeTracker::DirtyTopology);
    _SyncCurveRprim(
        curve.get(), &sceneDelegate,
        context.renderDelegate.GetRenderParam(),
        HdReprTokens->smoothHull, false);
    if (!_Require(
            sceneDelegate.topologyCalls == 1 &&
                sceneDelegate.primvarDescriptorCalls == 0 &&
                sceneDelegate.computedDescriptorCalls == 0 &&
                sceneDelegate.valueCalls == 0 &&
                sceneDelegate.indexedValueCalls == 0,
            "BasisCurves dirty pulls",
            "topology-only Sync pulled clean primvar data")) {
        return false;
    }

    TfToken const customName("custom");
    std::uint64_t const geometryGeneration =
        curve->GetGeometryGeneration();
    ty::PrototypeContext const* const prototypeContext =
        curve->GetPrototypeContext(0);
    sceneDelegate.ResetPullCounts();
    sceneDelegate.SetCustomPrimvar(
        VtFloatArray{1.0f, 2.0f, 3.0f}, VtIntArray(), false);
    _SyncCurveRprim(
        curve.get(), &sceneDelegate,
        context.renderDelegate.GetRenderParam());
    if (!_Require(
            sceneDelegate.customValueCalls == 1 &&
                sceneDelegate.customIndexedValueCalls == 0,
            "BasisCurves indexed primvar",
            "unindexed source used the wrong getter") ||
        !_Require(
            curve->GetGeometryGeneration() == geometryGeneration &&
                curve->GetPrototypeContext(0) == prototypeContext,
            "BasisCurves indexed primvar",
            "material-only primvar update rebuilt geometry") ||
        !_Require(
            _SampleRprimFloatPrimvar(
                *curve, customName, 0, 0.0f, 1.0f),
            "BasisCurves indexed primvar",
            "unindexed varying sampler was stale")) {
        return false;
    }

    sceneDelegate.ResetPullCounts();
    sceneDelegate.SetCustomPrimvar(
        VtFloatArray{10.0f, 20.0f}, VtIntArray{0, 1, 0}, true);
    _SyncCurveRprim(
        curve.get(), &sceneDelegate,
        context.renderDelegate.GetRenderParam());
    if (!_Require(
            sceneDelegate.customIndexedValueCalls == 1 &&
                sceneDelegate.customValueCalls == 0,
            "BasisCurves indexed primvar",
            "indexed transition did not use GetIndexedPrimvar") ||
        !_Require(
            _SampleRprimFloatPrimvar(
                *curve, customName, 0, 0.0f, 10.0f),
            "BasisCurves indexed primvar",
            "indexed sampler did not use authored indices")) {
        return false;
    }

    sceneDelegate.ResetPullCounts();
    sceneDelegate.SetCustomIndices(VtIntArray{1, 0, 1});
    _SyncCurveRprim(
        curve.get(), &sceneDelegate,
        context.renderDelegate.GetRenderParam());
    if (!_Require(sceneDelegate.customIndexedValueCalls == 1,
                  "BasisCurves indexed primvar",
                  "indices-only update did not repull indices") ||
        !_Require(
            _SampleRprimFloatPrimvar(
                *curve, customName, 0, 0.0f, 20.0f),
            "BasisCurves indexed primvar",
            "indices-only update retained stale flattened storage")) {
        return false;
    }

    sceneDelegate.ResetPullCounts();
    sceneDelegate.SetCustomPrimvar(
        VtFloatArray{3.0f, 4.0f, 5.0f}, VtIntArray(), false);
    _SyncCurveRprim(
        curve.get(), &sceneDelegate,
        context.renderDelegate.GetRenderParam());
    if (!_Require(
            sceneDelegate.customValueCalls == 1 &&
                sceneDelegate.customIndexedValueCalls == 0,
            "BasisCurves indexed primvar",
            "unindexed transition retained indexed getter state") ||
        !_Require(
            _SampleRprimFloatPrimvar(
                *curve, customName, 0, 0.0f, 3.0f),
            "BasisCurves indexed primvar",
            "unindexed transition retained stale flattened storage")) {
        return false;
    }

    sceneDelegate.ResetPullCounts();
    sceneDelegate.SetWidths(VtFloatArray{0.3f});
    _SyncCurveRprim(
        curve.get(), &sceneDelegate,
        context.renderDelegate.GetRenderParam());
    if (!_Require(
            sceneDelegate.primvarDescriptorCalls == HdInterpolationCount &&
                sceneDelegate.computedDescriptorCalls ==
                    HdInterpolationCount &&
                sceneDelegate.valueCalls == 1 &&
                sceneDelegate.indexedValueCalls == 0 &&
                sceneDelegate.customValueCalls == 0,
            "BasisCurves dirty pulls",
            "DirtyWidths repulled unrelated primvar values")) {
        return false;
    }

    sceneDelegate.ResetPullCounts();
    sceneDelegate.SetPrimvarsWidths(VtFloatArray{0.6f});
    _SyncCurveRprim(
        curve.get(), &sceneDelegate,
        context.renderDelegate.GetRenderParam());
    if (!_Require(sceneDelegate.primvarsWidthsValueCalls == 1,
                  "BasisCurves width precedence",
                  "new primvars:widths source was not pulled") ||
        !_Require(
            _SampleRprimFloatPrimvar(
                *curve, HdTokens->widths, 0, 0.0f, 0.6f),
            "BasisCurves width precedence",
            "primvars:widths did not override built-in widths")) {
        return false;
    }

    sceneDelegate.ResetPullCounts();
    sceneDelegate.SetPrimvarsWidths(VtFloatArray{0.8f});
    _SyncCurveRprim(
        curve.get(), &sceneDelegate,
        context.renderDelegate.GetRenderParam());
    if (!_Require(sceneDelegate.primvarsWidthsValueCalls == 1,
                  "BasisCurves width precedence",
                  "DirtyWidths did not refresh existing primvars:widths") ||
        !_Require(
            _SampleRprimFloatPrimvar(
                *curve, HdTokens->widths, 0, 0.0f, 0.8f),
            "BasisCurves width precedence",
            "updated primvars:widths retained stale sampler data")) {
        return false;
    }

    sceneDelegate.SetPrimvarsWidths(VtFloatArray{0.1f, 0.2f});
    TfDiagnosticTrap invalidHighPriorityTrap;
    _SyncCurveRprim(
        curve.get(), &sceneDelegate,
        context.renderDelegate.GetRenderParam());
    bool invalidHighPriorityReported = false;
    for (TfWarning const& warning :
            invalidHighPriorityTrap.GetWarnings()) {
        invalidHighPriorityReported |=
            warning.GetCommentary().find("cause=invalidSource") !=
                std::string::npos;
    }
    invalidHighPriorityTrap.Clear();
    if (!_Require(invalidHighPriorityReported,
                  "BasisCurves width precedence",
                  "invalid high-priority source was not reported") ||
        !_Require(curve->GetCurveGeometryRecordCount() == 0,
                  "BasisCurves width precedence",
                  "invalid high-priority source fell through to built-in")) {
        return false;
    }

    sceneDelegate.RemovePrimvarsWidths();
    _SyncCurveRprim(
        curve.get(), &sceneDelegate,
        context.renderDelegate.GetRenderParam());
    if (!_Require(curve->GetCurveGeometryRecordCount() == 1,
                  "BasisCurves width precedence",
                  "removing primvars:widths did not restore built-in") ||
        !_Require(
            _SampleRprimFloatPrimvar(
                *curve, HdTokens->widths, 0, 0.0f, 0.3f),
            "BasisCurves width precedence",
            "restored built-in widths sampler was stale")) {
        return false;
    }

    sceneDelegate.RemoveCustomPrimvar();
    _SyncCurveRprim(
        curve.get(), &sceneDelegate,
        context.renderDelegate.GetRenderParam());
    ty::PrototypeContext const* const finalContext =
        curve->GetPrototypeContext(0);
    bool const removed = finalContext &&
        finalContext->primvarMap.find(customName) ==
            finalContext->primvarMap.end();
    curve->Finalize(context.renderDelegate.GetRenderParam());
    return _Require(removed, "BasisCurves indexed primvar",
                    "removed descriptor left a stale sampler");
}

bool
TestBasisCurvesTopologyRevalidatesCachedPrimvars()
{
    _CurveRprimTestContext context;
    if (!context.renderIndex) {
        return false;
    }
    _CurveRprimSceneDelegate sceneDelegate(context.renderIndex.get());
    SdfPath const id("/topologyRevalidationCurve");
    TfToken const customName("custom");
    _AddRprimTestCurve(&sceneDelegate, id);
    sceneDelegate.SetCurvePoints(
        id, VtVec3fArray{
            GfVec3f(-2.5f, 0.0f, 0.0f),
            GfVec3f(-1.5f, 0.5f, 0.0f),
            GfVec3f(-0.5f, 0.0f, 0.0f),
            GfVec3f( 0.5f, 0.0f, 0.0f),
            GfVec3f( 1.5f, 0.5f, 0.0f),
            GfVec3f( 2.5f, 0.0f, 0.0f)});
    sceneDelegate.SetCurveTopology(HdBasisCurvesTopology(
        HdTokens->linear, HdTokens->bezier,
        HdTokens->nonperiodic, VtIntArray{6}, VtIntArray()));
    sceneDelegate.SetCustomPrimvar(
        VtFloatArray{1.0f}, VtIntArray(), false,
        HdInterpolationUniform);

    std::unique_ptr<HdEmbreeBasisCurves> curve =
        std::make_unique<HdEmbreeBasisCurves>(id);
    curve->SetMinimumWidth(0.0f, 1);
    sceneDelegate.dirtyBits = curve->GetInitialDirtyBitsMask();
    _SyncCurveRprim(
        curve.get(), &sceneDelegate,
        context.renderDelegate.GetRenderParam(),
        HdReprTokens->smoothHull, false);
    if (!_Require(
            _SampleRprimFloatPrimvar(
                *curve, customName, 0, 0.0f, 1.0f),
            "BasisCurves topology revalidation",
            "initial uniform sampler was invalid")) {
        return false;
    }

    // Splitting one authored curve into two preserves the point domain but
    // changes the uniform domain. The cached one-element primvar must be
    // revalidated and omitted without any descriptor or value/index pull.
    sceneDelegate.ResetPullCounts();
    sceneDelegate.SetCurveTopology(HdBasisCurvesTopology(
        HdTokens->linear, HdTokens->bezier,
        HdTokens->nonperiodic, VtIntArray{3, 3}, VtIntArray()));
    TfDiagnosticTrap topologyTrap;
    _SyncCurveRprim(
        curve.get(), &sceneDelegate,
        context.renderDelegate.GetRenderParam(),
        HdReprTokens->smoothHull, false);
    bool samplerWasRemoved = false;
    ty::PrototypeContext const* const splitContext =
        curve->GetPrototypeContext(0);
    if (splitContext) {
        samplerWasRemoved = splitContext->primvarMap.find(customName) ==
            splitContext->primvarMap.end();
    }
    bool revalidationWasReported = false;
    for (TfWarning const& warning : topologyTrap.GetWarnings()) {
        std::string const& commentary = warning.GetCommentary();
        revalidationWasReported |=
            commentary.find("category=primvar") != std::string::npos &&
            commentary.find("custom:elementCountMismatch") !=
                std::string::npos &&
            commentary.find("fallback=omitSampler") != std::string::npos;
    }
    topologyTrap.Clear();
    if (!_Require(
            sceneDelegate.topologyCalls == 1 &&
                sceneDelegate.primvarDescriptorCalls == 0 &&
                sceneDelegate.computedDescriptorCalls == 0 &&
                sceneDelegate.valueCalls == 0 &&
                sceneDelegate.indexedValueCalls == 0,
            "BasisCurves topology revalidation",
            "topology-only revalidation pulled clean scene data") ||
        !_Require(samplerWasRemoved && revalidationWasReported,
                  "BasisCurves topology revalidation",
                  "invalid cached uniform sampler was retained or silent")) {
        return false;
    }

    // A simultaneous topology and primvar update must consume the new value
    // once, rather than rebuilding with the old cache after topology
    // propagation adds generic dirty bits.
    sceneDelegate.ResetPullCounts();
    sceneDelegate.SetCurveTopology(HdBasisCurvesTopology(
        HdTokens->linear, HdTokens->bezier,
        HdTokens->nonperiodic, VtIntArray{6}, VtIntArray()));
    sceneDelegate.SetCustomPrimvar(
        VtFloatArray{7.0f}, VtIntArray(), false,
        HdInterpolationUniform);
    _SyncCurveRprim(
        curve.get(), &sceneDelegate,
        context.renderDelegate.GetRenderParam(),
        HdReprTokens->smoothHull, false);
    bool const updatedOnce =
        sceneDelegate.topologyCalls == 1 &&
        sceneDelegate.customValueCalls == 1 &&
        sceneDelegate.customIndexedValueCalls == 0 &&
        _SampleRprimFloatPrimvar(
            *curve, customName, 0, 0.0f, 7.0f);
    curve->Finalize(context.renderDelegate.GetRenderParam());
    return _Require(
        updatedOnce, "BasisCurves topology revalidation",
        "simultaneous topology/primvar update missed or duplicated new data");
}

bool
TestBasisCurvesMaterialBindingsAndRegistryRefresh()
{
    _CurveRprimTestContext context;
    if (!context.renderIndex) {
        return false;
    }
    _CurveRprimSceneDelegate sceneDelegate(context.renderIndex.get());
    SdfPath const curveId("/materialCurve");
    SdfPath const materialId("/curveMaterial");
    TfToken const customName("custom");
    _AddRprimTestCurve(&sceneDelegate, curveId);
    sceneDelegate.SetCustomPrimvar(
        VtFloatArray{1.0f, 2.0f, 3.0f}, VtIntArray(), false);
    sceneDelegate.SetMaterial(
        materialId,
        VtValue(_MakeCurveGeomPropMaterialNetwork(customName)));
    context.renderIndex->InsertSprim(
        HdPrimTypeTokens->material, &sceneDelegate, materialId);
    HdSprim* const material = context.renderIndex->GetSprim(
        HdPrimTypeTokens->material, materialId);
    if (!_Require(material != nullptr, "BasisCurves material bindings",
                  "material Sprim was not created")) {
        return false;
    }
    HdDirtyBits materialBits = material->GetInitialDirtyBitsMask();
    material->Sync(
        &sceneDelegate, context.renderDelegate.GetRenderParam(),
        &materialBits);

    std::unique_ptr<HdRprim, _FactoryRprimDeleter> curveOwner(
        context.renderDelegate.CreateRprim(
            HdPrimTypeTokens->basisCurves, curveId),
        _FactoryRprimDeleter{&context.renderDelegate});
    HdEmbreeBasisCurves* const curve =
        dynamic_cast<HdEmbreeBasisCurves*>(curveOwner.get());
    if (!_Require(curve != nullptr, "BasisCurves material bindings",
                  "factory curve was not created")) {
        return false;
    }
    curve->SetMinimumWidth(0.0f, 1);
    sceneDelegate.dirtyBits = curve->GetInitialDirtyBitsMask();
    _SyncCurveRprim(
        curve, &sceneDelegate, context.renderDelegate.GetRenderParam());
    ty::PrototypeContext const* prototype =
        curve->GetPrototypeContext(0);
    std::uint64_t const geometryGeneration =
        curve->GetGeometryGeneration();
    if (!_Require(
            prototype && prototype->material &&
                prototype->material->geomPropTokens ==
                    std::vector<TfToken>{customName} &&
                prototype->geomPropSamplers.size() == 1 &&
                prototype->geomPropSamplers[0] != nullptr,
            "BasisCurves material bindings",
            "initial material geomprop was not resolved")) {
        return false;
    }

    sceneDelegate.SetCustomPrimvar(
        VtFloatArray{7.0f, 8.0f, 9.0f}, VtIntArray(), false);
    _SyncCurveRprim(
        curve, &sceneDelegate, context.renderDelegate.GetRenderParam());
    prototype = curve->GetPrototypeContext(0);
    float updatedValue = 0.0f;
    bool const primvarRefreshWorked =
        prototype && prototype->geomPropSamplers.size() == 1 &&
        prototype->geomPropSamplers[0] &&
        prototype->geomPropSamplers[0]->Sample(
            0, 0.0f, 0.0f, &updatedValue) &&
        _Close(updatedValue, 7.0f) &&
        curve->GetGeometryGeneration() == geometryGeneration;
    if (!_Require(
            primvarRefreshWorked, "BasisCurves material bindings",
            "primvar replacement left a stale geomprop observer")) {
        return false;
    }

    sceneDelegate.SetMaterialResource(VtValue(
        _MakeCurveGeomPropMaterialNetwork(HdTokens->displayOpacity)));
    materialBits = HdMaterial::DirtyResource;
    material->Sync(
        &sceneDelegate, context.renderDelegate.GetRenderParam(),
        &materialBits);
    // Material Sync stops rendering and bumps the material version. The live
    // curve registry must let the delegate refresh this Rprim even though no
    // curve dirty bit is generated by the graph recompilation.
    context.renderDelegate.RefreshMaterialBindings();
    prototype = curve->GetPrototypeContext(0);
    std::unordered_map<
        TfToken,
        std::unique_ptr<ty::PrimvarSampler>,
        TfToken::HashFunctor>::const_iterator opacityIt;
    bool registryRefreshWorked = false;
    if (prototype) {
        opacityIt = prototype->primvarMap.find(HdTokens->displayOpacity);
        registryRefreshWorked =
            prototype->material &&
            prototype->material->geomPropTokens ==
                std::vector<TfToken>{HdTokens->displayOpacity} &&
            prototype->geomPropSamplers.size() == 1 &&
            opacityIt != prototype->primvarMap.end() &&
            prototype->geomPropSamplers[0] == opacityIt->second.get();
    }
    if (!_Require(
            registryRefreshWorked, "BasisCurves material bindings",
            "live registry did not refresh recompiled material bindings")) {
        return false;
    }

    sceneDelegate.SetMaterial(SdfPath(), VtValue());
    _SyncCurveRprim(
        curve, &sceneDelegate, context.renderDelegate.GetRenderParam());
    prototype = curve->GetPrototypeContext(0);
    return _Require(
        prototype && !prototype->material &&
            prototype->geomPropSamplers.empty() &&
            prototype->geomPropUniformValues.empty() &&
            curve->GetGeometryGeneration() == geometryGeneration,
        "BasisCurves material bindings",
        "material unbind retained observers or rebuilt geometry");
}

bool
TestBasisCurvesComputedPrimvarPrecedenceAndRemoval()
{
    _CurveRprimTestContext context;
    if (!context.renderIndex) {
        return false;
    }
    _CurveRprimSceneDelegate sceneDelegate(context.renderIndex.get());
    SdfPath const id("/computedCurve");
    TfToken const customName("custom");
    _AddRprimTestCurve(&sceneDelegate, id);
    sceneDelegate.SetCustomPrimvar(
        VtFloatArray{1.0f, 2.0f, 3.0f}, VtIntArray(), false);

    context.renderIndex->InsertSprim(
        HdPrimTypeTokens->extComputation, &sceneDelegate,
        sceneDelegate.GetComputationId());
    HdSprim* const computation = context.renderIndex->GetSprim(
        HdPrimTypeTokens->extComputation,
        sceneDelegate.GetComputationId());
    if (!_Require(computation != nullptr, "BasisCurves computed primvar",
                  "computation Sprim was not created")) {
        return false;
    }
    HdDirtyBits computationBits = HdExtComputation::DirtyBits::AllDirty;
    computation->Sync(&sceneDelegate, nullptr, &computationBits);

    std::unique_ptr<HdEmbreeBasisCurves> curve =
        std::make_unique<HdEmbreeBasisCurves>(id);
    curve->SetMinimumWidth(0.0f, 1);
    sceneDelegate.dirtyBits = curve->GetInitialDirtyBitsMask();
    _SyncCurveRprim(
        curve.get(), &sceneDelegate,
        context.renderDelegate.GetRenderParam());
    std::uint64_t const geometryGeneration =
        curve->GetGeometryGeneration();
    ty::PrototypeContext const* const prototypeContext =
        curve->GetPrototypeContext(0);
    if (!_Require(
            _SampleRprimFloatPrimvar(
                *curve, customName, 0, 0.0f, 1.0f),
            "BasisCurves computed primvar",
            "authored baseline sampler was incorrect")) {
        return false;
    }

    sceneDelegate.ResetPullCounts();
    sceneDelegate.SetComputedPrimvar(
        customName, HdInterpolationVarying,
        VtValue(VtFloatArray{9.0f, 8.0f, 7.0f}),
        HdChangeTracker::DirtyComputationPrimvarDesc);
    _SyncCurveRprim(
        curve.get(), &sceneDelegate,
        context.renderDelegate.GetRenderParam(),
        HdReprTokens->smoothHull, false);
    if (!_Require(sceneDelegate.computationInvocations == 1,
                  "BasisCurves computed primvar",
                  "computed descriptor was not evaluated once") ||
        !_Require(sceneDelegate.customValueCalls == 0,
                  "BasisCurves computed primvar",
                  "computed descriptor change repulled clean authored data") ||
        !_Require(
            _SampleRprimFloatPrimvar(
                *curve, customName, 0, 0.0f, 9.0f),
            "BasisCurves computed primvar",
            "computed source did not override authored source") ||
        !_Require(
            curve->GetGeometryGeneration() == geometryGeneration &&
                curve->GetPrototypeContext(0) == prototypeContext,
            "BasisCurves computed primvar",
            "general computed primvar rebuilt geometry")) {
        return false;
    }

    // Generic DirtyPrimvar is the Scene Index compatibility route for a
    // computed locator, including reserved names. The computed source remains
    // authoritative even though the authored value is cached in parallel.
    sceneDelegate.ResetPullCounts();
    sceneDelegate.SetComputedPrimvar(
        customName, HdInterpolationVarying,
        VtValue(VtFloatArray{6.0f, 5.0f, 4.0f}),
        HdChangeTracker::DirtyPrimvar);
    _SyncCurveRprim(
        curve.get(), &sceneDelegate,
        context.renderDelegate.GetRenderParam());
    if (!_Require(sceneDelegate.computationInvocations == 1,
                  "BasisCurves computed primvar",
                  "generic dirty route did not evaluate computed data") ||
        !_Require(
            _SampleRprimFloatPrimvar(
                *curve, customName, 0, 0.0f, 6.0f),
            "BasisCurves computed primvar",
            "generic dirty route selected authored data")) {
        return false;
    }

    sceneDelegate.ResetPullCounts();
    sceneDelegate.RemoveComputedPrimvar();
    _SyncCurveRprim(
        curve.get(), &sceneDelegate,
        context.renderDelegate.GetRenderParam(),
        HdReprTokens->smoothHull, false);
    bool const restoredAuthored =
        sceneDelegate.computationInvocations == 0 &&
        sceneDelegate.customValueCalls == 0 &&
        _SampleRprimFloatPrimvar(
            *curve, customName, 0, 0.0f, 1.0f);
    curve->Finalize(context.renderDelegate.GetRenderParam());
    return _Require(
        restoredAuthored, "BasisCurves computed primvar",
        "computed removal did not restore cached authored source");
}

bool
TestBasisCurvesComputedGeometrySources()
{
    _CurveRprimTestContext context;
    if (!context.renderIndex) {
        return false;
    }
    _CurveRprimSceneDelegate sceneDelegate(context.renderIndex.get());
    SdfPath const id("/computedGeometryCurve");
    _AddRprimTestCurve(&sceneDelegate, id);
    VtVec3fArray const computedPoints{
        GfVec3f(-1.0f, 2.0f, 0.0f),
        GfVec3f( 0.0f, 2.5f, 0.0f),
        GfVec3f( 1.0f, 2.0f, 0.0f)};
    sceneDelegate.SetComputedPrimvar(
        HdTokens->points, HdInterpolationVertex,
        VtValue(computedPoints),
        HdChangeTracker::DirtyComputationPrimvarDesc);

    context.renderIndex->InsertSprim(
        HdPrimTypeTokens->extComputation, &sceneDelegate,
        sceneDelegate.GetComputationId());
    HdSprim* const computation = context.renderIndex->GetSprim(
        HdPrimTypeTokens->extComputation,
        sceneDelegate.GetComputationId());
    if (!_Require(computation != nullptr,
                  "BasisCurves computed geometry",
                  "computation Sprim was not created")) {
        return false;
    }
    HdDirtyBits computationBits = HdExtComputation::DirtyBits::AllDirty;
    computation->Sync(&sceneDelegate, nullptr, &computationBits);

    std::unique_ptr<HdEmbreeBasisCurves> curve =
        std::make_unique<HdEmbreeBasisCurves>(id);
    curve->SetMinimumWidth(0.0f, 1);
    sceneDelegate.dirtyBits = curve->GetInitialDirtyBitsMask();
    sceneDelegate.ResetPullCounts();
    _SyncCurveRprim(
        curve.get(), &sceneDelegate,
        context.renderDelegate.GetRenderParam());
    GfVec3f const authoredProbe(-0.5f, 0.25f, 2.0f);
    GfVec3f const computedProbe(-0.5f, 2.25f, 2.0f);
    if (!_Require(sceneDelegate.computationInvocations == 1,
                  "BasisCurves computed geometry",
                  "computed points were not evaluated once") ||
        !_Require(
            _IntersectRprimRoot(&context.renderDelegate, computedProbe) &&
                !_IntersectRprimRoot(
                    &context.renderDelegate, authoredProbe),
            "BasisCurves computed geometry",
            "computed points did not override authored points")) {
        return false;
    }

    // A Scene Index can report a reserved computed locator as generic
    // DirtyPrimvar. Descriptor replacement must still remove computed points,
    // restore the authored cache, and select computed primvars:widths.
    sceneDelegate.SetComputedPrimvar(
        TfToken("primvars:widths"), HdInterpolationConstant,
        VtValue(VtFloatArray{0.6f}), HdChangeTracker::DirtyPrimvar);
    computationBits = HdExtComputation::DirtyBits::AllDirty;
    computation->Sync(&sceneDelegate, nullptr, &computationBits);
    sceneDelegate.ResetPullCounts();
    _SyncCurveRprim(
        curve.get(), &sceneDelegate,
        context.renderDelegate.GetRenderParam());
    if (!_Require(sceneDelegate.computationInvocations == 1,
                  "BasisCurves computed geometry",
                  "generic dirty route did not evaluate computed widths") ||
        !_Require(
            _SampleRprimFloatPrimvar(
                *curve, HdTokens->widths, 0, 0.0f, 0.6f),
            "BasisCurves computed geometry",
            "computed primvars:widths did not drive geometry/sampling") ||
        !_Require(
            _IntersectRprimRoot(&context.renderDelegate, authoredProbe) &&
                !_IntersectRprimRoot(
                    &context.renderDelegate, computedProbe),
            "BasisCurves computed geometry",
            "removing computed points did not restore authored points")) {
        return false;
    }

    VtVec3fArray const computedNormals(
        3, GfVec3f(0.0f, 0.0f, 1.0f));
    sceneDelegate.SetComputedPrimvar(
        TfToken("primvars:normals"), HdInterpolationVertex,
        VtValue(computedNormals),
        HdChangeTracker::DirtyComputationPrimvarDesc);
    computationBits = HdExtComputation::DirtyBits::AllDirty;
    computation->Sync(&sceneDelegate, nullptr, &computationBits);
    sceneDelegate.ResetPullCounts();
    _SyncCurveRprim(
        curve.get(), &sceneDelegate,
        context.renderDelegate.GetRenderParam(),
        HdReprTokens->smoothHull, false);
    ty::PrototypeContext const* prototype =
        curve->GetPrototypeContext(0);
    if (!_Require(sceneDelegate.computationInvocations == 1,
                  "BasisCurves computed geometry",
                  "computed normals were not evaluated once") ||
        !_Require(
            prototype && prototype->geometryKind ==
                ty::GeometryKind::orientedRibbon,
            "BasisCurves computed geometry",
            "computed primvars:normals did not create a ribbon")) {
        return false;
    }

    sceneDelegate.RemoveComputedPrimvar();
    sceneDelegate.ResetPullCounts();
    _SyncCurveRprim(
        curve.get(), &sceneDelegate,
        context.renderDelegate.GetRenderParam(),
        HdReprTokens->smoothHull, false);
    prototype = curve->GetPrototypeContext(0);
    bool const restoredAuthoredGeometry =
        sceneDelegate.computationInvocations == 0 &&
        prototype &&
        prototype->geometryKind == ty::GeometryKind::roundCurve &&
        _SampleRprimFloatPrimvar(
            *curve, HdTokens->widths, 0, 0.0f, 0.2f);
    curve->Finalize(context.renderDelegate.GetRenderParam());
    return _Require(
        restoredAuthoredGeometry, "BasisCurves computed geometry",
        "computed geometry removal did not restore authored sources");
}

bool
TestBasisCurvesNormalSourcePrecedence()
{
    _CurveRprimTestContext context;
    if (!context.renderIndex) {
        return false;
    }
    _CurveRprimSceneDelegate sceneDelegate(context.renderIndex.get());
    SdfPath const id("/normalCurve");
    _AddRprimTestCurve(&sceneDelegate, id);
    std::unique_ptr<HdEmbreeBasisCurves> curve =
        std::make_unique<HdEmbreeBasisCurves>(id);
    curve->SetMinimumWidth(0.0f, 1);
    sceneDelegate.dirtyBits = curve->GetInitialDirtyBitsMask();
    _SyncCurveRprim(
        curve.get(), &sceneDelegate,
        context.renderDelegate.GetRenderParam());
    ty::PrototypeContext const* prototype =
        curve->GetPrototypeContext(0);
    if (!_Require(
            prototype &&
                prototype->geometryKind == ty::GeometryKind::roundCurve,
            "BasisCurves normal precedence",
            "curve without normals was not a tube")) {
        return false;
    }

    VtVec3fArray const validNormals(
        3, GfVec3f(0.0f, 0.0f, 1.0f));
    sceneDelegate.SetNormals(validNormals);
    _SyncCurveRprim(
        curve.get(), &sceneDelegate,
        context.renderDelegate.GetRenderParam());
    prototype = curve->GetPrototypeContext(0);
    if (!_Require(
            prototype && prototype->geometryKind ==
                ty::GeometryKind::orientedRibbon,
            "BasisCurves normal precedence",
            "valid built-in normals did not create a ribbon")) {
        return false;
    }

    sceneDelegate.ResetPullCounts();
    sceneDelegate.SetPrimvarsNormals(validNormals);
    _SyncCurveRprim(
        curve.get(), &sceneDelegate,
        context.renderDelegate.GetRenderParam());
    prototype = curve->GetPrototypeContext(0);
    if (!_Require(sceneDelegate.primvarsNormalsValueCalls == 1,
                  "BasisCurves normal precedence",
                  "new primvars:normals source was not pulled") ||
        !_Require(
            prototype && prototype->geometryKind ==
                ty::GeometryKind::orientedRibbon,
            "BasisCurves normal precedence",
            "valid primvars:normals did not create a ribbon")) {
        return false;
    }

    sceneDelegate.ResetPullCounts();
    sceneDelegate.SetPrimvarsNormals(
        VtVec3fArray(3, GfVec3f(0.0f, 0.0f, -1.0f)));
    _SyncCurveRprim(
        curve.get(), &sceneDelegate,
        context.renderDelegate.GetRenderParam());
    if (!_Require(sceneDelegate.primvarsNormalsValueCalls == 1,
                  "BasisCurves normal precedence",
                  "DirtyNormals did not refresh primvars:normals")) {
        return false;
    }

    sceneDelegate.SetPrimvarsNormals(VtVec3fArray{
        GfVec3f(0.0f, 0.0f, 1.0f),
        GfVec3f(0.0f, 0.0f, 1.0f)});
    TfDiagnosticTrap invalidNormalTrap;
    _SyncCurveRprim(
        curve.get(), &sceneDelegate,
        context.renderDelegate.GetRenderParam());
    bool invalidNormalReported = false;
    for (TfWarning const& warning : invalidNormalTrap.GetWarnings()) {
        invalidNormalReported |=
            warning.GetCommentary().find("category=normal") !=
                std::string::npos &&
            warning.GetCommentary().find("fallback=tube") !=
                std::string::npos;
    }
    invalidNormalTrap.Clear();
    prototype = curve->GetPrototypeContext(0);
    if (!_Require(invalidNormalReported,
                  "BasisCurves normal precedence",
                  "invalid high-priority normals were not reported") ||
        !_Require(
            prototype &&
                prototype->geometryKind == ty::GeometryKind::roundCurve,
            "BasisCurves normal precedence",
            "invalid high-priority normals fell through to built-in")) {
        return false;
    }

    sceneDelegate.RemovePrimvarsNormals();
    _SyncCurveRprim(
        curve.get(), &sceneDelegate,
        context.renderDelegate.GetRenderParam());
    prototype = curve->GetPrototypeContext(0);
    bool const restoredBuiltIn = prototype &&
        prototype->geometryKind == ty::GeometryKind::orientedRibbon;
    curve->Finalize(context.renderDelegate.GetRenderParam());
    return _Require(
        restoredBuiltIn, "BasisCurves normal precedence",
        "removing primvars:normals did not restore built-in normals");
}

bool
TestBasisCurvesLegacyAndSceneIndexDataContract()
{
    SdfPath const curveId("/pairedCurve");
    SdfPath const computationId("/pairedComputation");
    TfToken const computedName("computedContract");
    TfToken const computationOutputName("output");
    VtIntArray const curveIndices{
        2, 0, 3, 1,
        6, 4, 7, 5,
        10, 8, 11, 9};
    VtVec3fArray const points{
        GfVec3f(-3.0f,  0.0f, 0.0f),
        GfVec3f(-3.0f,  2.0f, 0.0f),
        GfVec3f(-3.0f, -1.0f, 0.0f),
        GfVec3f(-3.0f,  1.0f, 0.0f),
        GfVec3f( 0.0f,  0.0f, 0.0f),
        GfVec3f( 0.0f,  2.0f, 0.0f),
        GfVec3f( 0.0f, -1.0f, 0.0f),
        GfVec3f( 0.0f,  1.0f, 0.0f),
        GfVec3f( 3.0f,  0.0f, 0.0f),
        GfVec3f( 3.0f,  2.0f, 0.0f),
        GfVec3f( 3.0f, -1.0f, 0.0f),
        GfVec3f( 3.0f,  1.0f, 0.0f),
        GfVec3f(20.0f, 20.0f, 0.0f)};
    VtFloatArray const computedValues{100.0f, 200.0f, 300.0f};

    _CurveRprimTestContext legacyContext;
    if (!legacyContext.renderIndex) {
        return false;
    }
    _CurveRprimSceneDelegate legacyDelegate(
        legacyContext.renderIndex.get());
    _AddRprimTestCurve(&legacyDelegate, curveId);
    HdBasisCurvesTopology legacyTopology(
        HdTokens->cubic,
        HdTokens->bspline,
        HdTokens->pinned,
        VtIntArray{4, 4, 4},
        curveIndices);
    legacyTopology.SetInvisibleCurves(VtIntArray{0});
    legacyTopology.SetInvisiblePoints(VtIntArray{6});
    legacyDelegate.SetCurveTopology(legacyTopology);
    legacyDelegate.SetCurvePoints(curveId, points);
    legacyDelegate.SetWidths(VtFloatArray{0.4f});
    legacyDelegate.SetCustomPrimvar(
        VtFloatArray{30.0f, 10.0f, 20.0f},
        VtIntArray{1, 2, 0},
        true,
        HdInterpolationUniform);
    legacyDelegate.SetComputedPrimvar(
        computedName,
        HdInterpolationUniform,
        VtValue(computedValues),
        HdChangeTracker::DirtyComputationPrimvarDesc);
    legacyContext.renderIndex->InsertSprim(
        HdPrimTypeTokens->extComputation,
        &legacyDelegate,
        legacyDelegate.GetComputationId());
    HdSprim* const legacyComputation =
        legacyContext.renderIndex->GetSprim(
            HdPrimTypeTokens->extComputation,
            legacyDelegate.GetComputationId());
    if (!_Require(
            legacyComputation != nullptr,
            "BasisCurves Legacy/Scene Index",
            "legacy computation Sprim was not created")) {
        return false;
    }
    HdDirtyBits legacyComputationBits =
        HdExtComputation::DirtyBits::AllDirty;
    legacyComputation->Sync(
        &legacyDelegate, nullptr, &legacyComputationBits);

    std::unique_ptr<HdEmbreeBasisCurves> legacyCurve =
        std::make_unique<HdEmbreeBasisCurves>(curveId);
    legacyCurve->SetMinimumWidth(0.0f, 1);
    legacyDelegate.dirtyBits = legacyCurve->GetInitialDirtyBitsMask();
    _SyncCurveRprim(
        legacyCurve.get(),
        &legacyDelegate,
        legacyContext.renderDelegate.GetRenderParam());

    HdRetainedSceneIndexRefPtr const retainedScene =
        HdRetainedSceneIndex::New();
    retainedScene->AddPrims({
        {
            curveId,
            HdPrimTypeTokens->basisCurves,
            _BuildRetainedCurveDataContractPrim(
                points,
                curveIndices,
                true,
                computedName,
                computationId,
                computationOutputName)},
        {
            computationId,
            HdPrimTypeTokens->extComputation,
            _BuildRetainedCurveComputationPrim(
                computationOutputName, computedValues)},
        {
            curveId.AppendChild(TfToken("invisibleCurves")),
            HdPrimTypeTokens->geomSubset,
            _BuildInvisibleCurveSubset(
                HdGeomSubsetSchemaTokens->typeCurveSet,
                VtIntArray{0})},
        {
            curveId.AppendChild(TfToken("invisiblePoints")),
            HdPrimTypeTokens->geomSubset,
            _BuildInvisibleCurveSubset(
                HdGeomSubsetSchemaTokens->typePointSet,
                VtIntArray{6})}});

    HdEmbreeRenderDelegate sceneIndexRenderDelegate;
    std::unique_ptr<HdRenderIndex> sceneIndexRenderIndex(
        HdRenderIndex::NewForBackendEmulation(
            &sceneIndexRenderDelegate, HdDriverVector(), retainedScene));
    if (!_Require(
            static_cast<bool>(sceneIndexRenderIndex),
            "BasisCurves Legacy/Scene Index",
            "Scene Index RenderIndex was not created")) {
        legacyCurve->Finalize(
            legacyContext.renderDelegate.GetRenderParam());
        return false;
    }
    HdEmbreeBasisCurves* const sceneIndexCurve =
        dynamic_cast<HdEmbreeBasisCurves*>(const_cast<HdRprim*>(
            sceneIndexRenderIndex->GetRprim(curveId)));
    HdSceneDelegate* const sceneIndexDelegate =
        sceneIndexRenderIndex->GetSceneDelegateForRprim(curveId);
    HdSprim* const sceneIndexComputation =
        sceneIndexRenderIndex->GetSprim(
            HdPrimTypeTokens->extComputation, computationId);
    if (!_Require(
            sceneIndexCurve && sceneIndexDelegate && sceneIndexComputation,
            "BasisCurves Legacy/Scene Index",
            "Scene Index adapters did not create curve/computation prims")) {
        legacyCurve->Finalize(
            legacyContext.renderDelegate.GetRenderParam());
        return false;
    }
    HdDirtyBits sceneIndexComputationBits =
        HdExtComputation::DirtyBits::AllDirty;
    sceneIndexComputation->Sync(
        sceneIndexDelegate, nullptr, &sceneIndexComputationBits);
    _SyncSceneIndexCurve(
        sceneIndexCurve,
        sceneIndexDelegate,
        sceneIndexRenderDelegate.GetRenderParam());

    std::vector<_CurveDataContractSample> legacySamples;
    std::vector<_CurveDataContractSample> sceneIndexSamples;
    bool const collected =
        _CollectCurveDataContractSamples(
            *legacyCurve, computedName, &legacySamples) &&
        _CollectCurveDataContractSamples(
            *sceneIndexCurve, computedName, &sceneIndexSamples);
    bool sawPartiallyVisibleCurve = false;
    bool sawFullyVisibleCurve = false;
    bool valuesUseAuthoredCurveDomain = collected;
    for (_CurveDataContractSample const& sample : legacySamples) {
        if (sample.metadata.authoredCurveId == 1) {
            sawPartiallyVisibleCurve = true;
            valuesUseAuthoredCurveDomain &=
                _Close(sample.custom, 20.0f) &&
                _Close(sample.computed, 200.0f);
        } else if (sample.metadata.authoredCurveId == 2) {
            sawFullyVisibleCurve = true;
            valuesUseAuthoredCurveDomain &=
                _Close(sample.custom, 30.0f) &&
                _Close(sample.computed, 300.0f);
        } else {
            valuesUseAuthoredCurveDomain = false;
        }
    }
    bool const initialParity =
        collected &&
        sawPartiallyVisibleCurve &&
        sawFullyVisibleCurve &&
        valuesUseAuthoredCurveDomain &&
        _CurveDataContractSamplesEqual(
            legacySamples, sceneIndexSamples) &&
        _IntersectRprimRoot(
            &legacyContext.renderDelegate,
            GfVec3f(0.0f, 1.5f, 2.0f)) &&
        _IntersectRprimRoot(
            &sceneIndexRenderDelegate,
            GfVec3f(0.0f, 1.5f, 2.0f));

    legacyDelegate.RemoveComputedPrimvar();
    _SyncCurveRprim(
        legacyCurve.get(),
        &legacyDelegate,
        legacyContext.renderDelegate.GetRenderParam());
    retainedScene->AddPrims({{
        curveId,
        HdPrimTypeTokens->basisCurves,
        _BuildRetainedCurveDataContractPrim(
            points,
            curveIndices,
            false,
            computedName,
            computationId,
            computationOutputName)}});
    _SyncSceneIndexCurve(
        sceneIndexCurve,
        sceneIndexDelegate,
        sceneIndexRenderDelegate.GetRenderParam());

    const auto computedWasRemoved = [&computedName](
        HdEmbreeBasisCurves const& curve) {
        for (size_t recordIndex = 0;
             recordIndex < curve.GetCurveGeometryRecordCount();
             ++recordIndex) {
            ty::PrototypeContext const* const context =
                curve.GetPrototypeContext(recordIndex);
            if (!context ||
                context->primvarMap.find(computedName) !=
                    context->primvarMap.end()) {
                return false;
            }
        }
        return true;
    };
    bool const removalParity =
        computedWasRemoved(*legacyCurve) &&
        computedWasRemoved(*sceneIndexCurve) &&
        legacyCurve->GetCurveGeometryRecordCount() ==
            sceneIndexCurve->GetCurveGeometryRecordCount();

    legacyCurve->Finalize(
        legacyContext.renderDelegate.GetRenderParam());
    return _Require(
        initialParity && removalParity,
        "BasisCurves Legacy/Scene Index",
        "pinned/indexed/computed/visibility contracts diverged");
}

HdRenderPassAovBinding
_MakeAovBinding(TfToken const& name, HdRenderBuffer* buffer)
{
    HdRenderPassAovBinding binding;
    binding.aovName = name;
    binding.renderBuffer = buffer;
    return binding;
}

template <typename T>
bool
_ReadFirstPixel(HdEmbreeRenderBuffer* buffer, T* value)
{
    if (!buffer || !value) {
        return false;
    }
    T const* const data = static_cast<T const*>(buffer->Map());
    if (!data) {
        return false;
    }
    *value = data[0];
    buffer->Unmap();
    return true;
}

bool
TestBasisCurvesRendererAndAovIntegration()
{
    _CurveRprimTestContext context;
    if (!context.renderIndex) {
        return false;
    }

    _CurveRprimSceneDelegate sceneDelegate(context.renderIndex.get());
    SdfPath const curveId("/shadedCurves");
    SdfPath const materialId("/curveMaterial");
    _AddRprimTestCurve(&sceneDelegate, curveId);
    sceneDelegate.SetCurveTopology(HdBasisCurvesTopology(
        HdTokens->linear,
        HdTokens->bezier,
        HdTokens->nonperiodic,
        VtIntArray{3, 3},
        VtIntArray()));
    // Curve zero has two generated Embree segments and stays outside the
    // image. Curve one is therefore generated primitive two but authored
    // element one, and its large tube covers the jittered one-pixel camera.
    sceneDelegate.SetCurvePoints(
        curveId,
        VtVec3fArray{
            GfVec3f(10.0f, -2.0f, -4.0f),
            GfVec3f(10.0f,  0.0f, -4.0f),
            GfVec3f(10.0f,  2.0f, -4.0f),
            GfVec3f( 0.0f, -8.0f, -4.0f),
            GfVec3f( 0.0f,  0.0f, -4.0f),
            GfVec3f( 0.0f,  8.0f, -4.0f)});
    sceneDelegate.SetWidths(VtFloatArray{3.0f});
    sceneDelegate.SetCustomPrimvar(
        VtFloatArray{0.2f, 0.1f},
        VtIntArray{1, 0},
        true,
        HdInterpolationUniform);
    sceneDelegate.SetMaterial(
        materialId,
        VtValue(_MakeCurveEmissiveGeomPropMaterialNetwork(
            TfToken("custom"))));
    context.renderIndex->InsertSprim(
        HdPrimTypeTokens->material, &sceneDelegate, materialId);
    HdSprim* const material = context.renderIndex->GetSprim(
        HdPrimTypeTokens->material, materialId);
    if (!material) {
        return false;
    }
    HdDirtyBits materialBits = material->GetInitialDirtyBitsMask();
    material->Sync(
        &sceneDelegate,
        context.renderDelegate.GetRenderParam(),
        &materialBits);

    context.renderIndex->InsertRprim(
        HdPrimTypeTokens->basisCurves, &sceneDelegate, curveId);
    HdEmbreeBasisCurves* const basisCurves =
        dynamic_cast<HdEmbreeBasisCurves*>(
            const_cast<HdRprim*>(context.renderIndex->GetRprim(curveId)));
    if (!basisCurves) {
        return false;
    }
    const int expectedPrimId = basisCurves->GetPrimId();
    _SyncCurveRprim(
        basisCurves,
        &sceneDelegate,
        context.renderDelegate.GetRenderParam());

    HdEmbreeRenderParam* const renderParam =
        static_cast<HdEmbreeRenderParam*>(
            context.renderDelegate.GetRenderParam());
    RTCScene const rootScene = renderParam->AcquireSceneForEdit();
    rtcCommitScene(rootScene);
    ty::Renderer* const renderer = renderParam->GetRenderer();
    renderer->SetCamera(GfMatrix4d(1.0), GfMatrix4d(1.0));
    renderer->SetDataWindow(GfRect2i(GfVec2i(0), 1, 1));
    renderer->SetLightingEnabled(false);
    ty::RenderSettings settings;
    settings.samplesToConvergence = 1;
    settings.randomNumberSeed = 1;
    renderer->SetRenderSettings(settings);

    HdEmbreeRenderBuffer color(SdfPath("/curveColor"));
    HdEmbreeRenderBuffer primId(SdfPath("/curvePrimId"));
    HdEmbreeRenderBuffer elementId(SdfPath("/curveElementId"));
    HdEmbreeRenderBuffer instanceId(SdfPath("/curveInstanceId"));
    HdEmbreeRenderBuffer normal(SdfPath("/curveNormal"));
    HdEmbreeRenderBuffer eyeNormal(SdfPath("/curveEyeNormal"));
    HdEmbreeRenderBuffer custom(SdfPath("/curveCustom"));
    HdEmbreeRenderBuffer cameraDepth(SdfPath("/curveCameraDepth"));
    HdEmbreeRenderBuffer depth(SdfPath("/curveDepth"));
    HdEmbreeRenderBuffer ambientOcclusion(
        SdfPath("/curveAmbientOcclusion"));
    if (!color.Allocate(
            GfVec3i(1), HdFormatFloat32Vec4, true) ||
        !primId.Allocate(
            GfVec3i(1), HdFormatInt32, true) ||
        !elementId.Allocate(
            GfVec3i(1), HdFormatInt32, true) ||
        !instanceId.Allocate(
            GfVec3i(1), HdFormatInt32, true) ||
        !normal.Allocate(
            GfVec3i(1), HdFormatFloat32Vec3, true) ||
        !eyeNormal.Allocate(
            GfVec3i(1), HdFormatFloat32Vec3, true) ||
        !custom.Allocate(
            GfVec3i(1), HdFormatFloat32Vec3, true) ||
        !cameraDepth.Allocate(
            GfVec3i(1), HdFormatFloat32, true) ||
        !depth.Allocate(
            GfVec3i(1), HdFormatFloat32, true) ||
        !ambientOcclusion.Allocate(
            GfVec3i(1), HdFormatFloat32Vec3, true)) {
        return false;
    }
    renderer->SetAovBindings({
        _MakeAovBinding(HdAovTokens->color, &color),
        _MakeAovBinding(HdAovTokens->primId, &primId),
        _MakeAovBinding(HdAovTokens->elementId, &elementId),
        _MakeAovBinding(HdAovTokens->instanceId, &instanceId),
        _MakeAovBinding(HdAovTokens->normal, &normal),
        _MakeAovBinding(HdAovTokens->Neye, &eyeNormal),
        _MakeAovBinding(TfToken("primvars:custom"), &custom),
        _MakeAovBinding(HdAovTokens->cameraDepth, &cameraDepth),
        _MakeAovBinding(HdAovTokens->depth, &depth),
        _MakeAovBinding(ty::AovTokens->ambocc, &ambientOcclusion)});

    HdRenderThread renderThread;
    renderThread.StartRender();
    renderer->Render(&renderThread);
    renderThread.StopRender();

    GfVec4f colorValue;
    int primIdValue = -1;
    int elementIdValue = -1;
    int instanceIdValue = -1;
    GfVec3f normalValue;
    GfVec3f eyeNormalValue;
    GfVec3f customValue;
    float cameraDepthValue = 0.0f;
    float depthValue = 0.0f;
    GfVec3f ambientOcclusionValue;
    if (!_ReadFirstPixel(&color, &colorValue) ||
        !_ReadFirstPixel(&primId, &primIdValue) ||
        !_ReadFirstPixel(&elementId, &elementIdValue) ||
        !_ReadFirstPixel(&instanceId, &instanceIdValue) ||
        !_ReadFirstPixel(&normal, &normalValue) ||
        !_ReadFirstPixel(&eyeNormal, &eyeNormalValue) ||
        !_ReadFirstPixel(&custom, &customValue) ||
        !_ReadFirstPixel(&cameraDepth, &cameraDepthValue) ||
        !_ReadFirstPixel(&depth, &depthValue) ||
        !_ReadFirstPixel(&ambientOcclusion, &ambientOcclusionValue)) {
        return false;
    }

    const bool normalIsSurfaceNormal =
        ty::IsFinite(normalValue) &&
        GfIsClose(normalValue.GetLength(), 1.0f, 1.0e-5f) &&
        std::abs(normalValue[1]) < 1.0e-4f;
    HdxPickResult const pickResult(
        &primIdValue,
        &instanceIdValue,
        &elementIdValue,
        nullptr,
        nullptr,
        nullptr,
        &depthValue,
        context.renderIndex.get(),
        HdxPickTokens->pickFaces,
        GfMatrix4d(1.0),
        GfMatrix4d(1.0),
        GfVec2f(0.0f, 1.0f),
        GfVec2i(1),
        GfVec4i(0, 0, 1, 1));
    HdxPickHitVector pickHits;
    pickResult.ResolveNearestToCenter(&pickHits);
    HdSelectionSharedPtr const selection =
        HdxUnitTestUtils::TranslateHitsToSelection(
            HdxPickTokens->pickFaces,
            HdSelection::HighlightModeSelect,
            pickHits);
    HdSelection::PrimSelectionState const* const selectionState =
        selection
        ? selection->GetPrimSelectionState(
              HdSelection::HighlightModeSelect, curveId)
        : nullptr;
    const bool authoredElementWasSelected =
        pickHits.size() == 1 &&
        pickHits[0].objectId == curveId &&
        pickHits[0].instanceIndex == 0 &&
        pickHits[0].elementIndex == 1 &&
        selectionState &&
        selectionState->elementIndices.size() == 1 &&
        selectionState->elementIndices[0].size() == 1 &&
        selectionState->elementIndices[0][0] == 1;
    const bool unlitAndAovsValid =
        renderer->DidLastFrameProduceValidPixels() &&
            renderer->GetCompletedSamples() == 1 &&
            GfIsClose(
                colorValue, GfVec4f(0.2f, 0.4f, 0.6f, 1.0f), 1.0e-5f) &&
            primIdValue == expectedPrimId &&
            elementIdValue == 1 &&
            instanceIdValue == 0 &&
            normalIsSurfaceNormal &&
            GfIsClose(eyeNormalValue, normalValue, 1.0e-5f) &&
            GfIsClose(customValue, GfVec3f(0.2f, 0.0f, 0.0f), 1.0e-5f) &&
            std::isfinite(cameraDepthValue) && cameraDepthValue > 0.0f &&
            std::isfinite(depthValue) &&
            ty::IsFinite(ambientOcclusionValue) &&
            renderer->GetAmbientOcclusionRayCount() == 1 &&
            authoredElementWasSelected;
    if (!unlitAndAovsValid) {
        return _Require(
            false,
            "BasisCurves renderer/AOV",
            "renderer-visible curve shading or authored AOV identity failed");
    }

    // A second pass switches from unlit displayColor to the shared material
    // path. The emissive value comes from the same indexed uniform curve
    // sampler, proving that generated primitive two still evaluates authored
    // curve one in MaterialX.
    settings.maxBounces = 0;
    renderer->SetRenderSettings(settings);
    renderer->SetLightingEnabled(true);
    renderer->SetAovBindings({
        _MakeAovBinding(HdAovTokens->color, &color)});
    renderer->ResetAccumulation();
    renderThread.StartRender();
    renderer->Render(&renderThread);
    renderThread.StopRender();
    GfVec4f materialColor;
    if (!_ReadFirstPixel(&color, &materialColor)) {
        return false;
    }
    return _Require(
        renderer->DidLastFrameProduceValidPixels() &&
            renderer->GetCompletedSamples() == 1 &&
            GfIsClose(
                materialColor,
                GfVec4f(0.2f, 0.2f, 0.2f, 1.0f),
                1.0e-5f),
        "BasisCurves renderer/AOV",
        "MaterialX did not preserve the authored curve uniform domain");
}

bool
TestDiagnosticNames()
{
    struct _NameCase
    {
        ty::CurveTopologyError error;
        char const* name;
    };
    const _NameCase cases[] = {
        {ty::CurveTopologyError::notCanonicalized, "notCanonicalized"},
        {ty::CurveTopologyError::none, "none"},
        {ty::CurveTopologyError::invalidCurveType, "invalidCurveType"},
        {ty::CurveTopologyError::invalidCurveBasis, "invalidCurveBasis"},
        {ty::CurveTopologyError::invalidCurveWrap, "invalidCurveWrap"},
        {ty::CurveTopologyError::invalidTypeBasisWrap,
         "invalidTypeBasisWrap"},
        {ty::CurveTopologyError::nonPositiveCurveVertexCount,
         "nonPositiveCurveVertexCount"},
        {ty::CurveTopologyError::insufficientCurveVertexCount,
         "insufficientCurveVertexCount"},
        {ty::CurveTopologyError::misalignedCurveVertexCount,
         "misalignedCurveVertexCount"},
        {ty::CurveTopologyError::curveVertexCountOverflow,
         "curveVertexCountOverflow"},
        {ty::CurveTopologyError::pointCountMismatch, "pointCountMismatch"},
        {ty::CurveTopologyError::curveIndexCountMismatch,
         "curveIndexCountMismatch"},
        {ty::CurveTopologyError::invalidCurvePointIndex,
         "invalidCurvePointIndex"},
        {ty::CurveTopologyError::domainSizeOverflow, "domainSizeOverflow"},
    };
    for (_NameCase const& testCase : cases) {
        if (std::string(ty::GetCurveTopologyErrorName(testCase.error)) !=
            testCase.name) {
            std::printf(
                "    diagnostic name mismatch for enum value %d\n",
                static_cast<int>(testCase.error));
            return false;
        }
    }
    return std::string(ty::GetCurveTopologyErrorName(
               static_cast<ty::CurveTopologyError>(-1))) == "unknown";
}

bool
TestGeometryDiagnosticNames()
{
    struct _GeometryNameCase
    {
        ty::CurveGeometryError error;
        char const* name;
    };
    const _GeometryNameCase geometryCases[] = {
        {ty::CurveGeometryError::notGenerated, "notGenerated"},
        {ty::CurveGeometryError::none, "none"},
        {ty::CurveGeometryError::invalidTopology, "invalidTopology"},
        {ty::CurveGeometryError::unsupportedPointType,
         "unsupportedPointType"},
        {ty::CurveGeometryError::pointCountMismatch,
         "pointCountMismatch"},
        {ty::CurveGeometryError::invalidPointMapping,
         "invalidPointMapping"},
        {ty::CurveGeometryError::nonFinitePoint, "nonFinitePoint"},
    };
    for (_GeometryNameCase const& testCase : geometryCases) {
        if (std::string(ty::GetCurveGeometryErrorName(testCase.error)) !=
            testCase.name) {
            return false;
        }
    }

    struct _PrimvarNameCase
    {
        ty::CurvePrimvarError error;
        char const* name;
    };
    const _PrimvarNameCase primvarCases[] = {
        {ty::CurvePrimvarError::none, "none"},
        {ty::CurvePrimvarError::unsupportedValueType,
         "unsupportedValueType"},
        {ty::CurvePrimvarError::invalidInterpolation,
         "invalidInterpolation"},
        {ty::CurvePrimvarError::elementCountMismatch,
         "elementCountMismatch"},
        {ty::CurvePrimvarError::indexCountMismatch,
         "indexCountMismatch"},
        {ty::CurvePrimvarError::invalidIndex, "invalidIndex"},
    };
    for (_PrimvarNameCase const& testCase : primvarCases) {
        if (std::string(ty::GetCurvePrimvarErrorName(testCase.error)) !=
            testCase.name) {
            return false;
        }
    }
    const ty::CurveGeometryResult defaultResult;
    return defaultResult.diagnostic.error ==
            ty::CurveGeometryError::notGenerated &&
        defaultResult.primitives.empty() &&
        std::string(ty::GetCurveGeometryErrorName(
            static_cast<ty::CurveGeometryError>(-1))) == "unknown" &&
        std::string(ty::GetCurvePrimvarErrorName(
            static_cast<ty::CurvePrimvarError>(-1))) == "unknown";
}

} // namespace

int
main()
{
    struct _Test
    {
        char const* name;
        bool (*fn)();
    };
    const _Test tests[] = {
        {"BasisCurves.EmptyBatch", &TestEmptyBatch},
        {"BasisCurves.SupportedTopologyMatrix", &TestSupportedTopologyMatrix},
        {"BasisCurves.SchemaLiteralBoundaries", &TestSchemaLiteralBoundaries},
        {"BasisCurves.TokenAndCombinationValidation",
         &TestTokenAndCombinationValidation},
        {"BasisCurves.CountValidationAndAggregation",
         &TestCountValidationAndAggregation},
        {"BasisCurves.LinearAndCubicControlReferences",
         &TestLinearAndCubicControlReferences},
        {"BasisCurves.PinnedPhantomDescriptors",
         &TestPinnedPhantomDescriptors},
        {"BasisCurves.MultiCurveDomainsAndMetadata",
         &TestMultiCurveDomainsAndMetadata},
        {"BasisCurves.IndexedAndUnindexedDomains",
         &TestIndexedAndUnindexedDomains},
        {"BasisCurves.InvisibleCurvesPreserveAuthoredDomains",
         &TestInvisibleCurvesPreserveAuthoredDomains},
        {"BasisCurves.InvisiblePointsUsePhysicalDomain",
         &TestInvisiblePointsUsePhysicalDomain},
        {"BasisCurves.FatalValidationReturnsNoPartialOutput",
         &TestFatalValidationReturnsNoPartialOutput},
        {"BasisCurves.InputBoundaryBehavior", &TestInputBoundaryBehavior},
        {"BasisCurves.DefaultResultIsNotCanonicalized",
         &TestDefaultResultIsNotCanonicalized},
        {"BasisCurves.DiagnosticResultIsPure", &TestDiagnosticResultIsPure},
        {"BasisCurves.DiagnosticNames", &TestDiagnosticNames},
        {"BasisCurves.GeometryPointValidation", &TestGeometryPointValidation},
        {"BasisCurves.NativeAndHermiteBasisAgreement",
         &TestNativeAndHermiteBasisAgreement},
        {"BasisCurves.LinearRibbonIsStraightHermite",
         &TestLinearRibbonIsStraightHermite},
        {"BasisCurves.PinnedGeometryUsesResolvedLogicalControls",
         &TestPinnedGeometryUsesResolvedLogicalControls},
        {"BasisCurves.WidthPrecedenceAndStructuralFallback",
         &TestWidthPrecedenceAndStructuralFallback},
        {"BasisCurves.StructuralPrimvarErrorMatrix",
         &TestStructuralPrimvarErrorMatrix},
        {"BasisCurves.AttributeInterpolationDomains",
         &TestAttributeInterpolationDomains},
        {"BasisCurves.WidthSampleRecovery", &TestWidthSampleRecovery},
        {"BasisCurves.ContinuousMinimumAndZeroWidth",
         &TestContinuousMinimumAndZeroWidth},
        {"BasisCurves.NormalFallbackAndRepair",
         &TestNormalFallbackAndRepair},
        {"BasisCurves.NormalEndpointAndCyclicRepair",
         &TestNormalEndpointAndCyclicRepair},
        {"BasisCurves.HermiteSelectionForVaryingNormal",
         &TestHermiteSelectionForVaryingNormal},
        {"BasisCurves.InteriorNormalDegeneracyRepair",
         &TestInteriorNormalDegeneracyRepair},
        {"BasisCurves.CenterlineDegeneracyFallback",
         &TestCenterlineDegeneracyFallback},
        {"BasisCurves.ScaleInvariantGeometryClassification",
         &TestScaleInvariantGeometryClassification},
        {"BasisCurves.CurveEmbreeRecords", &TestCurveEmbreeRecords},
        {"BasisCurves.CurvePrimvarSamplers", &TestCurvePrimvarSamplers},
        {"BasisCurves.AdvertisedFactoryContract",
         &TestBasisCurvesAdvertisedFactoryContract},
        {"BasisCurves.MinimumWidthSettingRebuild",
         &TestBasisCurvesMinimumWidthSettingRebuild},
        {"BasisCurves.RprimLifecycleAndRecovery",
         &TestBasisCurvesRprimLifecycleAndRecovery},
        {"BasisCurves.RprimOwnsMultipleRecords",
         &TestBasisCurvesRprimOwnsMultipleRecords},
        {"BasisCurves.PointInstancing",
         &TestBasisCurvesPointInstancing},
        {"BasisCurves.DirtyPullAndIndexedTransitions",
         &TestBasisCurvesDirtyPullAndIndexedTransitions},
        {"BasisCurves.TopologyRevalidatesCachedPrimvars",
         &TestBasisCurvesTopologyRevalidatesCachedPrimvars},
        {"BasisCurves.MaterialBindingsAndRegistryRefresh",
         &TestBasisCurvesMaterialBindingsAndRegistryRefresh},
        {"BasisCurves.ComputedPrimvarPrecedenceAndRemoval",
         &TestBasisCurvesComputedPrimvarPrecedenceAndRemoval},
        {"BasisCurves.ComputedGeometrySources",
         &TestBasisCurvesComputedGeometrySources},
        {"BasisCurves.NormalSourcePrecedence",
         &TestBasisCurvesNormalSourcePrecedence},
        {"BasisCurves.LegacyAndSceneIndexDataContract",
         &TestBasisCurvesLegacyAndSceneIndexDataContract},
        {"BasisCurves.RendererAndAovIntegration",
         &TestBasisCurvesRendererAndAovIntegration},
        {"BasisCurves.GeometryDiagnosticNames",
         &TestGeometryDiagnosticNames},
    };

    int failed = 0;
    for (_Test const& test : tests) {
        std::printf("  [RUN ] %s\n", test.name);
        if (test.fn()) {
            std::printf("  [PASS] %s\n", test.name);
        } else {
            std::printf("  [FAIL] %s\n", test.name);
            ++failed;
        }
    }

    if (failed != 0) {
        std::printf(
            "%d/%zu tests failed.\n",
            failed,
            sizeof(tests) / sizeof(tests[0]));
        return 1;
    }

    std::printf(
        "%zu/%zu tests passed.\n",
        sizeof(tests) / sizeof(tests[0]),
        sizeof(tests) / sizeof(tests[0]));
    return 0;
}
