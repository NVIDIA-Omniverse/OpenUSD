//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//

#include "curveGeometry.h"

#include "pxr/base/gf/vec3d.h"
#include "pxr/imaging/hd/primvarSchema.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

constexpr double _UnitEpsilon = 1.0e-10;
constexpr double _RootEpsilon = 1.0e-12;
constexpr double _NormalIndependenceEpsilon = 1.0e-6;
constexpr double _AntipodalThreshold = -0.999;
constexpr size_t _NormalSampleCount = 17;
constexpr size_t _DegeneracySampleCount = 65;
constexpr size_t _MaximumLinearizationDepth = 12;

struct _ScalarPolynomial
{
    double c0 = 0.0;
    double c1 = 0.0;
    double c2 = 0.0;
    double c3 = 0.0;

    double Evaluate(double u) const
    {
        return ((c3 * u + c2) * u + c1) * u + c0;
    }

    double Derivative(double u) const
    {
        return (3.0 * c3 * u + 2.0 * c2) * u + c1;
    }
};

struct _VectorPolynomial
{
    GfVec3d c0 = GfVec3d(0.0);
    GfVec3d c1 = GfVec3d(0.0);
    GfVec3d c2 = GfVec3d(0.0);
    GfVec3d c3 = GfVec3d(0.0);

    GfVec3d Evaluate(double u) const
    {
        return ((c3 * u + c2) * u + c1) * u + c0;
    }

    GfVec3d Derivative(double u) const
    {
        return (3.0 * c3 * u + 2.0 * c2) * u + c1;
    }
};

struct _SelectedSource
{
    ty::CurveAttributeSource kind = ty::CurveAttributeSource::none;
    ty::CurvePrimvarInput const* input = nullptr;
};

struct _ScalarPrimvar
{
    ty::CurvePrimvarError error = ty::CurvePrimvarError::none;
    ty::CurveInterpolation interpolation = ty::CurveInterpolation::none;
    std::vector<double> values;
};

struct _VectorPrimvar
{
    ty::CurvePrimvarError error = ty::CurvePrimvarError::none;
    ty::CurveInterpolation interpolation = ty::CurveInterpolation::none;
    std::vector<GfVec3d> values;
    std::vector<bool> invalidValues;
};

struct _ScalarSpanProfile
{
    _ScalarPolynomial polynomial;
    std::array<double, 4> nativeControls = {0.0, 0.0, 0.0, 0.0};
    std::uint8_t controlCount = 0;
    bool requiresRewrite = false;
};

struct _VectorSpanProfile
{
    _VectorPolynomial polynomial;
    std::array<GfVec3d, 4> nativeControls = {
        GfVec3d(0.0), GfVec3d(0.0), GfVec3d(0.0), GfVec3d(0.0)};
    std::uint8_t controlCount = 0;
    bool usesInvalidValue = false;
};

struct _NormalSample
{
    double authoredParameter = 0.0;
    GfVec3d tangent = GfVec3d(0.0);
    GfVec3d normal = GfVec3d(0.0);
    bool valid = false;
};

struct _PreparedSegment
{
    ty::CanonicalCurveSegment const* segment = nullptr;
    std::array<GfVec3d, 4> positionControls = {
        GfVec3d(0.0), GfVec3d(0.0), GfVec3d(0.0), GfVec3d(0.0)};
    _VectorPolynomial position;
    _ScalarSpanProfile width;
    _VectorSpanProfile normal;
    std::vector<_NormalSample> normalSamples;
    double positionEpsilon = 0.0;
    bool centerlineDegenerate = false;
    bool normalNeedsRepair = false;
    bool useTubeFallback = false;
    bool normalRepaired = false;
    std::array<GfVec3d, 4> repairedNormalData = {
        GfVec3d(0.0), GfVec3d(0.0), GfVec3d(0.0), GfVec3d(0.0)};
};

struct _WidthInterval
{
    double u0 = 0.0;
    double u1 = 1.0;
    bool useMinimum = false;
};

struct _LinearInterval
{
    double u0 = 0.0;
    double u1 = 1.0;
};

double
_ClampUnit(double u)
{
    return std::max(0.0, std::min(1.0, u));
}

bool
_IsFinite(GfVec3f const& value)
{
    return std::isfinite(value[0]) &&
        std::isfinite(value[1]) &&
        std::isfinite(value[2]);
}

bool
_IsFinite(GfVec3d const& value)
{
    return std::isfinite(value[0]) &&
        std::isfinite(value[1]) &&
        std::isfinite(value[2]);
}

GfVec3d
_ToVec3d(GfVec3f const& value)
{
    return GfVec3d(value[0], value[1], value[2]);
}

GfVec3f
_ToVec3f(GfVec3d const& value)
{
    return GfVec3f(
        static_cast<float>(value[0]),
        static_cast<float>(value[1]),
        static_cast<float>(value[2]));
}

double
_Length(GfVec3d const& value)
{
    return std::sqrt(GfDot(value, value));
}

bool
_Normalize(GfVec3d const& value, GfVec3d* outValue)
{
    const double length = _Length(value);
    if (!std::isfinite(length) || length <= 0.0) {
        return false;
    }
    *outValue = value / length;
    return _IsFinite(*outValue);
}

_ScalarPolynomial
_MakeLinearPolynomial(double value0, double value1)
{
    _ScalarPolynomial polynomial;
    polynomial.c0 = value0;
    polynomial.c1 = value1 - value0;
    return polynomial;
}

_VectorPolynomial
_MakeLinearPolynomial(GfVec3d const& value0, GfVec3d const& value1)
{
    _VectorPolynomial polynomial;
    polynomial.c0 = value0;
    polynomial.c1 = value1 - value0;
    return polynomial;
}

_ScalarPolynomial
_MakeCubicPolynomial(
    ty::CurveBasis basis,
    std::array<double, 4> const& control)
{
    _ScalarPolynomial polynomial;
    if (basis == ty::CurveBasis::bezier) {
        polynomial.c0 = control[0];
        polynomial.c1 = 3.0 * (control[1] - control[0]);
        polynomial.c2 =
            3.0 * (control[0] - 2.0 * control[1] + control[2]);
        polynomial.c3 =
            -control[0] + 3.0 * control[1] - 3.0 * control[2] +
            control[3];
    } else if (basis == ty::CurveBasis::bspline) {
        polynomial.c0 =
            (control[0] + 4.0 * control[1] + control[2]) / 6.0;
        polynomial.c1 = (-control[0] + control[2]) / 2.0;
        polynomial.c2 =
            (control[0] - 2.0 * control[1] + control[2]) / 2.0;
        polynomial.c3 =
            (-control[0] + 3.0 * control[1] - 3.0 * control[2] +
             control[3]) / 6.0;
    } else {
        polynomial.c0 = control[1];
        polynomial.c1 = (-control[0] + control[2]) / 2.0;
        polynomial.c2 =
            (2.0 * control[0] - 5.0 * control[1] +
             4.0 * control[2] - control[3]) / 2.0;
        polynomial.c3 =
            (-control[0] + 3.0 * control[1] - 3.0 * control[2] +
             control[3]) / 2.0;
    }
    return polynomial;
}

_VectorPolynomial
_MakeCubicPolynomial(
    ty::CurveBasis basis,
    std::array<GfVec3d, 4> const& control)
{
    _VectorPolynomial polynomial;
    if (basis == ty::CurveBasis::bezier) {
        polynomial.c0 = control[0];
        polynomial.c1 = 3.0 * (control[1] - control[0]);
        polynomial.c2 =
            3.0 * (control[0] - 2.0 * control[1] + control[2]);
        polynomial.c3 =
            -control[0] + 3.0 * control[1] - 3.0 * control[2] +
            control[3];
    } else if (basis == ty::CurveBasis::bspline) {
        polynomial.c0 =
            (control[0] + 4.0 * control[1] + control[2]) / 6.0;
        polynomial.c1 = (-control[0] + control[2]) / 2.0;
        polynomial.c2 =
            (control[0] - 2.0 * control[1] + control[2]) / 2.0;
        polynomial.c3 =
            (-control[0] + 3.0 * control[1] - 3.0 * control[2] +
             control[3]) / 6.0;
    } else {
        polynomial.c0 = control[1];
        polynomial.c1 = (-control[0] + control[2]) / 2.0;
        polynomial.c2 =
            (2.0 * control[0] - 5.0 * control[1] +
             4.0 * control[2] - control[3]) / 2.0;
        polynomial.c3 =
            (-control[0] + 3.0 * control[1] - 3.0 * control[2] +
             control[3]) / 2.0;
    }
    return polynomial;
}

_ScalarPolynomial
_MakeHermitePolynomial(std::array<double, 4> const& data)
{
    _ScalarPolynomial polynomial;
    polynomial.c0 = data[0];
    polynomial.c1 = data[1];
    polynomial.c2 = -3.0 * data[0] - 2.0 * data[1] +
        3.0 * data[2] - data[3];
    polynomial.c3 = 2.0 * data[0] + data[1] -
        2.0 * data[2] + data[3];
    return polynomial;
}

_VectorPolynomial
_MakeHermitePolynomial(std::array<GfVec3d, 4> const& data)
{
    _VectorPolynomial polynomial;
    polynomial.c0 = data[0];
    polynomial.c1 = data[1];
    polynomial.c2 = -3.0 * data[0] - 2.0 * data[1] +
        3.0 * data[2] - data[3];
    polynomial.c3 = 2.0 * data[0] + data[1] -
        2.0 * data[2] + data[3];
    return polynomial;
}

std::array<double, 4>
_MakeHermiteData(
    _ScalarPolynomial const& polynomial,
    double u0,
    double u1)
{
    const double scale = u1 - u0;
    return {
        polynomial.Evaluate(u0),
        polynomial.Derivative(u0) * scale,
        polynomial.Evaluate(u1),
        polynomial.Derivative(u1) * scale};
}

std::array<GfVec3d, 4>
_MakeHermiteData(
    _VectorPolynomial const& polynomial,
    double u0,
    double u1)
{
    const double scale = u1 - u0;
    return {
        polynomial.Evaluate(u0),
        polynomial.Derivative(u0) * scale,
        polynomial.Evaluate(u1),
        polynomial.Derivative(u1) * scale};
}

_SelectedSource
_SelectSource(
    ty::CurvePrimvarInput const& primvar,
    ty::CurvePrimvarInput const& builtIn)
{
    if (primvar.authored) {
        return {ty::CurveAttributeSource::primvar, &primvar};
    }
    if (builtIn.authored) {
        return {ty::CurveAttributeSource::builtIn, &builtIn};
    }
    return {};
}

ty::CurveInterpolation
_ParseInterpolation(TfToken const& token, bool emptyMeansVertex)
{
    if (token.IsEmpty() && emptyMeansVertex) {
        return ty::CurveInterpolation::vertex;
    }
    if (token == HdPrimvarSchemaTokens->constant) {
        return ty::CurveInterpolation::constant;
    }
    if (token == HdPrimvarSchemaTokens->uniform) {
        return ty::CurveInterpolation::uniform;
    }
    if (token == HdPrimvarSchemaTokens->varying) {
        return ty::CurveInterpolation::varying;
    }
    if (token == HdPrimvarSchemaTokens->vertex) {
        return ty::CurveInterpolation::vertex;
    }
    return ty::CurveInterpolation::none;
}

size_t
_GetDomainSize(
    ty::CurveTopologyResult const& topology,
    ty::CurveInterpolation interpolation)
{
    switch (interpolation) {
    case ty::CurveInterpolation::constant:
        return topology.constantDomainSize;
    case ty::CurveInterpolation::uniform:
        return topology.uniformDomainSize;
    case ty::CurveInterpolation::varying:
        return topology.varyingDomainSize;
    case ty::CurveInterpolation::vertex:
        return topology.physicalPointCount;
    case ty::CurveInterpolation::none:
        break;
    }
    return 0;
}

_ScalarPrimvar
_ValidateScalarPrimvar(
    ty::CurvePrimvarInput const& input,
    ty::CurveTopologyResult const& topology,
    double minimumWidth,
    ty::CurveGeometryRecoverySummary* recovery)
{
    _ScalarPrimvar result;
    result.interpolation = _ParseInterpolation(
        input.interpolation, false);
    if (result.interpolation == ty::CurveInterpolation::none) {
        result.error = ty::CurvePrimvarError::invalidInterpolation;
        return result;
    }
    if (!input.value.IsHolding<VtFloatArray>()) {
        result.error = ty::CurvePrimvarError::unsupportedValueType;
        return result;
    }

    const VtFloatArray& authoredValues =
        input.value.UncheckedGet<VtFloatArray>();
    const size_t domainSize = _GetDomainSize(topology, result.interpolation);
    const bool hasIndices = input.hasIndices || !input.indices.empty();
    if (!hasIndices) {
        if (authoredValues.size() != domainSize) {
            result.error = ty::CurvePrimvarError::elementCountMismatch;
            return result;
        }
        result.values.reserve(domainSize);
        for (const float value : authoredValues) {
            if (!std::isfinite(value)) {
                result.values.push_back(minimumWidth);
                ++recovery->nonFiniteWidthCount;
            } else {
                result.values.push_back(static_cast<double>(value));
                if (value < 0.0f) {
                    ++recovery->negativeWidthCount;
                }
            }
        }
        return result;
    }

    if (input.indices.size() != domainSize) {
        result.error = ty::CurvePrimvarError::indexCountMismatch;
        return result;
    }
    for (const int index : input.indices) {
        if (index < 0 || static_cast<size_t>(index) >= authoredValues.size()) {
            result.error = ty::CurvePrimvarError::invalidIndex;
            return result;
        }
    }
    result.values.reserve(domainSize);
    for (const int index : input.indices) {
        const float value = authoredValues[static_cast<size_t>(index)];
        if (!std::isfinite(value)) {
            result.values.push_back(minimumWidth);
            ++recovery->nonFiniteWidthCount;
        } else {
            result.values.push_back(static_cast<double>(value));
            if (value < 0.0f) {
                ++recovery->negativeWidthCount;
            }
        }
    }
    return result;
}

_VectorPrimvar
_ValidateVectorPrimvar(
    ty::CurvePrimvarInput const& input,
    ty::CurveTopologyResult const& topology,
    ty::CurveGeometryRecoverySummary* recovery)
{
    _VectorPrimvar result;
    result.interpolation = _ParseInterpolation(input.interpolation, true);
    if (result.interpolation == ty::CurveInterpolation::none) {
        result.error = ty::CurvePrimvarError::invalidInterpolation;
        return result;
    }
    if (!input.value.IsHolding<VtVec3fArray>()) {
        result.error = ty::CurvePrimvarError::unsupportedValueType;
        return result;
    }

    const VtVec3fArray& authoredValues =
        input.value.UncheckedGet<VtVec3fArray>();
    const size_t domainSize = _GetDomainSize(topology, result.interpolation);
    const bool hasIndices = input.hasIndices || !input.indices.empty();
    if (!hasIndices && authoredValues.size() != domainSize) {
        result.error = ty::CurvePrimvarError::elementCountMismatch;
        return result;
    }
    if (hasIndices && input.indices.size() != domainSize) {
        result.error = ty::CurvePrimvarError::indexCountMismatch;
        return result;
    }

    for (const int index : input.indices) {
        if (index < 0 || static_cast<size_t>(index) >= authoredValues.size()) {
            result.error = ty::CurvePrimvarError::invalidIndex;
            return result;
        }
    }

    result.values.reserve(domainSize);
    result.invalidValues.reserve(domainSize);
    for (size_t domainIndex = 0; domainIndex < domainSize; ++domainIndex) {
        size_t valueIndex = domainIndex;
        if (hasIndices) {
            const int authoredIndex = input.indices[domainIndex];
            valueIndex = static_cast<size_t>(authoredIndex);
        }
        const GfVec3f& authoredValue = authoredValues[valueIndex];
        const GfVec3d value = _ToVec3d(authoredValue);
        const bool invalid = !_IsFinite(authoredValue) ||
            _Length(value) <= 0.0;
        result.values.push_back(invalid ? GfVec3d(0.0) : value);
        result.invalidValues.push_back(invalid);
        if (invalid) {
            ++recovery->invalidNormalSampleCount;
        }
    }
    return result;
}

double
_ResolveScalarControl(
    ty::CurveControlReference const& control,
    ty::CurveTopologyResult const& topology,
    std::vector<double> const& values)
{
    const size_t physicalIndex =
        topology.logicalToPhysicalPointIndices[control.logicalControlIndex];
    const double value = values[physicalIndex];
    if (control.kind == ty::CurveControlKind::authored) {
        return value;
    }
    const size_t neighborIndex = topology.logicalToPhysicalPointIndices[
        control.neighborLogicalControlIndex];
    // This is a transient geometry control derived from vertex interpolation;
    // no value is appended to or requested from the authored domain.
    return 2.0 * value - values[neighborIndex];
}

GfVec3d
_ResolveVectorControl(
    ty::CurveControlReference const& control,
    ty::CurveTopologyResult const& topology,
    std::vector<GfVec3d> const& values)
{
    const size_t physicalIndex =
        topology.logicalToPhysicalPointIndices[control.logicalControlIndex];
    const GfVec3d value = values[physicalIndex];
    if (control.kind == ty::CurveControlKind::authored) {
        return value;
    }
    const size_t neighborIndex = topology.logicalToPhysicalPointIndices[
        control.neighborLogicalControlIndex];
    return 2.0 * value - values[neighborIndex];
}

bool
_ControlUsesInvalidValue(
    ty::CurveControlReference const& control,
    ty::CurveTopologyResult const& topology,
    std::vector<bool> const& invalidValues)
{
    const size_t physicalIndex =
        topology.logicalToPhysicalPointIndices[control.logicalControlIndex];
    if (invalidValues[physicalIndex]) {
        return true;
    }
    if (control.kind == ty::CurveControlKind::authored) {
        return false;
    }
    const size_t neighborIndex = topology.logicalToPhysicalPointIndices[
        control.neighborLogicalControlIndex];
    return invalidValues[neighborIndex];
}

std::array<GfVec3d, 4>
_ResolvePositionControls(
    ty::CanonicalCurveSegment const& segment,
    ty::CurveTopologyResult const& topology,
    VtVec3fArray const& points)
{
    std::array<GfVec3d, 4> result = {
        GfVec3d(0.0), GfVec3d(0.0), GfVec3d(0.0), GfVec3d(0.0)};
    for (std::uint8_t i = 0; i < segment.controlCount; ++i) {
        const ty::CurveControlReference& control = segment.controls[i];
        const size_t physicalIndex = topology.logicalToPhysicalPointIndices[
            control.logicalControlIndex];
        const GfVec3d value = _ToVec3d(points[physicalIndex]);
        if (control.kind == ty::CurveControlKind::authored) {
            result[i] = value;
        } else {
            const size_t neighborIndex =
                topology.logicalToPhysicalPointIndices[
                    control.neighborLogicalControlIndex];
            result[i] = 2.0 * value - _ToVec3d(points[neighborIndex]);
        }
    }
    return result;
}

_ScalarSpanProfile
_MakeScalarSpanProfile(
    ty::CanonicalCurveSegment const& segment,
    ty::CurveTopologyResult const& topology,
    _ScalarPrimvar const& primvar,
    double minimumWidth)
{
    _ScalarSpanProfile result;
    result.controlCount = segment.controlCount;
    const ty::CurveAuthoredDomain& domain =
        topology.curves[segment.metadata.authoredCurveId];

    if (primvar.interpolation == ty::CurveInterpolation::constant ||
        primvar.interpolation == ty::CurveInterpolation::uniform) {
        const size_t valueIndex =
            primvar.interpolation == ty::CurveInterpolation::constant
            ? 0
            : segment.metadata.authoredCurveId;
        result.nativeControls.fill(primvar.values[valueIndex]);
    } else if (primvar.interpolation == ty::CurveInterpolation::varying) {
        const size_t first =
            domain.varyingOffset + segment.metadata.authoredSegmentId;
        const size_t nextLocal =
            segment.metadata.authoredSegmentId + 1 == domain.varyingCount
            ? 0
            : segment.metadata.authoredSegmentId + 1;
        const size_t second = domain.varyingOffset + nextLocal;
        result.nativeControls[0] = primvar.values[first];
        result.nativeControls[1] = primvar.values[second];
        result.polynomial = _MakeLinearPolynomial(
            result.nativeControls[0], result.nativeControls[1]);
        result.requiresRewrite = topology.curveType == ty::CurveType::cubic;
        return result;
    } else {
        for (std::uint8_t i = 0; i < segment.controlCount; ++i) {
            result.nativeControls[i] = _ResolveScalarControl(
                segment.controls[i], topology, primvar.values);
        }
    }

    if (topology.curveType == ty::CurveType::linear) {
        result.polynomial = _MakeLinearPolynomial(
            result.nativeControls[0], result.nativeControls[1]);
    } else {
        result.polynomial = _MakeCubicPolynomial(
            topology.curveBasis, result.nativeControls);
    }
    for (std::uint8_t i = 0; i < segment.controlCount; ++i) {
        if (result.nativeControls[i] < minimumWidth) {
            // Embree radius controls must not contain a value below the hard
            // minimum even when a non-convex basis happens to stay above it.
            result.requiresRewrite = true;
        }
    }
    return result;
}

_VectorSpanProfile
_MakeVectorSpanProfile(
    ty::CanonicalCurveSegment const& segment,
    ty::CurveTopologyResult const& topology,
    _VectorPrimvar const& primvar)
{
    _VectorSpanProfile result;
    result.controlCount = segment.controlCount;
    const ty::CurveAuthoredDomain& domain =
        topology.curves[segment.metadata.authoredCurveId];

    if (primvar.interpolation == ty::CurveInterpolation::constant ||
        primvar.interpolation == ty::CurveInterpolation::uniform) {
        const size_t valueIndex =
            primvar.interpolation == ty::CurveInterpolation::constant
            ? 0
            : segment.metadata.authoredCurveId;
        result.nativeControls.fill(primvar.values[valueIndex]);
        result.usesInvalidValue = primvar.invalidValues[valueIndex];
    } else if (primvar.interpolation == ty::CurveInterpolation::varying) {
        const size_t first =
            domain.varyingOffset + segment.metadata.authoredSegmentId;
        const size_t nextLocal =
            segment.metadata.authoredSegmentId + 1 == domain.varyingCount
            ? 0
            : segment.metadata.authoredSegmentId + 1;
        const size_t second = domain.varyingOffset + nextLocal;
        result.nativeControls[0] = primvar.values[first];
        result.nativeControls[1] = primvar.values[second];
        result.usesInvalidValue =
            primvar.invalidValues[first] || primvar.invalidValues[second];
        result.polynomial = _MakeLinearPolynomial(
            result.nativeControls[0], result.nativeControls[1]);
        return result;
    } else {
        for (std::uint8_t i = 0; i < segment.controlCount; ++i) {
            result.nativeControls[i] = _ResolveVectorControl(
                segment.controls[i], topology, primvar.values);
            result.usesInvalidValue = result.usesInvalidValue ||
                _ControlUsesInvalidValue(
                    segment.controls[i], topology, primvar.invalidValues);
        }
    }

    if (topology.curveType == ty::CurveType::linear) {
        result.polynomial = _MakeLinearPolynomial(
            result.nativeControls[0], result.nativeControls[1]);
    } else {
        result.polynomial = _MakeCubicPolynomial(
            topology.curveBasis, result.nativeControls);
    }
    return result;
}

void
_AppendRoot(double root, std::vector<double>* roots)
{
    if (root <= _RootEpsilon || root >= 1.0 - _RootEpsilon) {
        return;
    }
    for (const double existing : *roots) {
        if (std::abs(existing - root) <= _RootEpsilon) {
            return;
        }
    }
    roots->push_back(root);
}

void
_AppendQuadraticRoots(
    double a,
    double b,
    double c,
    std::vector<double>* roots)
{
    const double coefficientScale = std::max(
        std::abs(a), std::max(std::abs(b), std::abs(c)));
    if (coefficientScale == 0.0) {
        return;
    }
    a /= coefficientScale;
    b /= coefficientScale;
    c /= coefficientScale;
    constexpr double coefficientEpsilon = 1.0e-14;
    if (std::abs(a) <= coefficientEpsilon) {
        if (std::abs(b) > coefficientEpsilon) {
            _AppendRoot(-c / b, roots);
        }
        return;
    }
    const double discriminant = b * b - 4.0 * a * c;
    const double discriminantScale =
        std::abs(b * b) + std::abs(4.0 * a * c);
    const double discriminantEpsilon =
        16.0 * std::numeric_limits<double>::epsilon() *
        discriminantScale;
    if (discriminant < -discriminantEpsilon) {
        return;
    }
    const double rootDiscriminant = std::sqrt(std::max(0.0, discriminant));
    const double q = -0.5 *
        (b + std::copysign(rootDiscriminant, b));
    if (std::abs(q) <= coefficientEpsilon) {
        _AppendRoot(-b / (2.0 * a), roots);
        return;
    }
    _AppendRoot(q / a, roots);
    _AppendRoot(c / q, roots);
}

std::vector<double>
_GetDerivativeRoots(_ScalarPolynomial const& polynomial)
{
    std::vector<double> roots;
    _AppendQuadraticRoots(
        3.0 * polynomial.c3,
        2.0 * polynomial.c2,
        polynomial.c1,
        &roots);
    std::sort(roots.begin(), roots.end());
    return roots;
}

void
_GetScalarRange(
    _ScalarPolynomial const& polynomial,
    double u0,
    double u1,
    double* outMinimum,
    double* outMaximum)
{
    double minimum = std::min(
        polynomial.Evaluate(u0), polynomial.Evaluate(u1));
    double maximum = std::max(
        polynomial.Evaluate(u0), polynomial.Evaluate(u1));
    const std::vector<double> roots = _GetDerivativeRoots(polynomial);
    for (const double root : roots) {
        if (root > u0 && root < u1) {
            const double value = polynomial.Evaluate(root);
            minimum = std::min(minimum, value);
            maximum = std::max(maximum, value);
        }
    }
    *outMinimum = minimum;
    *outMaximum = maximum;
}

std::vector<double>
_FindThresholdRoots(
    _ScalarPolynomial const& polynomial,
    double threshold)
{
    std::vector<double> bounds = _GetDerivativeRoots(polynomial);
    bounds.insert(bounds.begin(), 0.0);
    bounds.push_back(1.0);
    std::vector<double> roots;
    const double coefficientScale = std::max(
        std::abs(polynomial.c0 - threshold),
        std::max(
            std::abs(polynomial.c1),
            std::max(
                std::abs(polynomial.c2),
                std::abs(polynomial.c3))));
    if (coefficientScale == 0.0) {
        return roots;
    }
    const double valueEpsilon = coefficientScale * 1.0e-12;
    for (size_t i = 0; i + 1 < bounds.size(); ++i) {
        double left = bounds[i];
        double right = bounds[i + 1];
        double leftValue = polynomial.Evaluate(left) - threshold;
        double rightValue = polynomial.Evaluate(right) - threshold;
        if (std::abs(leftValue) <= valueEpsilon) {
            _AppendRoot(left, &roots);
        }
        if (std::abs(rightValue) <= valueEpsilon) {
            _AppendRoot(right, &roots);
        }
        if ((leftValue < 0.0 && rightValue > 0.0) ||
            (leftValue > 0.0 && rightValue < 0.0)) {
            for (size_t iteration = 0; iteration < 64; ++iteration) {
                const double middle = (left + right) * 0.5;
                const double middleValue =
                    polynomial.Evaluate(middle) - threshold;
                if (std::abs(middleValue) <= valueEpsilon) {
                    left = middle;
                    right = middle;
                    break;
                }
                if ((leftValue < 0.0 && middleValue > 0.0) ||
                    (leftValue > 0.0 && middleValue < 0.0)) {
                    right = middle;
                    rightValue = middleValue;
                } else {
                    left = middle;
                    leftValue = middleValue;
                }
            }
            _AppendRoot((left + right) * 0.5, &roots);
        }
    }
    std::sort(roots.begin(), roots.end());
    return roots;
}

std::vector<_WidthInterval>
_GetWidthIntervals(
    _ScalarPolynomial const& polynomial,
    double minimumWidth,
    bool* outModified)
{
    double minimum = 0.0;
    double ignoredMaximum = 0.0;
    _GetScalarRange(
        polynomial, 0.0, 1.0, &minimum, &ignoredMaximum);
    if (minimum >= minimumWidth) {
        *outModified = false;
        return {{0.0, 1.0, false}};
    }

    *outModified = true;
    std::vector<double> bounds = _FindThresholdRoots(
        polynomial, minimumWidth);
    bounds.insert(bounds.begin(), 0.0);
    bounds.push_back(1.0);
    std::vector<_WidthInterval> intervals;
    intervals.reserve(bounds.size() - 1);
    for (size_t i = 0; i + 1 < bounds.size(); ++i) {
        const double u0 = bounds[i];
        const double u1 = bounds[i + 1];
        if (u1 - u0 <= _RootEpsilon) {
            continue;
        }
        const double middle = (u0 + u1) * 0.5;
        intervals.push_back(
            {u0, u1, polynomial.Evaluate(middle) < minimumWidth});
    }
    if (intervals.empty()) {
        intervals.push_back({0.0, 1.0, true});
    }
    return intervals;
}

double
_EvaluateEffectiveWidth(
    _ScalarPolynomial const& polynomial,
    double minimumWidth,
    double u)
{
    return std::max(minimumWidth, polynomial.Evaluate(u));
}

double
_GetMaximumEffectiveWidth(
    _ScalarPolynomial const& polynomial,
    double minimumWidth,
    double u0,
    double u1)
{
    double rawMinimum = 0.0;
    double rawMaximum = 0.0;
    _GetScalarRange(
        polynomial, u0, u1, &rawMinimum, &rawMaximum);
    return std::max(minimumWidth, rawMaximum);
}

double
_GetPositionEpsilon(
    std::array<GfVec3d, 4> const& controls,
    std::uint8_t controlCount)
{
    double scale = 0.0;
    for (std::uint8_t i = 0; i < controlCount; ++i) {
        for (std::uint8_t j = i + 1; j < controlCount; ++j) {
            scale = std::max(scale, _Length(controls[j] - controls[i]));
        }
    }
    return scale * 1.0e-7;
}

bool
_HasDegenerateTangent(
    _VectorPolynomial const& position,
    double epsilon)
{
    std::vector<double> candidates;
    for (size_t component = 0; component < 3; ++component) {
        _AppendQuadraticRoots(
            3.0 * position.c3[component],
            2.0 * position.c2[component],
            position.c1[component],
            &candidates);
    }
    candidates.push_back(0.0);
    candidates.push_back(1.0);
    for (size_t i = 1; i + 1 < _DegeneracySampleCount; ++i) {
        candidates.push_back(
            static_cast<double>(i) /
            static_cast<double>(_DegeneracySampleCount - 1));
    }
    for (const double u : candidates) {
        const GfVec3d tangent = position.Derivative(u);
        if (!_IsFinite(tangent) || _Length(tangent) <= epsilon) {
            return true;
        }
    }
    return false;
}

bool
_IsCollapsed(
    _VectorPolynomial const& position,
    double epsilon)
{
    return _Length(position.c1) <= epsilon &&
        _Length(position.c2) <= epsilon &&
        _Length(position.c3) <= epsilon;
}

struct _PowerPolynomial
{
    std::array<double, 6> coefficients = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    size_t degree = 0;
};

double
_EvaluatePowerPolynomial(_PowerPolynomial const& polynomial, double u)
{
    double value = polynomial.coefficients[polynomial.degree];
    for (size_t i = polynomial.degree; i > 0; --i) {
        value = value * u + polynomial.coefficients[i - 1];
    }
    return value;
}

_PowerPolynomial
_GetPowerDerivative(_PowerPolynomial const& polynomial)
{
    _PowerPolynomial result;
    if (polynomial.degree == 0) {
        return result;
    }
    result.degree = polynomial.degree - 1;
    for (size_t i = 1; i <= polynomial.degree; ++i) {
        result.coefficients[i - 1] =
            static_cast<double>(i) * polynomial.coefficients[i];
    }
    return result;
}

void
_AppendParameter(double parameter, std::vector<double>* parameters)
{
    if (parameter < 0.0 || parameter > 1.0) {
        return;
    }
    const double clamped = _ClampUnit(parameter);
    for (const double existing : *parameters) {
        if (std::abs(existing - clamped) <= _RootEpsilon) {
            return;
        }
    }
    parameters->push_back(clamped);
}

std::vector<double>
_FindPowerPolynomialRoots(_PowerPolynomial polynomial)
{
    double coefficientScale = 0.0;
    for (size_t i = 0; i <= polynomial.degree; ++i) {
        coefficientScale = std::max(
            coefficientScale, std::abs(polynomial.coefficients[i]));
    }
    if (coefficientScale == 0.0) {
        return {};
    }
    for (size_t i = 0; i <= polynomial.degree; ++i) {
        polynomial.coefficients[i] /= coefficientScale;
    }
    constexpr double coefficientEpsilon = 1.0e-14;
    while (polynomial.degree > 0 &&
           std::abs(polynomial.coefficients[polynomial.degree]) <=
               coefficientEpsilon) {
        --polynomial.degree;
    }
    if (polynomial.degree == 0) {
        return {};
    }
    if (polynomial.degree == 1) {
        std::vector<double> result;
        _AppendParameter(
            -polynomial.coefficients[0] / polynomial.coefficients[1],
            &result);
        return result;
    }

    std::vector<double> stationary = _FindPowerPolynomialRoots(
        _GetPowerDerivative(polynomial));
    std::sort(stationary.begin(), stationary.end());
    std::vector<double> bounds = stationary;
    bounds.insert(bounds.begin(), 0.0);
    bounds.push_back(1.0);
    std::vector<double> roots;
    constexpr double valueEpsilon = 1.0e-9;
    for (const double point : bounds) {
        if (std::abs(_EvaluatePowerPolynomial(polynomial, point)) <=
            valueEpsilon) {
            _AppendParameter(point, &roots);
        }
    }
    for (size_t i = 0; i + 1 < bounds.size(); ++i) {
        double left = bounds[i];
        double right = bounds[i + 1];
        double leftValue = _EvaluatePowerPolynomial(polynomial, left);
        const double rightValue = _EvaluatePowerPolynomial(polynomial, right);
        if (!((leftValue < 0.0 && rightValue > 0.0) ||
              (leftValue > 0.0 && rightValue < 0.0))) {
            continue;
        }
        for (size_t iteration = 0; iteration < 64; ++iteration) {
            const double middle = (left + right) * 0.5;
            const double middleValue =
                _EvaluatePowerPolynomial(polynomial, middle);
            if (std::abs(middleValue) <= valueEpsilon) {
                left = middle;
                right = middle;
                break;
            }
            if ((leftValue < 0.0 && middleValue > 0.0) ||
                (leftValue > 0.0 && middleValue < 0.0)) {
                right = middle;
            } else {
                left = middle;
                leftValue = middleValue;
            }
        }
        _AppendParameter((left + right) * 0.5, &roots);
    }
    std::sort(roots.begin(), roots.end());
    return roots;
}

std::array<_PowerPolynomial, 3>
_GetNormalTangentCrossPolynomials(
    _VectorPolynomial const& normal,
    _VectorPolynomial const& position)
{
    const std::array<GfVec3d, 4> normalCoefficients = {
        normal.c0, normal.c1, normal.c2, normal.c3};
    const std::array<GfVec3d, 3> tangentCoefficients = {
        position.c1, 2.0 * position.c2, 3.0 * position.c3};
    std::array<GfVec3d, 6> crossCoefficients = {
        GfVec3d(0.0), GfVec3d(0.0), GfVec3d(0.0),
        GfVec3d(0.0), GfVec3d(0.0), GfVec3d(0.0)};
    for (size_t normalDegree = 0;
         normalDegree < normalCoefficients.size();
         ++normalDegree) {
        for (size_t tangentDegree = 0;
             tangentDegree < tangentCoefficients.size();
             ++tangentDegree) {
            crossCoefficients[normalDegree + tangentDegree] += GfCross(
                normalCoefficients[normalDegree],
                tangentCoefficients[tangentDegree]);
        }
    }

    std::array<_PowerPolynomial, 3> result;
    for (size_t component = 0; component < 3; ++component) {
        result[component].degree = 5;
        for (size_t degree = 0; degree <= 5; ++degree) {
            result[component].coefficients[degree] =
                crossCoefficients[degree][component];
        }
    }
    return result;
}

std::vector<double>
_GetNormalValidationParameters(
    _VectorPolynomial const& normal,
    _VectorPolynomial const& position)
{
    std::vector<double> parameters;
    parameters.reserve(_NormalSampleCount + 24);
    for (size_t i = 0; i < _NormalSampleCount; ++i) {
        _AppendParameter(
            static_cast<double>(i) /
                static_cast<double>(_NormalSampleCount - 1),
            &parameters);
    }
    for (size_t component = 0; component < 3; ++component) {
        _ScalarPolynomial componentPolynomial;
        componentPolynomial.c0 = normal.c0[component];
        componentPolynomial.c1 = normal.c1[component];
        componentPolynomial.c2 = normal.c2[component];
        componentPolynomial.c3 = normal.c3[component];
        const std::vector<double> roots = _FindThresholdRoots(
            componentPolynomial, 0.0);
        for (const double root : roots) {
            _AppendParameter(root, &parameters);
        }
    }
    const std::array<_PowerPolynomial, 3> crossPolynomials =
        _GetNormalTangentCrossPolynomials(normal, position);
    for (_PowerPolynomial const& polynomial : crossPolynomials) {
        const std::vector<double> roots =
            _FindPowerPolynomialRoots(polynomial);
        for (const double root : roots) {
            _AppendParameter(root, &parameters);
        }
    }
    std::sort(parameters.begin(), parameters.end());
    return parameters;
}

bool
_ProjectNormal(
    GfVec3d const& value,
    GfVec3d const& tangent,
    GfVec3d* outNormal)
{
    GfVec3d unitTangent(0.0);
    if (!_IsFinite(value) || !_Normalize(tangent, &unitTangent)) {
        return false;
    }
    const double inputLength = _Length(value);
    if (!std::isfinite(inputLength) || inputLength <= 0.0) {
        return false;
    }
    const GfVec3d projected = value - GfDot(value, unitTangent) * unitTangent;
    if (_Length(projected) <= inputLength * _NormalIndependenceEpsilon) {
        return false;
    }
    return _Normalize(projected, outNormal);
}

std::vector<_NormalSample>
_MakeNormalSamples(_PreparedSegment const& prepared)
{
    const std::vector<double> parameters = _GetNormalValidationParameters(
        prepared.normal.polynomial, prepared.position);

    std::vector<_NormalSample> samples;
    samples.reserve(parameters.size());
    for (const double u : parameters) {
        _NormalSample sample;
        sample.authoredParameter =
            static_cast<double>(
                prepared.segment->metadata.authoredSegmentId) + u;
        sample.tangent = prepared.position.Derivative(u);
        sample.valid = _ProjectNormal(
            prepared.normal.polynomial.Evaluate(u),
            sample.tangent,
            &sample.normal);
        samples.push_back(sample);
    }
    return samples;
}

bool
_NormalSamplesNeedRepair(std::vector<_NormalSample> const& samples)
{
    for (const _NormalSample& sample : samples) {
        if (!sample.valid) {
            return true;
        }
    }
    for (size_t i = 1; i < samples.size(); ++i) {
        if (GfDot(samples[i - 1].normal, samples[i].normal) <
            _AntipodalThreshold) {
            return true;
        }
    }
    return false;
}

bool
_TransportNormal(
    GfVec3d const& normal,
    GfVec3d const& tangentFrom,
    GfVec3d const& tangentTo,
    GfVec3d* outNormal)
{
    GfVec3d from(0.0);
    GfVec3d to(0.0);
    GfVec3d sourceNormal(0.0);
    if (!_Normalize(tangentFrom, &from) ||
        !_Normalize(tangentTo, &to) ||
        !_ProjectNormal(normal, tangentFrom, &sourceNormal)) {
        return false;
    }

    const GfVec3d cross = GfCross(from, to);
    const double sine = _Length(cross);
    const double cosine = std::max(-1.0, std::min(1.0, GfDot(from, to)));
    GfVec3d transported = sourceNormal;
    if (sine > _UnitEpsilon) {
        const GfVec3d axis = cross / sine;
        transported = sourceNormal * cosine +
            GfCross(axis, sourceNormal) * sine +
            axis * GfDot(axis, sourceNormal) * (1.0 - cosine);
    }
    return _ProjectNormal(transported, tangentTo, outNormal);
}

struct _NormalAnchorPair
{
    _NormalSample const* left = nullptr;
    _NormalSample const* right = nullptr;
};

_NormalAnchorPair
_FindNormalAnchors(
    std::vector<_NormalSample const*> const& anchors,
    double target,
    bool periodic,
    double period)
{
    _NormalAnchorPair result;
    double leftDistance = std::numeric_limits<double>::infinity();
    double rightDistance = std::numeric_limits<double>::infinity();
    for (_NormalSample const* const anchor : anchors) {
        double backward = target - anchor->authoredParameter;
        double forward = anchor->authoredParameter - target;
        if (periodic && period > 0.0) {
            backward = std::fmod(backward + period, period);
            forward = std::fmod(forward + period, period);
        }
        if ((!periodic && backward >= 0.0) || periodic) {
            if (backward < leftDistance) {
                leftDistance = backward;
                result.left = anchor;
            }
        }
        if ((!periodic && forward >= 0.0) || periodic) {
            if (forward < rightDistance) {
                rightDistance = forward;
                result.right = anchor;
            }
        }
    }
    if (result.left == nullptr && result.right != nullptr) {
        result.left = result.right;
    }
    if (result.right == nullptr && result.left != nullptr) {
        result.right = result.left;
    }
    return result;
}

bool
_BlendTransportedAnchors(
    _NormalAnchorPair const& anchors,
    GfVec3d const& targetTangent,
    GfVec3d* outNormal)
{
    if (anchors.left == nullptr || anchors.right == nullptr) {
        return false;
    }
    GfVec3d left(0.0);
    GfVec3d right(0.0);
    if (!_TransportNormal(
            anchors.left->normal,
            anchors.left->tangent,
            targetTangent,
            &left) ||
        !_TransportNormal(
            anchors.right->normal,
            anchors.right->tangent,
            targetTangent,
            &right)) {
        return false;
    }
    if (anchors.left != anchors.right &&
        GfDot(left, right) <= _AntipodalThreshold) {
        return false;
    }
    return _Normalize(left + right, outNormal);
}

bool
_ValidateNormalPolynomial(
    _VectorPolynomial const& normal,
    _VectorPolynomial const& position)
{
    const std::vector<double> parameters =
        _GetNormalValidationParameters(normal, position);
    GfVec3d previous(0.0);
    bool hasPrevious = false;
    for (const double u : parameters) {
        GfVec3d projected(0.0);
        if (!_ProjectNormal(
                normal.Evaluate(u), position.Derivative(u), &projected)) {
            return false;
        }
        if (hasPrevious &&
            GfDot(previous, projected) <= _AntipodalThreshold) {
            return false;
        }
        previous = projected;
        hasPrevious = true;
    }
    return true;
}

bool
_RepairNormalProfile(
    _PreparedSegment const& prepared,
    std::vector<_NormalSample const*> const& curveAnchors,
    ty::CurveWrap wrap,
    size_t authoredSegmentCount,
    std::array<GfVec3d, 4>* outData)
{
    const double center = static_cast<double>(
        prepared.segment->metadata.authoredSegmentId) + 0.5;
    const _NormalAnchorPair anchors = _FindNormalAnchors(
        curveAnchors,
        center,
        wrap == ty::CurveWrap::periodic,
        static_cast<double>(authoredSegmentCount));
    if (anchors.left == nullptr || anchors.right == nullptr) {
        return false;
    }

    const GfVec3d midpointTangent = prepared.position.Derivative(0.5);
    GfVec3d midpointNormal(0.0);
    if (!_BlendTransportedAnchors(
            anchors, midpointTangent, &midpointNormal)) {
        return false;
    }

    constexpr double derivativeStep = 1.0e-3;
    GfVec3d normal0(0.0);
    GfVec3d normalNear0(0.0);
    GfVec3d normalNear1(0.0);
    GfVec3d normal1(0.0);
    if (!_BlendTransportedAnchors(
            anchors, prepared.position.Derivative(0.0), &normal0) ||
        !_BlendTransportedAnchors(
            anchors,
            prepared.position.Derivative(derivativeStep),
            &normalNear0) ||
        !_BlendTransportedAnchors(
            anchors,
            prepared.position.Derivative(1.0 - derivativeStep),
            &normalNear1) ||
        !_BlendTransportedAnchors(
            anchors, prepared.position.Derivative(1.0), &normal1)) {
        return false;
    }

    (*outData)[0] = normal0;
    (*outData)[1] = (normalNear0 - normal0) / derivativeStep;
    (*outData)[2] = normal1;
    (*outData)[3] = (normal1 - normalNear1) / derivativeStep;
    return _ValidateNormalPolynomial(
        _MakeHermitePolynomial(*outData), prepared.position);
}

double
_GetScalarMagnitudeBound(_ScalarPolynomial const& polynomial)
{
    double minimum = 0.0;
    double maximum = 0.0;
    _GetScalarRange(polynomial, 0.0, 1.0, &minimum, &maximum);
    return std::max(std::abs(minimum), std::abs(maximum));
}

double
_GetPositionLinearizationError(
    _VectorPolynomial const& position,
    double u0,
    double u1)
{
    _VectorPolynomial error = _MakeHermitePolynomial(
        _MakeHermiteData(position, u0, u1));
    const GfVec3d endpoint0 = position.Evaluate(u0);
    const GfVec3d endpoint1 = position.Evaluate(u1);
    error.c0 -= endpoint0;
    error.c1 -= endpoint1 - endpoint0;

    double squaredBound = 0.0;
    for (size_t component = 0; component < 3; ++component) {
        _ScalarPolynomial componentError;
        componentError.c0 = error.c0[component];
        componentError.c1 = error.c1[component];
        componentError.c2 = error.c2[component];
        componentError.c3 = error.c3[component];
        const double componentBound =
            _GetScalarMagnitudeBound(componentError);
        squaredBound += componentBound * componentBound;
    }
    return std::sqrt(squaredBound);
}

double
_GetWidthLinearizationError(
    _ScalarPolynomial const& width,
    double minimumWidth,
    bool useMinimum,
    double u0,
    double u1)
{
    if (useMinimum) {
        return 0.0;
    }
    _ScalarPolynomial error = _MakeHermitePolynomial(
        _MakeHermiteData(width, u0, u1));
    const double endpoint0 =
        _EvaluateEffectiveWidth(width, minimumWidth, u0);
    const double endpoint1 =
        _EvaluateEffectiveWidth(width, minimumWidth, u1);
    error.c0 -= endpoint0;
    error.c1 -= endpoint1 - endpoint0;
    return _GetScalarMagnitudeBound(error);
}

bool
_CanUseLinearApproximation(
    _VectorPolynomial const& position,
    _ScalarPolynomial const& width,
    double minimumWidth,
    bool widthUsesMinimum,
    double u0,
    double u1,
    double positionTolerance,
    double widthTolerance)
{
    return _GetPositionLinearizationError(position, u0, u1) <=
            positionTolerance &&
        _GetWidthLinearizationError(
            width, minimumWidth, widthUsesMinimum, u0, u1) <=
            widthTolerance;
}

void
_SubdivideLinearIntervals(
    _VectorPolynomial const& position,
    _ScalarPolynomial const& width,
    double minimumWidth,
    bool widthUsesMinimum,
    double u0,
    double u1,
    double positionEpsilon,
    double positionTolerance,
    double widthTolerance,
    size_t depth,
    std::vector<_LinearInterval>* intervals,
    size_t* removedZeroLengthCount)
{
    const bool terminate = depth == _MaximumLinearizationDepth ||
        _CanUseLinearApproximation(
            position,
            width,
            minimumWidth,
            widthUsesMinimum,
            u0,
            u1,
            positionTolerance,
            widthTolerance);
    if (!terminate) {
        const double middle = (u0 + u1) * 0.5;
        _SubdivideLinearIntervals(
            position,
            width,
            minimumWidth,
            widthUsesMinimum,
            u0,
            middle,
            positionEpsilon,
            positionTolerance,
            widthTolerance,
            depth + 1,
            intervals,
            removedZeroLengthCount);
        _SubdivideLinearIntervals(
            position,
            width,
            minimumWidth,
            widthUsesMinimum,
            middle,
            u1,
            positionEpsilon,
            positionTolerance,
            widthTolerance,
            depth + 1,
            intervals,
            removedZeroLengthCount);
        return;
    }

    if (_GetMaximumEffectiveWidth(width, minimumWidth, u0, u1) <= 0.0) {
        return;
    }
    if (_Length(position.Evaluate(u1) - position.Evaluate(u0)) <=
        positionEpsilon) {
        ++*removedZeroLengthCount;
        return;
    }
    intervals->push_back({u0, u1});
}

ty::CurveSegmentMetadata
_MakeMetadata(
    ty::CurveSegmentMetadata const& authored,
    double localU0,
    double localU1)
{
    ty::CurveSegmentMetadata result = authored;
    const double authoredRange =
        static_cast<double>(authored.authoredU1 - authored.authoredU0);
    result.authoredU0 = static_cast<float>(
        static_cast<double>(authored.authoredU0) + localU0 * authoredRange);
    result.authoredU1 = static_cast<float>(
        static_cast<double>(authored.authoredU0) + localU1 * authoredRange);
    result.isAuthoredCurveStart = authored.isAuthoredCurveStart &&
        localU0 <= _RootEpsilon;
    result.isAuthoredCurveEnd = authored.isAuthoredCurveEnd &&
        localU1 >= 1.0 - _RootEpsilon;
    return result;
}

ty::CurveGeometryRepresentation
_GetNativeRepresentation(ty::CurveBasis basis)
{
    switch (basis) {
    case ty::CurveBasis::bezier:
        return ty::CurveGeometryRepresentation::nativeBezier;
    case ty::CurveBasis::bspline:
        return ty::CurveGeometryRepresentation::nativeBspline;
    case ty::CurveBasis::catmullRom:
        return ty::CurveGeometryRepresentation::nativeCatmullRom;
    case ty::CurveBasis::none:
        break;
    }
    return ty::CurveGeometryRepresentation::hermite;
}

void
_StorePositionData(
    std::array<GfVec3d, 4> const& source,
    std::array<GfVec3f, 4>* destination)
{
    for (size_t i = 0; i < source.size(); ++i) {
        (*destination)[i] = _ToVec3f(source[i]);
    }
}

void
_StoreRadiusData(
    std::array<double, 4> const& widthData,
    std::array<float, 4>* radiusData)
{
    for (size_t i = 0; i < widthData.size(); ++i) {
        (*radiusData)[i] = static_cast<float>(widthData[i] * 0.5);
    }
}

void
_StoreNormalData(
    std::array<GfVec3d, 4> const& source,
    std::array<GfVec3f, 4>* destination)
{
    for (size_t i = 0; i < source.size(); ++i) {
        (*destination)[i] = _ToVec3f(source[i]);
    }
}

std::array<double, 4>
_MakeRadiusWidthData(
    _ScalarSpanProfile const& width,
    _WidthInterval const& interval,
    double minimumWidth)
{
    if (interval.useMinimum) {
        return {minimumWidth, 0.0, minimumWidth, 0.0};
    }
    std::array<double, 4> data = _MakeHermiteData(
        width.polynomial, interval.u0, interval.u1);
    data[0] = std::max(minimumWidth, data[0]);
    data[2] = std::max(minimumWidth, data[2]);
    return data;
}

std::array<GfVec3d, 4>
_GetNormalHermiteData(
    _PreparedSegment const& prepared,
    double u0,
    double u1)
{
    if (prepared.normalRepaired) {
        return _MakeHermiteData(
            _MakeHermitePolynomial(prepared.repairedNormalData), u0, u1);
    }
    return _MakeHermiteData(prepared.normal.polynomial, u0, u1);
}

ty::CurveGeometryPrimitive
_MakeNativePrimitive(
    _PreparedSegment const& prepared,
    ty::CurveBasis basis,
    bool ribbon)
{
    ty::CurveGeometryPrimitive primitive;
    primitive.representation = _GetNativeRepresentation(basis);
    primitive.shape = ribbon
        ? ty::CurveGeometryShape::ribbon
        : ty::CurveGeometryShape::tube;
    primitive.controlCount = 4;
    _StorePositionData(prepared.positionControls, &primitive.positionData);
    _StoreRadiusData(prepared.width.nativeControls, &primitive.radiusData);
    if (ribbon) {
        _StoreNormalData(
            prepared.normal.nativeControls, &primitive.normalData);
    }
    primitive.metadata = prepared.segment->metadata;
    return primitive;
}

ty::CurveGeometryPrimitive
_MakeHermitePrimitive(
    _PreparedSegment const& prepared,
    _WidthInterval const& interval,
    double minimumWidth,
    bool ribbon)
{
    ty::CurveGeometryPrimitive primitive;
    primitive.representation = ty::CurveGeometryRepresentation::hermite;
    primitive.shape = ribbon
        ? ty::CurveGeometryShape::ribbon
        : ty::CurveGeometryShape::tube;
    primitive.controlCount = 4;
    _StorePositionData(
        _MakeHermiteData(
            prepared.position, interval.u0, interval.u1),
        &primitive.positionData);
    _StoreRadiusData(
        _MakeRadiusWidthData(prepared.width, interval, minimumWidth),
        &primitive.radiusData);
    if (ribbon) {
        _StoreNormalData(
            _GetNormalHermiteData(
                prepared, interval.u0, interval.u1),
            &primitive.normalData);
    }
    primitive.metadata = _MakeMetadata(
        prepared.segment->metadata, interval.u0, interval.u1);
    return primitive;
}

ty::CurveGeometryPrimitive
_MakeRoundLinearPrimitive(
    _PreparedSegment const& prepared,
    double minimumWidth,
    double u0,
    double u1)
{
    ty::CurveGeometryPrimitive primitive;
    primitive.representation =
        ty::CurveGeometryRepresentation::roundLinear;
    primitive.shape = ty::CurveGeometryShape::tube;
    primitive.controlCount = 2;
    primitive.positionData[0] = _ToVec3f(prepared.position.Evaluate(u0));
    primitive.positionData[1] = _ToVec3f(prepared.position.Evaluate(u1));
    primitive.radiusData[0] = static_cast<float>(
        _EvaluateEffectiveWidth(
            prepared.width.polynomial, minimumWidth, u0) * 0.5);
    primitive.radiusData[1] = static_cast<float>(
        _EvaluateEffectiveWidth(
            prepared.width.polynomial, minimumWidth, u1) * 0.5);
    primitive.metadata = _MakeMetadata(
        prepared.segment->metadata, u0, u1);
    return primitive;
}

ty::CurveGeometryPrimitive
_MakeSpherePrimitive(
    _PreparedSegment const& prepared,
    double minimumWidth)
{
    ty::CurveGeometryPrimitive primitive;
    primitive.representation =
        ty::CurveGeometryRepresentation::spherePoint;
    primitive.shape = ty::CurveGeometryShape::tube;
    primitive.controlCount = 1;
    primitive.positionData[0] = _ToVec3f(prepared.position.Evaluate(0.5));
    primitive.radiusData[0] = static_cast<float>(
        _GetMaximumEffectiveWidth(
            prepared.width.polynomial, minimumWidth, 0.0, 1.0) * 0.5);
    primitive.metadata = prepared.segment->metadata;
    return primitive;
}

bool
_ValidateTopologyMappings(ty::CurveTopologyResult const& topology)
{
    if (topology.logicalToPhysicalPointIndices.size() !=
            topology.logicalControlDomainSize ||
        topology.curves.size() != topology.uniformDomainSize) {
        return false;
    }
    for (const size_t physicalIndex :
         topology.logicalToPhysicalPointIndices) {
        if (physicalIndex >= topology.physicalPointCount) {
            return false;
        }
    }
    for (ty::CanonicalCurveSegment const& segment : topology.segments) {
        if (segment.metadata.authoredCurveId >= topology.curves.size() ||
            (segment.controlCount != 2 && segment.controlCount != 4)) {
            return false;
        }
        for (std::uint8_t i = 0; i < segment.controlCount; ++i) {
            if (segment.controls[i].logicalControlIndex >=
                    topology.logicalControlDomainSize ||
                segment.controls[i].neighborLogicalControlIndex >=
                    topology.logicalControlDomainSize) {
                return false;
            }
        }
    }
    return true;
}

ty::CurveGeometryResult
_GeometryFailure(
    ty::CurveGeometryError error,
    size_t elementIndex = ty::CurveGeometryNoIndex,
    size_t affectedCount = 1)
{
    ty::CurveGeometryResult result;
    result.diagnostic.error = error;
    result.diagnostic.elementIndex = elementIndex;
    result.diagnostic.affectedCount = affectedCount;
    return result;
}

bool
_FitsFloat(GfVec3d const& value)
{
    const double limit = static_cast<double>(
        std::numeric_limits<float>::max());
    return _IsFinite(value) &&
        std::abs(value[0]) <= limit &&
        std::abs(value[1]) <= limit &&
        std::abs(value[2]) <= limit;
}

_VectorPolynomial
_GetPrimitiveVectorPolynomial(
    ty::CurveGeometryPrimitive const& primitive,
    std::array<GfVec3f, 4> const& data)
{
    std::array<GfVec3d, 4> converted = {
        GfVec3d(0.0), GfVec3d(0.0), GfVec3d(0.0), GfVec3d(0.0)};
    for (size_t i = 0; i < converted.size(); ++i) {
        converted[i] = _ToVec3d(data[i]);
    }
    switch (primitive.representation) {
    case ty::CurveGeometryRepresentation::nativeBezier:
        return _MakeCubicPolynomial(ty::CurveBasis::bezier, converted);
    case ty::CurveGeometryRepresentation::nativeBspline:
        return _MakeCubicPolynomial(ty::CurveBasis::bspline, converted);
    case ty::CurveGeometryRepresentation::nativeCatmullRom:
        return _MakeCubicPolynomial(ty::CurveBasis::catmullRom, converted);
    case ty::CurveGeometryRepresentation::hermite:
        return _MakeHermitePolynomial(converted);
    case ty::CurveGeometryRepresentation::roundLinear:
        return _MakeLinearPolynomial(converted[0], converted[1]);
    case ty::CurveGeometryRepresentation::spherePoint:
        return _MakeLinearPolynomial(converted[0], converted[0]);
    }
    return {};
}

_ScalarPolynomial
_GetPrimitiveScalarPolynomial(
    ty::CurveGeometryPrimitive const& primitive,
    std::array<float, 4> const& data)
{
    const std::array<double, 4> converted = {
        static_cast<double>(data[0]),
        static_cast<double>(data[1]),
        static_cast<double>(data[2]),
        static_cast<double>(data[3])};
    switch (primitive.representation) {
    case ty::CurveGeometryRepresentation::nativeBezier:
        return _MakeCubicPolynomial(ty::CurveBasis::bezier, converted);
    case ty::CurveGeometryRepresentation::nativeBspline:
        return _MakeCubicPolynomial(ty::CurveBasis::bspline, converted);
    case ty::CurveGeometryRepresentation::nativeCatmullRom:
        return _MakeCubicPolynomial(ty::CurveBasis::catmullRom, converted);
    case ty::CurveGeometryRepresentation::hermite:
        return _MakeHermitePolynomial(converted);
    case ty::CurveGeometryRepresentation::roundLinear:
        return _MakeLinearPolynomial(converted[0], converted[1]);
    case ty::CurveGeometryRepresentation::spherePoint:
        return _MakeLinearPolynomial(converted[0], converted[0]);
    }
    return {};
}

} // anonymous namespace

ty::CurveGeometryResult
ty::BuildCurveGeometry(
    ty::CurveTopologyResult const& topology,
    ty::CurveGeometryInput const& input)
{
    if (!topology.IsValid()) {
        ty::CurveGeometryResult result = _GeometryFailure(
            ty::CurveGeometryError::invalidTopology);
        result.diagnostic.topologyError = topology.diagnostic.error;
        return result;
    }
    if (!_ValidateTopologyMappings(topology)) {
        return _GeometryFailure(
            ty::CurveGeometryError::invalidPointMapping);
    }
    if (!input.points.IsHolding<VtVec3fArray>()) {
        return _GeometryFailure(
            ty::CurveGeometryError::unsupportedPointType);
    }
    const VtVec3fArray& points = input.points.UncheckedGet<VtVec3fArray>();
    if (points.size() != topology.physicalPointCount) {
        ty::CurveGeometryResult result = _GeometryFailure(
            ty::CurveGeometryError::pointCountMismatch);
        result.diagnostic.expectedCount = topology.physicalPointCount;
        result.diagnostic.actualCount = points.size();
        return result;
    }

    // Validate only points referenced by logical controls. An indexed points
    // array may contain an unrelated, unreferenced suffix or holes.
    std::vector<bool> checkedPoints(points.size(), false);
    size_t firstNonFinite = ty::CurveGeometryNoIndex;
    size_t nonFiniteCount = 0;
    for (const size_t physicalIndex :
         topology.logicalToPhysicalPointIndices) {
        if (checkedPoints[physicalIndex]) {
            continue;
        }
        checkedPoints[physicalIndex] = true;
        if (!_IsFinite(points[physicalIndex])) {
            if (firstNonFinite == ty::CurveGeometryNoIndex) {
                firstNonFinite = physicalIndex;
            }
            ++nonFiniteCount;
        }
    }
    if (nonFiniteCount != 0) {
        return _GeometryFailure(
            ty::CurveGeometryError::nonFinitePoint,
            firstNonFinite,
            nonFiniteCount);
    }

    ty::CurveGeometryResult result;
    result.diagnostic.error = ty::CurveGeometryError::none;
    double minimumWidth = static_cast<double>(input.minimumWidth);
    if (!std::isfinite(minimumWidth)) {
        minimumWidth = 0.0;
        ++result.recovery.nonFiniteMinimumWidthCount;
    } else if (minimumWidth < 0.0) {
        minimumWidth = 0.0;
        ++result.recovery.negativeMinimumWidthCount;
    }
    result.minimumWidth = static_cast<float>(minimumWidth);

    // Select width precedence before validation. Malformed primvars:widths
    // therefore falls to the minimum, never to an authored built-in width.
    const _SelectedSource selectedWidth = _SelectSource(
        input.primvarWidths, input.builtInWidths);
    result.widthStatus.selectedSource = selectedWidth.kind;
    _ScalarPrimvar width;
    if (selectedWidth.input == nullptr) {
        result.widthStatus.effectiveSource =
            ty::CurveAttributeSource::minimum;
        result.widthStatus.interpolation =
            ty::CurveInterpolation::constant;
        width.interpolation = ty::CurveInterpolation::constant;
        width.values.push_back(minimumWidth);
        ++result.recovery.missingWidthFallbackCount;
    } else {
        width = _ValidateScalarPrimvar(
            *selectedWidth.input,
            topology,
            minimumWidth,
            &result.recovery);
        result.widthStatus.interpolation = width.interpolation;
        result.widthStatus.error = width.error;
        if (width.error == ty::CurvePrimvarError::none) {
            result.widthStatus.effectiveSource = selectedWidth.kind;
        } else {
            result.widthStatus.effectiveSource =
                ty::CurveAttributeSource::minimum;
            result.widthStatus.interpolation =
                ty::CurveInterpolation::constant;
            width.interpolation = ty::CurveInterpolation::constant;
            width.values = {minimumWidth};
            ++result.recovery.invalidWidthFallbackCount;
        }
    }

    // Normal precedence is independent of width recovery. A malformed
    // selected normal source disables orientation for the whole prim.
    const _SelectedSource selectedNormal = _SelectSource(
        input.primvarNormals, input.builtInNormals);
    result.normalStatus.selectedSource = selectedNormal.kind;
    _VectorPrimvar normal;
    bool hasNormal = false;
    if (selectedNormal.input != nullptr) {
        normal = _ValidateVectorPrimvar(
            *selectedNormal.input, topology, &result.recovery);
        result.normalStatus.interpolation = normal.interpolation;
        result.normalStatus.error = normal.error;
        if (normal.error == ty::CurvePrimvarError::none) {
            result.normalStatus.effectiveSource = selectedNormal.kind;
            hasNormal = true;
        } else {
            ++result.recovery.invalidNormalSourceCount;
        }
    }

    std::vector<_PreparedSegment> preparedSegments;
    preparedSegments.reserve(topology.segments.size());
    for (ty::CanonicalCurveSegment const& segment : topology.segments) {
        _PreparedSegment prepared;
        prepared.segment = &segment;
        prepared.positionControls = _ResolvePositionControls(
            segment, topology, points);
        for (std::uint8_t i = 0; i < segment.controlCount; ++i) {
            if (!_FitsFloat(prepared.positionControls[i])) {
                return _GeometryFailure(
                    ty::CurveGeometryError::nonFinitePoint,
                    segment.controls[i].logicalControlIndex,
                    1);
            }
        }
        prepared.position = topology.curveType == ty::CurveType::linear
            ? _MakeLinearPolynomial(
                prepared.positionControls[0], prepared.positionControls[1])
            : _MakeCubicPolynomial(
                topology.curveBasis, prepared.positionControls);
        prepared.width = _MakeScalarSpanProfile(
            segment, topology, width, minimumWidth);
        if (hasNormal) {
            prepared.normal = _MakeVectorSpanProfile(
                segment, topology, normal);
        }
        prepared.positionEpsilon = _GetPositionEpsilon(
            prepared.positionControls, segment.controlCount);
        prepared.centerlineDegenerate = _HasDegenerateTangent(
            prepared.position, prepared.positionEpsilon);
        if (hasNormal && !prepared.centerlineDegenerate) {
            prepared.normalSamples = _MakeNormalSamples(prepared);
            prepared.normalNeedsRepair =
                prepared.normal.usesInvalidValue ||
                _NormalSamplesNeedRepair(prepared.normalSamples);
            if (prepared.normalNeedsRepair) {
                ++result.recovery.localNormalIssueSpanCount;
            }
        }
        preparedSegments.push_back(prepared);
    }

    if (hasNormal) {
        std::vector<std::vector<_NormalSample const*>> anchors(
            topology.curves.size());
        for (_PreparedSegment const& prepared : preparedSegments) {
            const size_t curveId =
                prepared.segment->metadata.authoredCurveId;
            for (_NormalSample const& sample : prepared.normalSamples) {
                if (sample.valid) {
                    anchors[curveId].push_back(&sample);
                }
            }
        }
        for (_PreparedSegment& prepared : preparedSegments) {
            if (prepared.centerlineDegenerate ||
                !prepared.normalNeedsRepair) {
                continue;
            }
            const size_t curveId =
                prepared.segment->metadata.authoredCurveId;
            if (_RepairNormalProfile(
                    prepared,
                    anchors[curveId],
                    topology.curveWrap,
                    topology.curves[curveId].authoredSegmentCount,
                    &prepared.repairedNormalData)) {
                prepared.normalRepaired = true;
                ++result.recovery.repairedNormalSpanCount;
            } else {
                prepared.useTubeFallback = true;
                ++result.recovery.tubeFallbackSpanCount;
            }
        }
    }

    bool sawPositiveWidth = false;
    for (_PreparedSegment const& prepared : preparedSegments) {
        bool widthModified = false;
        const std::vector<_WidthInterval> widthIntervals =
            _GetWidthIntervals(
                prepared.width.polynomial,
                minimumWidth,
                &widthModified);
        const double maximumWidth = _GetMaximumEffectiveWidth(
            prepared.width.polynomial, minimumWidth, 0.0, 1.0);
        if (maximumWidth <= 0.0) {
            continue;
        }
        sawPositiveWidth = true;

        if (_IsCollapsed(
                prepared.position, prepared.positionEpsilon)) {
            result.primitives.push_back(
                _MakeSpherePrimitive(prepared, minimumWidth));
            ++result.recovery.spherePointSpanCount;
            continue;
        }

        if (topology.curveType == ty::CurveType::cubic &&
            prepared.centerlineDegenerate) {
            ++result.recovery.linearizedSpanCount;
            std::vector<_LinearInterval> linearIntervals;
            const double positionTolerance =
                prepared.positionEpsilon * 100.0;
            const double widthTolerance = std::max(
                1.0e-8, maximumWidth * 1.0e-5);
            for (_WidthInterval const& widthInterval : widthIntervals) {
                if (_GetMaximumEffectiveWidth(
                        prepared.width.polynomial,
                        minimumWidth,
                        widthInterval.u0,
                        widthInterval.u1) <= 0.0) {
                    continue;
                }
                _SubdivideLinearIntervals(
                    prepared.position,
                    prepared.width.polynomial,
                    minimumWidth,
                    widthInterval.useMinimum,
                    widthInterval.u0,
                    widthInterval.u1,
                    prepared.positionEpsilon,
                    positionTolerance,
                    widthTolerance,
                    0,
                    &linearIntervals,
                    &result.recovery.removedZeroLengthSegmentCount);
            }
            for (_LinearInterval const& interval : linearIntervals) {
                result.primitives.push_back(_MakeRoundLinearPrimitive(
                    prepared,
                    minimumWidth,
                    interval.u0,
                    interval.u1));
            }
            continue;
        }

        const bool ribbon = hasNormal && !prepared.useTubeFallback;
        for (_WidthInterval const& interval : widthIntervals) {
            if (_GetMaximumEffectiveWidth(
                    prepared.width.polynomial,
                    minimumWidth,
                    interval.u0,
                    interval.u1) <= 0.0) {
                continue;
            }
            if (topology.curveType == ty::CurveType::linear) {
                if (ribbon) {
                    result.primitives.push_back(_MakeHermitePrimitive(
                        prepared, interval, minimumWidth, true));
                } else {
                    result.primitives.push_back(_MakeRoundLinearPrimitive(
                        prepared,
                        minimumWidth,
                        interval.u0,
                        interval.u1));
                }
                continue;
            }

            const bool intervalIsWholeSpan =
                interval.u0 <= _RootEpsilon &&
                interval.u1 >= 1.0 - _RootEpsilon;
            const bool forceHermite =
                widthModified ||
                prepared.width.requiresRewrite ||
                width.interpolation == ty::CurveInterpolation::varying ||
                (ribbon &&
                 normal.interpolation == ty::CurveInterpolation::varying) ||
                prepared.normalRepaired ||
                prepared.useTubeFallback ||
                !intervalIsWholeSpan;
            if (forceHermite) {
                result.primitives.push_back(_MakeHermitePrimitive(
                    prepared, interval, minimumWidth, ribbon));
            } else {
                result.primitives.push_back(_MakeNativePrimitive(
                    prepared, topology.curveBasis, ribbon));
            }
        }
    }

    if (!preparedSegments.empty() && !sawPositiveWidth) {
        ++result.recovery.allEffectiveWidthsZeroCount;
    }
    return result;
}

GfVec3f
ty::EvaluateCurvePosition(
    ty::CurveGeometryPrimitive const& primitive,
    float u)
{
    const _VectorPolynomial polynomial = _GetPrimitiveVectorPolynomial(
        primitive, primitive.positionData);
    return _ToVec3f(polynomial.Evaluate(_ClampUnit(u)));
}

GfVec3f
ty::EvaluateCurveTangent(
    ty::CurveGeometryPrimitive const& primitive,
    float u)
{
    const _VectorPolynomial polynomial = _GetPrimitiveVectorPolynomial(
        primitive, primitive.positionData);
    return _ToVec3f(polynomial.Derivative(_ClampUnit(u)));
}

float
ty::EvaluateCurveRadius(
    ty::CurveGeometryPrimitive const& primitive,
    float u)
{
    const _ScalarPolynomial polynomial = _GetPrimitiveScalarPolynomial(
        primitive, primitive.radiusData);
    return static_cast<float>(std::max(0.0, polynomial.Evaluate(
        _ClampUnit(u))));
}

GfVec3f
ty::EvaluateCurveNormal(
    ty::CurveGeometryPrimitive const& primitive,
    float u)
{
    if (primitive.shape != ty::CurveGeometryShape::ribbon) {
        return GfVec3f(0.0f);
    }
    const double localU = _ClampUnit(u);
    const _VectorPolynomial normal = _GetPrimitiveVectorPolynomial(
        primitive, primitive.normalData);
    const _VectorPolynomial position = _GetPrimitiveVectorPolynomial(
        primitive, primitive.positionData);
    GfVec3d projected(0.0);
    if (!_ProjectNormal(
            normal.Evaluate(localU),
            position.Derivative(localU),
            &projected)) {
        return GfVec3f(0.0f);
    }
    return _ToVec3f(projected);
}

char const*
ty::GetCurveGeometryErrorName(ty::CurveGeometryError error) noexcept
{
    switch (error) {
    case ty::CurveGeometryError::notGenerated:
        return "notGenerated";
    case ty::CurveGeometryError::none:
        return "none";
    case ty::CurveGeometryError::invalidTopology:
        return "invalidTopology";
    case ty::CurveGeometryError::unsupportedPointType:
        return "unsupportedPointType";
    case ty::CurveGeometryError::pointCountMismatch:
        return "pointCountMismatch";
    case ty::CurveGeometryError::invalidPointMapping:
        return "invalidPointMapping";
    case ty::CurveGeometryError::nonFinitePoint:
        return "nonFinitePoint";
    }
    return "unknown";
}

char const*
ty::GetCurvePrimvarErrorName(ty::CurvePrimvarError error) noexcept
{
    switch (error) {
    case ty::CurvePrimvarError::none:
        return "none";
    case ty::CurvePrimvarError::unsupportedValueType:
        return "unsupportedValueType";
    case ty::CurvePrimvarError::invalidInterpolation:
        return "invalidInterpolation";
    case ty::CurvePrimvarError::elementCountMismatch:
        return "elementCountMismatch";
    case ty::CurvePrimvarError::indexCountMismatch:
        return "indexCountMismatch";
    case ty::CurvePrimvarError::invalidIndex:
        return "invalidIndex";
    }
    return "unknown";
}

PXR_NAMESPACE_CLOSE_SCOPE
