//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_CURVE_GEOMETRY_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_CURVE_GEOMETRY_H

#include "curveTopology.h"

#include <renderer/renderSettings.h>

#include "pxr/base/gf/vec3f.h"
#include "pxr/base/tf/token.h"
#include "pxr/base/vt/array.h"
#include "pxr/base/vt/value.h"
#include "pxr/pxr.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE
namespace ty {

/// Validated interpolation used to evaluate curve geometry attributes.
enum class CurveInterpolation
{
    none,
    constant,
    uniform,
    varying,
    vertex
};

/// Authored source selected before validating its value. An invalid higher-
/// priority authored source never falls through to a lower authored source.
enum class CurveAttributeSource
{
    none,
    primvar,
    builtIn,
    minimum
};

/// Structural validation result for a selected width or normal source.
enum class CurvePrimvarError
{
    none,
    unsupportedValueType,
    invalidInterpolation,
    elementCountMismatch,
    indexCountMismatch,
    invalidIndex
};

/// Fatal geometry-profile validation. Any value other than `none` returns no
/// primitive plans.
enum class CurveGeometryError
{
    notGenerated,
    none,
    invalidTopology,
    unsupportedPointType,
    pointCountMismatch,
    invalidPointMapping,
    nonFinitePoint
};

/// Surface family selected independently for every generated primitive.
enum class CurveGeometryShape
{
    tube,
    ribbon
};

/// Embree-facing representation selected independently of surface family.
enum class CurveGeometryRepresentation
{
    nativeBezier,
    nativeBspline,
    nativeCatmullRom,
    hermite,
    roundLinear,
    spherePoint
};

constexpr size_t CurveGeometryNoIndex = std::numeric_limits<size_t>::max();

/// One possible authored geometry attribute source. `authored` records source
/// presence separately from value validity so precedence survives malformed
/// data. `hasIndices` separately preserves an explicitly empty index source.
struct CurvePrimvarInput
{
    bool authored = false;
    VtValue value;
    TfToken interpolation;
    /// Records an indices data source independently of its array length so an
    /// explicitly empty indexed primvar is not mistaken for an unindexed one.
    bool hasIndices = false;
    VtIntArray indices;
};

/// Hydra data needed by the pure geometry-profile builder. Width and normal
/// source pairs are ordered explicitly so the builder can enforce USD source
/// precedence before structural validation.
struct CurveGeometryInput
{
    VtValue points;
    CurvePrimvarInput primvarWidths;
    CurvePrimvarInput builtInWidths;
    CurvePrimvarInput primvarNormals;
    CurvePrimvarInput builtInNormals;
    /// Object/prototype-space diameter. Radius conversion happens only while
    /// primitive controls are emitted.
    float minimumWidth = DefaultMinimumCurveWidth;
};

/// Selected and effective source for one attribute. Width failures use the
/// minimum source; normal failures use no source and therefore produce tubes.
struct CurveAttributeStatus
{
    CurveAttributeSource selectedSource = CurveAttributeSource::none;
    CurveAttributeSource effectiveSource = CurveAttributeSource::none;
    CurveInterpolation interpolation = CurveInterpolation::none;
    CurvePrimvarError error = CurvePrimvarError::none;
};

/// Aggregated fatal point/topology diagnostic. `elementIndex` names the first
/// offending physical point or logical mapping when applicable.
struct CurveGeometryDiagnostic
{
    CurveGeometryError error = CurveGeometryError::notGenerated;
    size_t elementIndex = CurveGeometryNoIndex;
    size_t affectedCount = 0;
    size_t expectedCount = 0;
    size_t actualCount = 0;
    CurveTopologyError topologyError = CurveTopologyError::none;
};

/// Recoverable input and local-geometry changes. Counts are aggregated per
/// build so downstream code can issue one prim/update diagnostic.
struct CurveGeometryRecoverySummary
{
    size_t missingWidthFallbackCount = 0;
    size_t invalidWidthFallbackCount = 0;
    size_t negativeMinimumWidthCount = 0;
    size_t nonFiniteMinimumWidthCount = 0;
    size_t negativeWidthCount = 0;
    size_t nonFiniteWidthCount = 0;
    size_t invalidNormalSourceCount = 0;
    size_t invalidNormalSampleCount = 0;
    size_t localNormalIssueSpanCount = 0;
    size_t repairedNormalSpanCount = 0;
    size_t tubeFallbackSpanCount = 0;
    size_t linearizedSpanCount = 0;
    size_t removedZeroLengthSegmentCount = 0;
    size_t spherePointSpanCount = 0;
    size_t allEffectiveWidthsZeroCount = 0;
};

/// One generated primitive plan. Data layout depends on `representation`:
///
/// - native cubic: four controls in authored basis order;
/// - Hermite: value0, derivative0, value1, derivative1;
/// - round linear: endpoint0, endpoint1;
/// - sphere point: one value.
///
/// Position, radius, and normal arrays use the same layout. Radius data is in
/// object-space radius units. Normal data is meaningful only for ribbons.
struct CurveGeometryPrimitive
{
    CurveGeometryRepresentation representation =
        CurveGeometryRepresentation::roundLinear;
    CurveGeometryShape shape = CurveGeometryShape::tube;
    std::uint8_t controlCount = 0;
    std::array<GfVec3f, 4> positionData = {
        GfVec3f(0.0f), GfVec3f(0.0f), GfVec3f(0.0f), GfVec3f(0.0f)};
    std::array<float, 4> radiusData = {0.0f, 0.0f, 0.0f, 0.0f};
    std::array<GfVec3f, 4> normalData = {
        GfVec3f(0.0f), GfVec3f(0.0f), GfVec3f(0.0f), GfVec3f(0.0f)};
    CurveSegmentMetadata metadata;
};

/// Complete geometry-profile plan. Fatal input leaves `primitives` empty.
struct CurveGeometryResult
{
    CurveGeometryDiagnostic diagnostic;
    CurveGeometryRecoverySummary recovery;
    CurveAttributeStatus widthStatus;
    CurveAttributeStatus normalStatus;
    float minimumWidth = 0.0f;
    std::vector<CurveGeometryPrimitive> primitives;

    bool IsValid() const
    {
        return diagnostic.error == CurveGeometryError::none;
    }
};

/// Build curve profiles without creating or committing Embree objects.
/// `topology` must be a complete result from CanonicalizeCurveTopology().
/// Referenced points must be finite VtVec3fArray values matching its physical
/// point domain. Recoverable width, normal, and finite centerline problems are
/// represented in the returned plans and recovery summary.
CurveGeometryResult BuildCurveGeometry(
    CurveTopologyResult const& topology,
    CurveGeometryInput const& input);

/// Evaluate generated controls over primitive-local u in [0, 1]. These pure
/// helpers define the numeric contract used by packing and focused tests.
GfVec3f EvaluateCurvePosition(
    CurveGeometryPrimitive const& primitive,
    float u);
GfVec3f EvaluateCurveTangent(
    CurveGeometryPrimitive const& primitive,
    float u);
float EvaluateCurveRadius(
    CurveGeometryPrimitive const& primitive,
    float u);
GfVec3f EvaluateCurveNormal(
    CurveGeometryPrimitive const& primitive,
    float u);

char const* GetCurveGeometryErrorName(CurveGeometryError error) noexcept;
char const* GetCurvePrimvarErrorName(CurvePrimvarError error) noexcept;

} // namespace ty
PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_IMAGING_PLUGIN_HD_EMBREE_CURVE_GEOMETRY_H
