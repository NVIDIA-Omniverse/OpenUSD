//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "curveSamplers.h"

#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/vt/typeHeaders.h"
#include "pxr/base/vt/visitValue.h"
#include "pxr/imaging/hd/primvarSchema.h"
#include "pxr/imaging/hd/vtBufferSource.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

struct _SamplerControl
{
    ty::CurveControlKind kind = ty::CurveControlKind::authored;
    size_t valueIndex = 0;
    size_t neighborValueIndex = 0;
};

struct _SamplerPrimitive
{
    ty::CurveSegmentMetadata metadata;
    std::array<_SamplerControl, 4> controls;
    std::uint8_t controlCount = 0;
    size_t varyingIndex0 = 0;
    size_t varyingIndex1 = 0;
};

ty::CurveInterpolation
_ParseInterpolation(TfToken const& interpolation)
{
    if (interpolation == HdPrimvarSchemaTokens->constant) {
        return ty::CurveInterpolation::constant;
    }
    if (interpolation == HdPrimvarSchemaTokens->uniform) {
        return ty::CurveInterpolation::uniform;
    }
    if (interpolation == HdPrimvarSchemaTokens->varying) {
        return ty::CurveInterpolation::varying;
    }
    if (interpolation == HdPrimvarSchemaTokens->vertex) {
        return ty::CurveInterpolation::vertex;
    }
    return ty::CurveInterpolation::none;
}

size_t
_GetExpectedCount(
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

bool
_CanInterpolate(HdTupleType tupleType)
{
    switch (HdGetComponentType(tupleType.type)) {
    case HdTypeInt8:
    case HdTypeInt16:
    case HdTypeUInt16:
    case HdTypeInt32:
    case HdTypeUInt32:
    case HdTypeFloat:
    case HdTypeDouble:
        return true;
    default:
        return false;
    }
}

struct _FlattenIndexedValue
{
    explicit _FlattenIndexedValue(VtIntArray const& sourceIndices)
        : indices(sourceIndices)
    {
    }

    template <typename T>
    VtValue operator()(VtArray<T> const& source) const
    {
        VtArray<T> flattened(indices.size());
        for (size_t i = 0; i < indices.size(); ++i) {
            flattened[i] = source[static_cast<size_t>(indices[i])];
        }
        return VtValue(std::move(flattened));
    }

    VtValue operator()(VtValue const&) const
    {
        return VtValue();
    }

    VtIntArray const& indices;
};

ty::CurvePrimvarSamplerResult
_Failure(
    ty::CurveSamplerError error,
    size_t elementIndex = ty::CurveSamplerNoIndex,
    size_t affectedCount = 0)
{
    ty::CurvePrimvarSamplerResult result;
    result.diagnostic.error = error;
    result.diagnostic.elementIndex = elementIndex;
    result.diagnostic.affectedCount = affectedCount;
    return result;
}

bool
_GetGlobalSegmentIndex(
    ty::CurveTopologyResult const& topology,
    ty::CurveSegmentMetadata const& metadata,
    size_t* globalSegmentIndex)
{
    if (globalSegmentIndex == nullptr ||
        metadata.authoredCurveId >= topology.curves.size()) {
        return false;
    }
    ty::CurveAuthoredDomain const& domain =
        topology.curves[metadata.authoredCurveId];
    if (metadata.authoredSegmentId >= domain.authoredSegmentCount) {
        return false;
    }
    *globalSegmentIndex =
        domain.authoredSegmentOffset + metadata.authoredSegmentId;
    return *globalSegmentIndex < topology.authoredSegmentCount;
}

bool
_BuildSamplerPrimitives(
    ty::CurveTopologyResult const& topology,
    std::vector<ty::CurveSegmentMetadata> const& primitiveMetadata,
    std::vector<_SamplerPrimitive>* samplerPrimitives)
{
    if (samplerPrimitives == nullptr ||
        topology.logicalToPhysicalPointIndices.size() !=
            topology.logicalControlDomainSize ||
        topology.curves.size() != topology.uniformDomainSize) {
        return false;
    }

    std::vector<ty::CanonicalCurveSegment const*> segmentsById(
        topology.authoredSegmentCount, nullptr);
    for (ty::CanonicalCurveSegment const& segment : topology.segments) {
        size_t globalSegmentIndex = 0;
        if (!_GetGlobalSegmentIndex(
                topology, segment.metadata, &globalSegmentIndex) ||
            segmentsById[globalSegmentIndex] != nullptr) {
            return false;
        }
        segmentsById[globalSegmentIndex] = &segment;
    }

    samplerPrimitives->clear();
    samplerPrimitives->reserve(primitiveMetadata.size());
    for (ty::CurveSegmentMetadata const& metadata : primitiveMetadata) {
        size_t globalSegmentIndex = 0;
        if (!_GetGlobalSegmentIndex(
                topology, metadata, &globalSegmentIndex) ||
            segmentsById[globalSegmentIndex] == nullptr ||
            !std::isfinite(metadata.authoredU0) ||
            !std::isfinite(metadata.authoredU1) ||
            metadata.authoredU0 < 0.0f ||
            metadata.authoredU0 > metadata.authoredU1 ||
            metadata.authoredU1 > 1.0f) {
            return false;
        }

        ty::CanonicalCurveSegment const& segment =
            *segmentsById[globalSegmentIndex];
        if (segment.controlCount != 2 && segment.controlCount != 4) {
            return false;
        }

        _SamplerPrimitive primitive;
        primitive.metadata = metadata;
        primitive.controlCount = segment.controlCount;
        for (std::uint8_t i = 0; i < segment.controlCount; ++i) {
            ty::CurveControlReference const& control = segment.controls[i];
            if (control.logicalControlIndex >=
                    topology.logicalToPhysicalPointIndices.size() ||
                control.neighborLogicalControlIndex >=
                    topology.logicalToPhysicalPointIndices.size()) {
                return false;
            }
            primitive.controls[i].kind = control.kind;
            primitive.controls[i].valueIndex =
                topology.logicalToPhysicalPointIndices[
                    control.logicalControlIndex];
            primitive.controls[i].neighborValueIndex =
                topology.logicalToPhysicalPointIndices[
                    control.neighborLogicalControlIndex];
            if (primitive.controls[i].valueIndex >=
                    topology.physicalPointCount ||
                primitive.controls[i].neighborValueIndex >=
                    topology.physicalPointCount) {
                return false;
            }
        }

        ty::CurveAuthoredDomain const& domain =
            topology.curves[metadata.authoredCurveId];
        if (domain.varyingCount == 0) {
            return false;
        }
        primitive.varyingIndex0 =
            domain.varyingOffset + metadata.authoredSegmentId;
        size_t const nextVarying = topology.curveWrap ==
                ty::CurveWrap::periodic
            ? (metadata.authoredSegmentId + 1) % domain.varyingCount
            : metadata.authoredSegmentId + 1;
        primitive.varyingIndex1 = domain.varyingOffset + nextVarying;
        if (primitive.varyingIndex0 >= topology.varyingDomainSize ||
            primitive.varyingIndex1 >= topology.varyingDomainSize) {
            return false;
        }
        samplerPrimitives->push_back(primitive);
    }
    return true;
}

std::array<float, 4>
_GetControlWeights(
    ty::CurveType curveType,
    ty::CurveBasis curveBasis,
    float u)
{
    if (curveType == ty::CurveType::linear) {
        return {1.0f - u, u, 0.0f, 0.0f};
    }

    float const u2 = u * u;
    float const u3 = u2 * u;
    float const oneMinusU = 1.0f - u;
    if (curveBasis == ty::CurveBasis::bezier) {
        float const oneMinusU2 = oneMinusU * oneMinusU;
        return {
            oneMinusU2 * oneMinusU,
            3.0f * u * oneMinusU2,
            3.0f * u2 * oneMinusU,
            u3};
    }
    if (curveBasis == ty::CurveBasis::bspline) {
        return {
            oneMinusU * oneMinusU * oneMinusU / 6.0f,
            (3.0f * u3 - 6.0f * u2 + 4.0f) / 6.0f,
            (-3.0f * u3 + 3.0f * u2 + 3.0f * u + 1.0f) / 6.0f,
            u3 / 6.0f};
    }
    return {
        (-u3 + 2.0f * u2 - u) * 0.5f,
        (3.0f * u3 - 5.0f * u2 + 2.0f) * 0.5f,
        (-3.0f * u3 + 4.0f * u2 + u) * 0.5f,
        (u3 - u2) * 0.5f};
}

bool
_SampleBufferIndex(
    ty::BufferSampler const& sampler,
    size_t index,
    void* value,
    HdTupleType dataType)
{
    if (index > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    return sampler.Sample(static_cast<int>(index), value, dataType);
}

} // anonymous namespace

struct ty::CurvePrimvarSampler::_Impl
{
    _Impl(
        TfToken const& name,
        VtValue const& value,
        ty::CurveInterpolation sourceInterpolation,
        ty::CurveType sourceCurveType,
        ty::CurveBasis sourceCurveBasis,
        std::vector<_SamplerPrimitive>&& sourcePrimitives)
        : buffer(name, value)
        , sampler(buffer)
        , interpolation(sourceInterpolation)
        , curveType(sourceCurveType)
        , curveBasis(sourceCurveBasis)
        , primitives(std::move(sourcePrimitives))
    {
    }

    HdVtBufferSource const buffer;
    ty::BufferSampler const sampler;
    ty::CurveInterpolation const interpolation;
    ty::CurveType const curveType;
    ty::CurveBasis const curveBasis;
    std::vector<_SamplerPrimitive> const primitives;
};

ty::CurvePrimvarSampler::CurvePrimvarSampler(
    std::unique_ptr<ty::CurvePrimvarSampler::_Impl> implementation)
    : _implementation(std::move(implementation))
{
}

ty::CurvePrimvarSampler::~CurvePrimvarSampler() = default;

bool
ty::CurvePrimvarSampler::Sample(
    unsigned int element,
    float u,
    float,
    void* value,
    HdTupleType dataType) const
{
    if (value == nullptr || !std::isfinite(u) || u < 0.0f || u > 1.0f ||
        element >= _implementation->primitives.size()) {
        return false;
    }

    _SamplerPrimitive const& primitive =
        _implementation->primitives[element];
    float const authoredU = primitive.metadata.authoredU0 +
        u * (primitive.metadata.authoredU1 -
             primitive.metadata.authoredU0);

    if (_implementation->interpolation ==
        ty::CurveInterpolation::constant) {
        return _SampleBufferIndex(
            _implementation->sampler, 0, value, dataType);
    }
    if (_implementation->interpolation ==
        ty::CurveInterpolation::uniform) {
        return _SampleBufferIndex(
            _implementation->sampler,
            primitive.metadata.authoredCurveId,
            value,
            dataType);
    }

    if (_implementation->interpolation ==
        ty::CurveInterpolation::varying) {
        alignas(GfMatrix4d) TypeHelper::PrimvarTypeContainer samples[2];
        if (!_SampleBufferIndex(
                _implementation->sampler,
                primitive.varyingIndex0,
                &samples[0],
                dataType) ||
            !_SampleBufferIndex(
                _implementation->sampler,
                primitive.varyingIndex1,
                &samples[1],
                dataType)) {
            return false;
        }
        void* samplePointers[2] = {&samples[0], &samples[1]};
        float weights[2] = {1.0f - authoredU, authoredU};
        return _Interpolate(
            value, samplePointers, weights, 2, dataType);
    }

    alignas(GfMatrix4d) TypeHelper::PrimvarTypeContainer controls[4];
    for (std::uint8_t i = 0; i < primitive.controlCount; ++i) {
        _SamplerControl const& control = primitive.controls[i];
        if (control.kind == ty::CurveControlKind::authored) {
            if (!_SampleBufferIndex(
                    _implementation->sampler,
                    control.valueIndex,
                    &controls[i],
                    dataType)) {
                return false;
            }
            continue;
        }

        alignas(GfMatrix4d) TypeHelper::PrimvarTypeContainer authored[2];
        if (!_SampleBufferIndex(
                _implementation->sampler,
                control.valueIndex,
                &authored[0],
                dataType) ||
            !_SampleBufferIndex(
                _implementation->sampler,
                control.neighborValueIndex,
                &authored[1],
                dataType)) {
            return false;
        }
        void* authoredPointers[2] = {&authored[0], &authored[1]};
        float phantomWeights[2] = {2.0f, -1.0f};
        if (!_Interpolate(
                &controls[i],
                authoredPointers,
                phantomWeights,
                2,
                dataType)) {
            return false;
        }
    }

    std::array<float, 4> weights = _GetControlWeights(
        _implementation->curveType,
        _implementation->curveBasis,
        authoredU);
    void* controlPointers[4] = {
        &controls[0], &controls[1], &controls[2], &controls[3]};
    return _Interpolate(
        value,
        controlPointers,
        weights.data(),
        primitive.controlCount,
        dataType);
}

bool
ty::DecodeCurveHit(
    std::vector<ty::CurveSegmentMetadata> const& primitiveMetadata,
    unsigned int primitiveId,
    float localU,
    ty::DecodedCurveHit* decoded) noexcept
{
    if (decoded == nullptr || !std::isfinite(localU) ||
        localU < 0.0f || localU > 1.0f ||
        primitiveId >= primitiveMetadata.size()) {
        return false;
    }
    ty::CurveSegmentMetadata const& metadata =
        primitiveMetadata[primitiveId];
    if (!std::isfinite(metadata.authoredU0) ||
        !std::isfinite(metadata.authoredU1) ||
        metadata.authoredU0 < 0.0f ||
        metadata.authoredU0 > metadata.authoredU1 ||
        metadata.authoredU1 > 1.0f) {
        return false;
    }

    ty::DecodedCurveHit result;
    result.authoredCurveId = metadata.authoredCurveId;
    result.authoredSegmentId = metadata.authoredSegmentId;
    result.authoredU = metadata.authoredU0 +
        localU * (metadata.authoredU1 - metadata.authoredU0);
    *decoded = result;
    return true;
}

ty::CurvePrimvarSamplerResult
ty::CreateCurvePrimvarSampler(
    TfToken const& name,
    ty::CurvePrimvarInput const& input,
    ty::CurveTopologyResult const& topology,
    std::vector<ty::CurveSegmentMetadata> const& primitiveMetadata)
{
    if (!topology.IsValid()) {
        return _Failure(ty::CurveSamplerError::invalidTopology);
    }

    ty::CurveInterpolation const interpolation =
        _ParseInterpolation(input.interpolation);
    if (interpolation == ty::CurveInterpolation::none) {
        ty::CurvePrimvarSamplerResult result = _Failure(
            ty::CurveSamplerError::invalidInterpolation);
        result.diagnostic.interpolation = input.interpolation;
        return result;
    }

    HdVtBufferSource source(name, input.value);
    HdTupleType const tupleType = source.GetTupleType();
    if (tupleType.type == HdTypeInvalid || tupleType.count != 1 ||
        ((interpolation == ty::CurveInterpolation::varying ||
          interpolation == ty::CurveInterpolation::vertex) &&
         !_CanInterpolate(tupleType))) {
        return _Failure(ty::CurveSamplerError::unsupportedValueType);
    }

    size_t const expectedCount = _GetExpectedCount(
        topology, interpolation);
    VtValue stableValue = input.value;
    if (input.hasIndices) {
        if (input.indices.size() != expectedCount) {
            ty::CurvePrimvarSamplerResult result = _Failure(
                ty::CurveSamplerError::indexCountMismatch);
            result.diagnostic.expectedCount = expectedCount;
            result.diagnostic.actualCount = input.indices.size();
            return result;
        }

        size_t firstInvalidIndex = ty::CurveSamplerNoIndex;
        size_t invalidIndexCount = 0;
        for (size_t i = 0; i < input.indices.size(); ++i) {
            int const index = input.indices[i];
            if (index < 0 ||
                static_cast<size_t>(index) >= source.GetNumElements()) {
                if (firstInvalidIndex == ty::CurveSamplerNoIndex) {
                    firstInvalidIndex = i;
                }
                ++invalidIndexCount;
            }
        }
        if (invalidIndexCount != 0) {
            ty::CurvePrimvarSamplerResult result = _Failure(
                ty::CurveSamplerError::invalidIndex,
                firstInvalidIndex,
                invalidIndexCount);
            result.diagnostic.value = input.indices[firstInvalidIndex];
            return result;
        }

        stableValue = VtVisitValue(
            input.value, _FlattenIndexedValue(input.indices));
        if (stableValue.IsEmpty()) {
            return _Failure(ty::CurveSamplerError::unsupportedValueType);
        }
        HdVtBufferSource flattenedSource(name, stableValue);
        if (flattenedSource.GetTupleType() != tupleType ||
            flattenedSource.GetNumElements() != expectedCount) {
            return _Failure(ty::CurveSamplerError::unsupportedValueType);
        }
    } else if (source.GetNumElements() != expectedCount) {
        ty::CurvePrimvarSamplerResult result = _Failure(
            ty::CurveSamplerError::elementCountMismatch);
        result.diagnostic.expectedCount = expectedCount;
        result.diagnostic.actualCount = source.GetNumElements();
        return result;
    }

    std::vector<_SamplerPrimitive> samplerPrimitives;
    if (!_BuildSamplerPrimitives(
            topology, primitiveMetadata, &samplerPrimitives)) {
        return _Failure(
            ty::CurveSamplerError::invalidPrimitiveMetadata);
    }

    ty::CurvePrimvarSamplerResult result;
    result.diagnostic.error = ty::CurveSamplerError::none;
    std::unique_ptr<ty::CurvePrimvarSampler::_Impl> implementation =
        std::make_unique<ty::CurvePrimvarSampler::_Impl>(
            name,
            stableValue,
            interpolation,
            topology.curveType,
            topology.curveBasis,
            std::move(samplerPrimitives));
    result.sampler.reset(
        new ty::CurvePrimvarSampler(std::move(implementation)));
    return result;
}

char const*
ty::GetCurveSamplerErrorName(ty::CurveSamplerError error) noexcept
{
    switch (error) {
    case ty::CurveSamplerError::notCreated:
        return "notCreated";
    case ty::CurveSamplerError::none:
        return "none";
    case ty::CurveSamplerError::invalidTopology:
        return "invalidTopology";
    case ty::CurveSamplerError::unsupportedValueType:
        return "unsupportedValueType";
    case ty::CurveSamplerError::invalidInterpolation:
        return "invalidInterpolation";
    case ty::CurveSamplerError::elementCountMismatch:
        return "elementCountMismatch";
    case ty::CurveSamplerError::indexCountMismatch:
        return "indexCountMismatch";
    case ty::CurveSamplerError::invalidIndex:
        return "invalidIndex";
    case ty::CurveSamplerError::invalidPrimitiveMetadata:
        return "invalidPrimitiveMetadata";
    }
    return "unknown";
}

PXR_NAMESPACE_CLOSE_SCOPE
