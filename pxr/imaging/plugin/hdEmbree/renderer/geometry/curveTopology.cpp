//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//

#include "curveTopology.h"

#include "pxr/imaging/hd/tokens.h"

#include <limits>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

bool
_AddChecked(size_t value, size_t* outTotal)
{
    if (value > std::numeric_limits<size_t>::max() - *outTotal) {
        return false;
    }
    *outTotal += value;
    return true;
}

ty::CurveTopologyResult
_Failure(
    ty::CurveTopologyError error,
    size_t elementIndex = ty::CurveTopologyNoIndex,
    size_t affectedCount = 1)
{
    ty::CurveTopologyResult result;
    result.diagnostic.error = error;
    result.diagnostic.elementIndex = elementIndex;
    result.diagnostic.affectedCount = affectedCount;
    return result;
}

ty::CurveControlReference
_AuthoredControl(size_t logicalControlIndex)
{
    return {
        ty::CurveControlKind::authored,
        logicalControlIndex,
        logicalControlIndex};
}

ty::CurveControlReference
_PinnedStartControl(size_t logicalControlOffset)
{
    return {
        ty::CurveControlKind::pinnedStartPhantom,
        logicalControlOffset,
        logicalControlOffset + 1};
}

ty::CurveControlReference
_PinnedEndControl(size_t logicalControlOffset, size_t logicalControlCount)
{
    return {
        ty::CurveControlKind::pinnedEndPhantom,
        logicalControlOffset + logicalControlCount - 1,
        logicalControlOffset + logicalControlCount - 2};
}

bool
_ControlUsesInvisiblePoint(
    ty::CurveControlReference const& control,
    std::vector<size_t> const& logicalToPhysicalPointIndices,
    std::vector<bool> const& invisiblePoints)
{
    const size_t physicalPoint =
        logicalToPhysicalPointIndices[control.logicalControlIndex];
    if (invisiblePoints[physicalPoint]) {
        return true;
    }
    if (control.kind == ty::CurveControlKind::authored) {
        return false;
    }
    const size_t neighborPhysicalPoint =
        logicalToPhysicalPointIndices[control.neighborLogicalControlIndex];
    return invisiblePoints[neighborPhysicalPoint];
}

bool
_SegmentUsesInvisiblePoint(
    ty::CanonicalCurveSegment const& segment,
    std::vector<size_t> const& logicalToPhysicalPointIndices,
    std::vector<bool> const& invisiblePoints)
{
    for (std::uint8_t i = 0; i < segment.controlCount; ++i) {
        if (_ControlUsesInvisiblePoint(
                segment.controls[i],
                logicalToPhysicalPointIndices,
                invisiblePoints)) {
            return true;
        }
    }
    return false;
}

ty::CanonicalCurveSegment
_MakeLinearSegment(
    ty::CurveAuthoredDomain const& domain,
    size_t authoredCurveId,
    size_t authoredSegmentId,
    ty::CurveWrap wrap)
{
    ty::CanonicalCurveSegment segment;
    segment.controlCount = 2;
    const size_t first = domain.logicalControlOffset + authoredSegmentId;
    const size_t second = wrap == ty::CurveWrap::periodic
        ? domain.logicalControlOffset +
            (authoredSegmentId + 1) % domain.logicalControlCount
        : first + 1;
    segment.controls[0] = _AuthoredControl(first);
    segment.controls[1] = _AuthoredControl(second);
    segment.metadata.authoredCurveId = authoredCurveId;
    segment.metadata.authoredSegmentId = authoredSegmentId;
    segment.metadata.isAuthoredCurveStart =
        wrap != ty::CurveWrap::periodic && authoredSegmentId == 0;
    segment.metadata.isAuthoredCurveEnd =
        wrap != ty::CurveWrap::periodic &&
        authoredSegmentId + 1 == domain.authoredSegmentCount;
    return segment;
}

ty::CanonicalCurveSegment
_MakeCubicSegment(
    ty::CurveAuthoredDomain const& domain,
    size_t authoredCurveId,
    size_t authoredSegmentId,
    ty::CurveWrap wrap,
    size_t vstep)
{
    ty::CanonicalCurveSegment segment;
    segment.controlCount = 4;
    const size_t first = authoredSegmentId * vstep;
    for (size_t i = 0; i < segment.controls.size(); ++i) {
        const size_t localControl = wrap == ty::CurveWrap::periodic
            ? (first + i) % domain.logicalControlCount
            : first + i;
        segment.controls[i] = _AuthoredControl(
            domain.logicalControlOffset + localControl);
    }
    segment.metadata.authoredCurveId = authoredCurveId;
    segment.metadata.authoredSegmentId = authoredSegmentId;
    segment.metadata.isAuthoredCurveStart =
        wrap != ty::CurveWrap::periodic && authoredSegmentId == 0;
    segment.metadata.isAuthoredCurveEnd =
        wrap != ty::CurveWrap::periodic &&
        authoredSegmentId + 1 == domain.authoredSegmentCount;
    return segment;
}

ty::CurveControlReference
_GetPinnedExpandedControl(
    ty::CurveAuthoredDomain const& domain,
    size_t expandedControlIndex)
{
    if (expandedControlIndex == 0) {
        return _PinnedStartControl(domain.logicalControlOffset);
    }
    if (expandedControlIndex == domain.logicalControlCount + 1) {
        return _PinnedEndControl(
            domain.logicalControlOffset, domain.logicalControlCount);
    }
    return _AuthoredControl(
        domain.logicalControlOffset + expandedControlIndex - 1);
}

ty::CanonicalCurveSegment
_MakePinnedSegment(
    ty::CurveAuthoredDomain const& domain,
    size_t authoredCurveId,
    size_t authoredSegmentId)
{
    ty::CanonicalCurveSegment segment;
    segment.controlCount = 4;
    for (size_t i = 0; i < segment.controls.size(); ++i) {
        segment.controls[i] = _GetPinnedExpandedControl(
            domain, authoredSegmentId + i);
    }
    segment.metadata.authoredCurveId = authoredCurveId;
    segment.metadata.authoredSegmentId = authoredSegmentId;
    segment.metadata.isAuthoredCurveStart = authoredSegmentId == 0;
    segment.metadata.isAuthoredCurveEnd =
        authoredSegmentId + 1 == domain.authoredSegmentCount;
    return segment;
}

} // anonymous namespace

ty::CurveTopologyResult
ty::CanonicalizeCurveTopology(ty::CurveTopologyInput const& input)
{
    // Normalize the raw Hydra tokens before deriving any topology domains.
    ty::CurveType curveType;
    if (input.curveType == HdTokens->linear) {
        curveType = ty::CurveType::linear;
    } else if (input.curveType == HdTokens->cubic) {
        curveType = ty::CurveType::cubic;
    } else {
        ty::CurveTopologyResult result = _Failure(
            ty::CurveTopologyError::invalidCurveType);
        result.diagnostic.token = input.curveType;
        return result;
    }

    ty::CurveWrap curveWrap;
    if (input.curveWrap == HdTokens->nonperiodic) {
        curveWrap = ty::CurveWrap::nonperiodic;
    } else if (input.curveWrap == HdTokens->periodic) {
        curveWrap = ty::CurveWrap::periodic;
    } else if (input.curveWrap == HdTokens->pinned) {
        curveWrap = ty::CurveWrap::pinned;
    } else {
        ty::CurveTopologyResult result = _Failure(
            ty::CurveTopologyError::invalidCurveWrap);
        result.diagnostic.token = input.curveWrap;
        return result;
    }

    ty::CurveBasis curveBasis = ty::CurveBasis::none;
    size_t vstep = 0;
    if (curveType == ty::CurveType::cubic) {
        if (input.curveBasis == HdTokens->bezier) {
            curveBasis = ty::CurveBasis::bezier;
            vstep = 3;
        } else if (input.curveBasis == HdTokens->bspline) {
            curveBasis = ty::CurveBasis::bspline;
            vstep = 1;
        } else if (input.curveBasis == HdTokens->catmullRom) {
            curveBasis = ty::CurveBasis::catmullRom;
            vstep = 1;
        } else {
            ty::CurveTopologyResult result = _Failure(
                ty::CurveTopologyError::invalidCurveBasis);
            result.diagnostic.token = input.curveBasis;
            return result;
        }
    }

    const bool validPinned =
        curveType == ty::CurveType::cubic &&
        (curveBasis == ty::CurveBasis::bspline ||
         curveBasis == ty::CurveBasis::catmullRom);
    if (curveWrap == ty::CurveWrap::pinned && !validPinned) {
        ty::CurveTopologyResult result = _Failure(
            ty::CurveTopologyError::invalidTypeBasisWrap);
        result.diagnostic.token = input.curveWrap;
        return result;
    }

    // Validate every authored count before allowing one curve to shift the
    // offsets of any curve that follows it.
    size_t nonPositiveCount = 0;
    size_t firstNonPositive = ty::CurveTopologyNoIndex;
    for (size_t curveId = 0;
         curveId < input.curveVertexCounts.size();
         ++curveId) {
        if (input.curveVertexCounts[curveId] <= 0) {
            if (firstNonPositive == ty::CurveTopologyNoIndex) {
                firstNonPositive = curveId;
            }
            ++nonPositiveCount;
        }
    }
    if (nonPositiveCount != 0) {
        ty::CurveTopologyResult result = _Failure(
            ty::CurveTopologyError::nonPositiveCurveVertexCount,
            firstNonPositive,
            nonPositiveCount);
        result.diagnostic.value = input.curveVertexCounts[firstNonPositive];
        return result;
    }

    std::vector<size_t> segmentCounts(input.curveVertexCounts.size());
    size_t insufficientCount = 0;
    size_t firstInsufficient = ty::CurveTopologyNoIndex;
    size_t misalignedCount = 0;
    size_t firstMisaligned = ty::CurveTopologyNoIndex;
    size_t logicalControlCount = 0;
    size_t authoredSegmentCount = 0;
    size_t varyingDomainSize = 0;
    for (size_t curveId = 0;
         curveId < input.curveVertexCounts.size();
         ++curveId) {
        const size_t controlCount = static_cast<size_t>(
            input.curveVertexCounts[curveId]);
        bool insufficient = false;
        bool misaligned = false;
        size_t segmentCount = 0;

        if (curveType == ty::CurveType::linear) {
            if (curveWrap == ty::CurveWrap::nonperiodic) {
                insufficient = controlCount <= 2;
                if (!insufficient) {
                    segmentCount = controlCount - 1;
                }
            } else {
                insufficient = controlCount <= 3;
                if (!insufficient) {
                    segmentCount = controlCount;
                }
            }
        } else if (curveWrap == ty::CurveWrap::pinned) {
            insufficient = controlCount < 2;
            if (!insufficient) {
                segmentCount = controlCount - 1;
            }
        } else if (curveWrap == ty::CurveWrap::periodic) {
            insufficient = controlCount < vstep;
            misaligned = !insufficient && controlCount % vstep != 0;
            if (!insufficient && !misaligned) {
                segmentCount = controlCount / vstep;
            }
        } else {
            insufficient = controlCount < 4;
            misaligned = !insufficient && (controlCount - 4) % vstep != 0;
            if (!insufficient && !misaligned) {
                segmentCount = (controlCount - 4) / vstep + 1;
            }
        }

        if (insufficient) {
            if (firstInsufficient == ty::CurveTopologyNoIndex) {
                firstInsufficient = curveId;
            }
            ++insufficientCount;
            continue;
        }
        if (misaligned) {
            if (firstMisaligned == ty::CurveTopologyNoIndex) {
                firstMisaligned = curveId;
            }
            ++misalignedCount;
            continue;
        }

        segmentCounts[curveId] = segmentCount;
        const size_t varyingCount = curveWrap == ty::CurveWrap::periodic
            ? segmentCount
            : segmentCount + 1;
        if (!_AddChecked(controlCount, &logicalControlCount)) {
            return _Failure(
                ty::CurveTopologyError::curveVertexCountOverflow, curveId);
        }
        if (!_AddChecked(segmentCount, &authoredSegmentCount) ||
            !_AddChecked(varyingCount, &varyingDomainSize)) {
            return _Failure(
                ty::CurveTopologyError::domainSizeOverflow, curveId);
        }
    }

    if (insufficientCount != 0) {
        ty::CurveTopologyResult result = _Failure(
            ty::CurveTopologyError::insufficientCurveVertexCount,
            firstInsufficient,
            insufficientCount);
        result.diagnostic.value = input.curveVertexCounts[firstInsufficient];
        return result;
    }
    if (misalignedCount != 0) {
        ty::CurveTopologyResult result = _Failure(
            ty::CurveTopologyError::misalignedCurveVertexCount,
            firstMisaligned,
            misalignedCount);
        result.diagnostic.value = input.curveVertexCounts[firstMisaligned];
        return result;
    }

    // Topology indices map logical controls into the physical points domain;
    // they are distinct from indices authored on individual primvars.
    if (!input.curveIndices.empty() &&
        input.curveIndices.size() != logicalControlCount) {
        ty::CurveTopologyResult result = _Failure(
            ty::CurveTopologyError::curveIndexCountMismatch);
        result.diagnostic.expectedCount = logicalControlCount;
        result.diagnostic.actualCount = input.curveIndices.size();
        return result;
    }
    if (input.curveIndices.empty() &&
        input.physicalPointCount != logicalControlCount) {
        ty::CurveTopologyResult result = _Failure(
            ty::CurveTopologyError::pointCountMismatch);
        result.diagnostic.expectedCount = logicalControlCount;
        result.diagnostic.actualCount = input.physicalPointCount;
        return result;
    }

    size_t invalidPointIndexCount = 0;
    size_t firstInvalidPointIndex = ty::CurveTopologyNoIndex;
    size_t referencedPointDomainSize = input.curveIndices.empty()
        ? logicalControlCount
        : 0;
    for (size_t i = 0; i < input.curveIndices.size(); ++i) {
        const int pointIndex = input.curveIndices[i];
        if (pointIndex < 0 ||
            static_cast<size_t>(pointIndex) >= input.physicalPointCount) {
            if (firstInvalidPointIndex == ty::CurveTopologyNoIndex) {
                firstInvalidPointIndex = i;
            }
            ++invalidPointIndexCount;
        } else {
            const size_t referencedPointCount =
                static_cast<size_t>(pointIndex) + 1;
            if (referencedPointCount > referencedPointDomainSize) {
                referencedPointDomainSize = referencedPointCount;
            }
        }
    }
    if (invalidPointIndexCount != 0) {
        ty::CurveTopologyResult result = _Failure(
            ty::CurveTopologyError::invalidCurvePointIndex,
            firstInvalidPointIndex,
            invalidPointIndexCount);
        result.diagnostic.value = input.curveIndices[firstInvalidPointIndex];
        return result;
    }

    // Materialize the complete value only after all fatal validation passes.
    // Visibility removes generated spans but never compacts authored domains.
    ty::CurveTopologyResult result;
    result.diagnostic.error = ty::CurveTopologyError::none;
    result.curveType = curveType;
    result.curveBasis = curveBasis;
    result.curveWrap = curveWrap;
    result.vstep = vstep;
    result.logicalControlDomainSize = logicalControlCount;
    result.referencedPointDomainSize = referencedPointDomainSize;
    result.physicalPointCount = input.physicalPointCount;
    result.constantDomainSize = 1;
    result.uniformDomainSize = input.curveVertexCounts.size();
    result.varyingDomainSize = varyingDomainSize;
    result.authoredSegmentCount = authoredSegmentCount;
    result.curves.reserve(input.curveVertexCounts.size());
    result.logicalToPhysicalPointIndices.reserve(logicalControlCount);
    result.segments.reserve(authoredSegmentCount);

    if (input.curveIndices.empty()) {
        for (size_t i = 0; i < logicalControlCount; ++i) {
            result.logicalToPhysicalPointIndices.push_back(i);
        }
    } else {
        for (const int pointIndex : input.curveIndices) {
            result.logicalToPhysicalPointIndices.push_back(
                static_cast<size_t>(pointIndex));
        }
    }

    std::vector<bool> invisibleCurves(input.curveVertexCounts.size(), false);
    for (const int curveId : input.invisibleCurves) {
        if (curveId < 0 ||
            static_cast<size_t>(curveId) >= invisibleCurves.size()) {
            ++result.recovery.ignoredInvisibleCurveCount;
            continue;
        }
        invisibleCurves[static_cast<size_t>(curveId)] = true;
    }
    std::vector<bool> invisiblePoints(input.physicalPointCount, false);
    for (const int pointId : input.invisiblePoints) {
        if (pointId < 0 ||
            static_cast<size_t>(pointId) >= invisiblePoints.size()) {
            ++result.recovery.ignoredInvisiblePointCount;
            continue;
        }
        invisiblePoints[static_cast<size_t>(pointId)] = true;
    }

    size_t logicalControlOffset = 0;
    size_t authoredSegmentOffset = 0;
    size_t varyingOffset = 0;
    for (size_t curveId = 0;
         curveId < input.curveVertexCounts.size();
         ++curveId) {
        const size_t controlCount = static_cast<size_t>(
            input.curveVertexCounts[curveId]);
        const size_t segmentCount = segmentCounts[curveId];
        const size_t varyingCount = curveWrap == ty::CurveWrap::periodic
            ? segmentCount
            : segmentCount + 1;
        const ty::CurveAuthoredDomain domain{
            logicalControlOffset,
            controlCount,
            authoredSegmentOffset,
            segmentCount,
            varyingOffset,
            varyingCount};
        result.curves.push_back(domain);

        if (!invisibleCurves[curveId]) {
            for (size_t segmentId = 0;
                 segmentId < segmentCount;
                 ++segmentId) {
                ty::CanonicalCurveSegment segment;
                if (curveType == ty::CurveType::linear) {
                    segment = _MakeLinearSegment(
                        domain, curveId, segmentId, curveWrap);
                } else if (curveWrap == ty::CurveWrap::pinned) {
                    segment = _MakePinnedSegment(
                        domain, curveId, segmentId);
                } else {
                    segment = _MakeCubicSegment(
                        domain, curveId, segmentId, curveWrap, vstep);
                }
                if (!_SegmentUsesInvisiblePoint(
                        segment,
                        result.logicalToPhysicalPointIndices,
                        invisiblePoints)) {
                    result.segments.push_back(segment);
                }
            }
        }

        logicalControlOffset += controlCount;
        authoredSegmentOffset += segmentCount;
        varyingOffset += varyingCount;
    }

    return result;
}

char const*
ty::GetCurveTopologyErrorName(ty::CurveTopologyError error) noexcept
{
    switch (error) {
    case ty::CurveTopologyError::notCanonicalized:
        return "notCanonicalized";
    case ty::CurveTopologyError::none:
        return "none";
    case ty::CurveTopologyError::invalidCurveType:
        return "invalidCurveType";
    case ty::CurveTopologyError::invalidCurveBasis:
        return "invalidCurveBasis";
    case ty::CurveTopologyError::invalidCurveWrap:
        return "invalidCurveWrap";
    case ty::CurveTopologyError::invalidTypeBasisWrap:
        return "invalidTypeBasisWrap";
    case ty::CurveTopologyError::nonPositiveCurveVertexCount:
        return "nonPositiveCurveVertexCount";
    case ty::CurveTopologyError::insufficientCurveVertexCount:
        return "insufficientCurveVertexCount";
    case ty::CurveTopologyError::misalignedCurveVertexCount:
        return "misalignedCurveVertexCount";
    case ty::CurveTopologyError::curveVertexCountOverflow:
        return "curveVertexCountOverflow";
    case ty::CurveTopologyError::pointCountMismatch:
        return "pointCountMismatch";
    case ty::CurveTopologyError::curveIndexCountMismatch:
        return "curveIndexCountMismatch";
    case ty::CurveTopologyError::invalidCurvePointIndex:
        return "invalidCurvePointIndex";
    case ty::CurveTopologyError::domainSizeOverflow:
        return "domainSizeOverflow";
    }
    return "unknown";
}

PXR_NAMESPACE_CLOSE_SCOPE
