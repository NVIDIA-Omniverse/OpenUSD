//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_CURVE_TOPOLOGY_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_CURVE_TOPOLOGY_H

#include "pxr/base/tf/token.h"
#include "pxr/base/vt/array.h"
#include "pxr/pxr.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE
namespace ty {

/// Validated BasisCurves type used by renderer geometry code.
enum class CurveType
{
    linear,
    cubic
};

/// Validated cubic basis. Linear curves use `none` because their authored
/// basis token has no effect under the UsdGeomBasisCurves contract.
enum class CurveBasis
{
    none,
    bezier,
    bspline,
    catmullRom
};

/// Validated authored wrap mode.
enum class CurveWrap
{
    nonperiodic,
    periodic,
    pinned
};

/// Operation used to obtain one canonical control value.
enum class CurveControlKind
{
    authored,
    pinnedStartPhantom,
    pinnedEndPhantom
};

/// Result status or fatal validation category. Canonicalization returns no
/// topology data when this is anything other than `none`.
enum class CurveTopologyError
{
    notCanonicalized,
    none,
    invalidCurveType,
    invalidCurveBasis,
    invalidCurveWrap,
    invalidTypeBasisWrap,
    nonPositiveCurveVertexCount,
    insufficientCurveVertexCount,
    misalignedCurveVertexCount,
    curveVertexCountOverflow,
    pointCountMismatch,
    curveIndexCountMismatch,
    invalidCurvePointIndex,
    domainSizeOverflow
};

constexpr size_t CurveTopologyNoIndex = std::numeric_limits<size_t>::max();

/// Raw authored topology inputs. `physicalPointCount` names the points-array
/// domain; `curveVertexCounts` and `curveIndices` name the logical control
/// domain and its optional mapping into that physical domain.
struct CurveTopologyInput
{
    TfToken curveType;
    TfToken curveBasis;
    TfToken curveWrap;
    VtIntArray curveVertexCounts;
    VtIntArray curveIndices;
    VtIntArray invisibleCurves;
    VtIntArray invisiblePoints;
    size_t physicalPointCount = 0;
};

/// One authored or synthetic control reference. A phantom value evaluates as
/// twice the value at `logicalControlIndex` minus the value at
/// `neighborLogicalControlIndex`; both indices name the authored logical-
/// control domain and are resolved through the result's point mapping.
struct CurveControlReference
{
    CurveControlKind kind = CurveControlKind::authored;
    size_t logicalControlIndex = 0;
    size_t neighborLogicalControlIndex = 0;
};

/// Authored identity retained independently of generated primitive order.
/// The U interval is local to one authored segment; an unsplit canonical
/// segment therefore covers `[0, 1]`.
struct CurveSegmentMetadata
{
    size_t authoredCurveId = 0;
    size_t authoredSegmentId = 0;
    float authoredU0 = 0.0f;
    float authoredU1 = 1.0f;
    bool isAuthoredCurveStart = false;
    bool isAuthoredCurveEnd = false;
};

/// One visible canonical segment. Linear segments use the first two control
/// references and cubic segments use all four.
struct CanonicalCurveSegment
{
    std::array<CurveControlReference, 4> controls;
    std::uint8_t controlCount = 0;
    CurveSegmentMetadata metadata;
};

/// Per-authored-curve offsets into logical-control, segment, and varying
/// domains.
struct CurveAuthoredDomain
{
    size_t logicalControlOffset = 0;
    size_t logicalControlCount = 0;
    size_t authoredSegmentOffset = 0;
    size_t authoredSegmentCount = 0;
    size_t varyingOffset = 0;
    size_t varyingCount = 0;
};

/// Aggregated information for one fatal validation category. When applicable,
/// `elementIndex` identifies the first offending input element and
/// `affectedCount` reports how many elements failed the same validation.
struct CurveTopologyDiagnostic
{
    CurveTopologyError error = CurveTopologyError::notCanonicalized;
    size_t elementIndex = CurveTopologyNoIndex;
    size_t affectedCount = 0;
    size_t expectedCount = 0;
    size_t actualCount = 0;
    int value = 0;
    TfToken token;
};

/// Recoverable topological-visibility input ignored to match Hydra's
/// topological visibility behavior. Duplicate valid IDs are idempotent and do
/// not contribute to these counts.
struct CurveTopologyRecoverySummary
{
    size_t ignoredInvisibleCurveCount = 0;
    size_t ignoredInvisiblePointCount = 0;
};

/// Complete validated topology value. On failure only `diagnostic` is set;
/// all canonical arrays and domain sizes remain empty or zero.
struct CurveTopologyResult
{
    CurveTopologyDiagnostic diagnostic;
    CurveTopologyRecoverySummary recovery;
    CurveType curveType = CurveType::linear;
    CurveBasis curveBasis = CurveBasis::none;
    CurveWrap curveWrap = CurveWrap::nonperiodic;
    size_t vstep = 0;
    /// Number of authored control slots described by curveVertexCounts.
    size_t logicalControlDomainSize = 0;
    /// Smallest physical point prefix containing every topology reference.
    /// Indexed vertex buffers may legally contain an unreferenced suffix.
    size_t referencedPointDomainSize = 0;
    /// Size of the authored points array, including unreferenced values.
    size_t physicalPointCount = 0;
    size_t constantDomainSize = 0;
    size_t uniformDomainSize = 0;
    size_t varyingDomainSize = 0;
    size_t authoredSegmentCount = 0;
    std::vector<CurveAuthoredDomain> curves;
    std::vector<size_t> logicalToPhysicalPointIndices;
    std::vector<CanonicalCurveSegment> segments;

    bool IsValid() const
    {
        return diagnostic.error == CurveTopologyError::none;
    }
};

/// Validate authored BasisCurves topology and return a curve-major canonical
/// segment sequence. The function applies UsdGeomBasisCurves type/basis/wrap,
/// segment-count, and varying-domain rules; resolves topology indices without
/// conflating them with primvar indices; and removes topologically invisible
/// spans without renumbering authored identities.
///
/// Invalid input returns a diagnostic-only value and never exposes partially
/// initialized curves, mappings, or segments. Allocation failure propagates.
CurveTopologyResult CanonicalizeCurveTopology(
    CurveTopologyInput const& input);

/// Stable diagnostic category name for downstream, prim-path-aware reporting.
char const* GetCurveTopologyErrorName(CurveTopologyError error) noexcept;

} // namespace ty
PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_IMAGING_PLUGIN_HD_EMBREE_CURVE_TOPOLOGY_H
