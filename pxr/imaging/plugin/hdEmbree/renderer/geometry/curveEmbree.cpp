//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "curveEmbree.h"

#include "intersectionFilter.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

struct ty::CurveGeometryRecordBuilder
{
    static bool PackCubicOrHermite(
        CurveGeometryPrimitive const& primitive,
        CurveGeometryRecordData* record);

    static bool PackRoundLinear(
        CurveTopologyResult const& topology,
        CurveGeometryResult const& geometry,
        std::vector<size_t> const& primitiveIndices,
        CurveGeometryRecordData* record);
};

namespace {

struct _PrimitiveGroup
{
    ty::CurveGeometryShape shape = ty::CurveGeometryShape::tube;
    ty::CurveGeometryRepresentation representation =
        ty::CurveGeometryRepresentation::roundLinear;
    RTCGeometryType geometryType = RTC_GEOMETRY_TYPE_ROUND_LINEAR_CURVE;
    std::vector<size_t> primitiveIndices;
};

ty::CurveGeometryRecordBuildResult
_Failure(
    ty::CurveRecordError error,
    size_t primitiveIndex = ty::CurveRecordNoIndex)
{
    ty::CurveGeometryRecordBuildResult result;
    result.diagnostic.error = error;
    result.diagnostic.primitiveIndex = primitiveIndex;
    return result;
}

GfVec4f
_MakeVertex(GfVec3f const& position, float radius)
{
    return GfVec4f(position[0], position[1], position[2], radius);
}

bool
_MetadataMatchesCanonicalSegment(
    ty::CurveTopologyResult const& topology,
    std::vector<ty::CanonicalCurveSegment const*> const& segmentsById,
    ty::CurveSegmentMetadata const& metadata)
{
    if (metadata.authoredCurveId >= topology.curves.size() ||
        !std::isfinite(metadata.authoredU0) ||
        !std::isfinite(metadata.authoredU1) ||
        metadata.authoredU0 < 0.0f ||
        metadata.authoredU0 > metadata.authoredU1 ||
        metadata.authoredU1 > 1.0f) {
        return false;
    }
    ty::CurveAuthoredDomain const& domain =
        topology.curves[metadata.authoredCurveId];
    if (metadata.authoredSegmentId >= domain.authoredSegmentCount) {
        return false;
    }
    size_t const globalSegmentIndex =
        domain.authoredSegmentOffset + metadata.authoredSegmentId;
    if (globalSegmentIndex >= segmentsById.size() ||
        segmentsById[globalSegmentIndex] == nullptr) {
        return false;
    }
    ty::CurveSegmentMetadata const& canonical =
        segmentsById[globalSegmentIndex]->metadata;
    bool const expectedStart = canonical.isAuthoredCurveStart &&
        metadata.authoredU0 == 0.0f;
    bool const expectedEnd = canonical.isAuthoredCurveEnd &&
        metadata.authoredU1 == 1.0f;
    return metadata.isAuthoredCurveStart == expectedStart &&
        metadata.isAuthoredCurveEnd == expectedEnd;
}

bool
_BuildCanonicalSegmentLookup(
    ty::CurveTopologyResult const& topology,
    std::vector<ty::CanonicalCurveSegment const*>* segmentsById)
{
    if (segmentsById == nullptr ||
        topology.curves.size() != topology.uniformDomainSize ||
        topology.logicalToPhysicalPointIndices.size() !=
            topology.logicalControlDomainSize) {
        return false;
    }
    segmentsById->assign(topology.authoredSegmentCount, nullptr);
    for (ty::CanonicalCurveSegment const& segment : topology.segments) {
        if (segment.metadata.authoredCurveId >= topology.curves.size()) {
            return false;
        }
        ty::CurveAuthoredDomain const& domain =
            topology.curves[segment.metadata.authoredCurveId];
        if (segment.metadata.authoredSegmentId >=
            domain.authoredSegmentCount) {
            return false;
        }
        size_t const index = domain.authoredSegmentOffset +
            segment.metadata.authoredSegmentId;
        if (index >= segmentsById->size() ||
            (*segmentsById)[index] != nullptr) {
            return false;
        }
        (*segmentsById)[index] = &segment;
    }
    return true;
}

bool
_IsExpectedControlCount(
    ty::CurveGeometryPrimitive const& primitive)
{
    switch (primitive.representation) {
    case ty::CurveGeometryRepresentation::nativeBezier:
    case ty::CurveGeometryRepresentation::nativeBspline:
    case ty::CurveGeometryRepresentation::nativeCatmullRom:
    case ty::CurveGeometryRepresentation::hermite:
        return primitive.controlCount == 4;
    case ty::CurveGeometryRepresentation::roundLinear:
        return primitive.controlCount == 2;
    case ty::CurveGeometryRepresentation::spherePoint:
        return primitive.controlCount == 1;
    }
    return false;
}

bool
_IsRepresentationCompatible(
    ty::CurveTopologyResult const& topology,
    ty::CurveGeometryPrimitive const& primitive)
{
    switch (primitive.representation) {
    case ty::CurveGeometryRepresentation::nativeBezier:
        return topology.curveType == ty::CurveType::cubic &&
            topology.curveBasis == ty::CurveBasis::bezier;
    case ty::CurveGeometryRepresentation::nativeBspline:
        return topology.curveType == ty::CurveType::cubic &&
            topology.curveBasis == ty::CurveBasis::bspline;
    case ty::CurveGeometryRepresentation::nativeCatmullRom:
        return topology.curveType == ty::CurveType::cubic &&
            topology.curveBasis == ty::CurveBasis::catmullRom;
    case ty::CurveGeometryRepresentation::hermite:
    case ty::CurveGeometryRepresentation::roundLinear:
    case ty::CurveGeometryRepresentation::spherePoint:
        return true;
    }
    return false;
}

bool
_IsFinite(GfVec3f const& value)
{
    return std::isfinite(value[0]) &&
        std::isfinite(value[1]) &&
        std::isfinite(value[2]);
}

bool
_IsPrimitiveFinite(ty::CurveGeometryPrimitive const& primitive)
{
    for (std::uint8_t i = 0; i < primitive.controlCount; ++i) {
        if (!_IsFinite(primitive.positionData[i]) ||
            !std::isfinite(primitive.radiusData[i])) {
            return false;
        }
        if (primitive.shape == ty::CurveGeometryShape::ribbon &&
            !_IsFinite(primitive.normalData[i])) {
            return false;
        }
    }
    return true;
}

bool
_IsSuccessor(
    ty::CurveTopologyResult const& topology,
    ty::CurveSegmentMetadata const& left,
    ty::CurveSegmentMetadata const& right)
{
    if (left.authoredCurveId != right.authoredCurveId ||
        left.authoredCurveId >= topology.curves.size()) {
        return false;
    }
    if (left.authoredSegmentId == right.authoredSegmentId) {
        return left.authoredU1 == right.authoredU0 &&
            left.authoredU1 < 1.0f;
    }
    if (left.authoredU1 != 1.0f || right.authoredU0 != 0.0f) {
        return false;
    }
    if (right.authoredSegmentId == left.authoredSegmentId + 1) {
        return true;
    }

    ty::CurveAuthoredDomain const& domain =
        topology.curves[left.authoredCurveId];
    return topology.curveWrap == ty::CurveWrap::periodic &&
        left.authoredSegmentId + 1 == domain.authoredSegmentCount &&
        right.authoredSegmentId == 0;
}

bool
_CanAppend(size_t currentSize, size_t additionalSize)
{
    size_t const maximumIndex =
        static_cast<size_t>(std::numeric_limits<unsigned int>::max());
    return additionalSize <= maximumIndex &&
        currentSize <= maximumIndex - additionalSize;
}

} // anonymous namespace

bool
ty::CurveGeometryRecordBuilder::PackCubicOrHermite(
    ty::CurveGeometryPrimitive const& primitive,
    ty::CurveGeometryRecordData* record)
{
    bool const hermite = primitive.representation ==
        ty::CurveGeometryRepresentation::hermite;
    size_t const vertexCount = hermite ? 2 : 4;
    if (!_CanAppend(record->_vertices.size(), vertexCount)) {
        return false;
    }
    unsigned int const firstVertex =
        static_cast<unsigned int>(record->_vertices.size());
    record->_indices.push_back(firstVertex);

    if (hermite) {
        record->_vertices.push_back(_MakeVertex(
            primitive.positionData[0], primitive.radiusData[0]));
        record->_vertices.push_back(_MakeVertex(
            primitive.positionData[2], primitive.radiusData[2]));
        record->_tangents.push_back(_MakeVertex(
            primitive.positionData[1], primitive.radiusData[1]));
        record->_tangents.push_back(_MakeVertex(
            primitive.positionData[3], primitive.radiusData[3]));
        if (primitive.shape == ty::CurveGeometryShape::ribbon) {
            record->_normals.push_back(primitive.normalData[0]);
            record->_normals.push_back(primitive.normalData[2]);
            record->_normalDerivatives.push_back(primitive.normalData[1]);
            record->_normalDerivatives.push_back(primitive.normalData[3]);
        }
        return true;
    }

    for (size_t i = 0; i < 4; ++i) {
        record->_vertices.push_back(_MakeVertex(
            primitive.positionData[i], primitive.radiusData[i]));
        if (primitive.shape == ty::CurveGeometryShape::ribbon) {
            record->_normals.push_back(primitive.normalData[i]);
        }
    }
    return true;
}

bool
ty::CurveGeometryRecordBuilder::PackRoundLinear(
    ty::CurveTopologyResult const& topology,
    ty::CurveGeometryResult const& geometry,
    std::vector<size_t> const& primitiveIndices,
    ty::CurveGeometryRecordData* record)
{
    size_t const primitiveCount = primitiveIndices.size();
    std::vector<size_t> leftNeighbors(
        primitiveCount, ty::CurveRecordNoIndex);
    std::vector<size_t> rightNeighbors(
        primitiveCount, ty::CurveRecordNoIndex);

    for (size_t i = 1; i < primitiveCount; ++i) {
        ty::CurveGeometryPrimitive const& left =
            geometry.primitives[primitiveIndices[i - 1]];
        ty::CurveGeometryPrimitive const& right =
            geometry.primitives[primitiveIndices[i]];
        if (_IsSuccessor(topology, left.metadata, right.metadata)) {
            rightNeighbors[i - 1] = i;
            leftNeighbors[i] = i - 1;
        }
    }

    // Curve-major planning keeps each curve contiguous in a record. Complete
    // the periodic seam explicitly instead of mistaking the record boundary
    // for an authored endpoint.
    size_t first = 0;
    while (first < primitiveCount) {
        size_t last = first;
        size_t const curveId = geometry.primitives[
            primitiveIndices[first]].metadata.authoredCurveId;
        while (last + 1 < primitiveCount &&
               geometry.primitives[primitiveIndices[last + 1]]
                       .metadata.authoredCurveId == curveId) {
            ++last;
        }
        ty::CurveGeometryPrimitive const& lastPrimitive =
            geometry.primitives[primitiveIndices[last]];
        ty::CurveGeometryPrimitive const& firstPrimitive =
            geometry.primitives[primitiveIndices[first]];
        if (topology.curveWrap == ty::CurveWrap::periodic &&
            _IsSuccessor(
                topology,
                lastPrimitive.metadata,
                firstPrimitive.metadata)) {
            rightNeighbors[last] = first;
            leftNeighbors[first] = last;
        }
        first = last + 1;
    }

    for (size_t i = 0; i < primitiveCount; ++i) {
        if (!_CanAppend(record->_vertices.size(), 4)) {
            return false;
        }
        ty::CurveGeometryPrimitive const& primitive =
            geometry.primitives[primitiveIndices[i]];
        GfVec4f const endpoint0 = _MakeVertex(
            primitive.positionData[0], primitive.radiusData[0]);
        GfVec4f const endpoint1 = _MakeVertex(
            primitive.positionData[1], primitive.radiusData[1]);

        GfVec4f leftGuard = endpoint0;
        std::uint32_t flags = 0;
        if (leftNeighbors[i] != ty::CurveRecordNoIndex) {
            ty::CurveGeometryPrimitive const& neighbor =
                geometry.primitives[
                    primitiveIndices[leftNeighbors[i]]];
            leftGuard = _MakeVertex(
                neighbor.positionData[0], neighbor.radiusData[0]);
            flags |= static_cast<std::uint32_t>(
                RTC_CURVE_FLAG_NEIGHBOR_LEFT);
        }

        GfVec4f rightGuard = endpoint1;
        if (rightNeighbors[i] != ty::CurveRecordNoIndex) {
            ty::CurveGeometryPrimitive const& neighbor =
                geometry.primitives[
                    primitiveIndices[rightNeighbors[i]]];
            rightGuard = _MakeVertex(
                neighbor.positionData[1], neighbor.radiusData[1]);
            flags |= static_cast<std::uint32_t>(
                RTC_CURVE_FLAG_NEIGHBOR_RIGHT);
        }

        unsigned int const firstVertex =
            static_cast<unsigned int>(record->_vertices.size());
        record->_vertices.push_back(leftGuard);
        record->_vertices.push_back(endpoint0);
        record->_vertices.push_back(endpoint1);
        record->_vertices.push_back(rightGuard);
        record->_indices.push_back(firstVertex + 1);
        record->_curveFlags.push_back(flags);
    }
    return true;
}

ty::CurveGeometryRecordData::CurveGeometryRecordData(
    ty::CurveGeometryShape shape,
    ty::CurveGeometryRepresentation representation,
    RTCGeometryType geometryType)
    : _shape(shape)
    , _representation(representation)
    , _geometryType(geometryType)
    , _context(std::make_unique<ty::PrototypeContext>())
{
    _context->geometryKind = shape == ty::CurveGeometryShape::ribbon
        ? ty::GeometryKind::orientedRibbon
        : ty::GeometryKind::roundCurve;
    _context->curveRepresentation = representation;
}

ty::CurveGeometryRecordData::~CurveGeometryRecordData() = default;

RTCGeometryType
ty::CurveGeometryRecordData::GetGeometryType() const noexcept
{
    return _geometryType;
}

ty::CurveGeometryShape
ty::CurveGeometryRecordData::GetShape() const noexcept
{
    return _shape;
}

ty::CurveGeometryRepresentation
ty::CurveGeometryRecordData::GetRepresentation() const noexcept
{
    return _representation;
}

size_t
ty::CurveGeometryRecordData::GetPrimitiveCount() const noexcept
{
    return _context->curvePrimitiveMetadata.size();
}

ty::PrototypeContext&
ty::CurveGeometryRecordData::GetContext() noexcept
{
    return *_context;
}

ty::PrototypeContext const&
ty::CurveGeometryRecordData::GetContext() const noexcept
{
    return *_context;
}

std::vector<GfVec4f> const&
ty::CurveGeometryRecordData::GetVertices() const noexcept
{
    return _vertices;
}

std::vector<unsigned int> const&
ty::CurveGeometryRecordData::GetIndices() const noexcept
{
    return _indices;
}

std::vector<GfVec4f> const&
ty::CurveGeometryRecordData::GetTangents() const noexcept
{
    return _tangents;
}

std::vector<GfVec3f> const&
ty::CurveGeometryRecordData::GetNormals() const noexcept
{
    return _normals;
}

std::vector<GfVec3f> const&
ty::CurveGeometryRecordData::GetNormalDerivatives() const noexcept
{
    return _normalDerivatives;
}

std::vector<std::uint32_t> const&
ty::CurveGeometryRecordData::GetCurveFlags() const noexcept
{
    return _curveFlags;
}

bool
ty::GetEmbreeCurveGeometryType(
    ty::CurveGeometryShape shape,
    ty::CurveGeometryRepresentation representation,
    RTCGeometryType* geometryType) noexcept
{
    if (geometryType == nullptr) {
        return false;
    }
    if (shape != ty::CurveGeometryShape::tube &&
        shape != ty::CurveGeometryShape::ribbon) {
        return false;
    }

    if (representation ==
        ty::CurveGeometryRepresentation::spherePoint) {
        if (shape != ty::CurveGeometryShape::tube) {
            return false;
        }
        *geometryType = RTC_GEOMETRY_TYPE_SPHERE_POINT;
        return true;
    }
    if (representation ==
        ty::CurveGeometryRepresentation::roundLinear) {
        if (shape != ty::CurveGeometryShape::tube) {
            return false;
        }
        *geometryType = RTC_GEOMETRY_TYPE_ROUND_LINEAR_CURVE;
        return true;
    }

    bool const ribbon = shape == ty::CurveGeometryShape::ribbon;
    switch (representation) {
    case ty::CurveGeometryRepresentation::nativeBezier:
        *geometryType = ribbon
            ? RTC_GEOMETRY_TYPE_NORMAL_ORIENTED_BEZIER_CURVE
            : RTC_GEOMETRY_TYPE_ROUND_BEZIER_CURVE;
        return true;
    case ty::CurveGeometryRepresentation::nativeBspline:
        *geometryType = ribbon
            ? RTC_GEOMETRY_TYPE_NORMAL_ORIENTED_BSPLINE_CURVE
            : RTC_GEOMETRY_TYPE_ROUND_BSPLINE_CURVE;
        return true;
    case ty::CurveGeometryRepresentation::nativeCatmullRom:
        *geometryType = ribbon
            ? RTC_GEOMETRY_TYPE_NORMAL_ORIENTED_CATMULL_ROM_CURVE
            : RTC_GEOMETRY_TYPE_ROUND_CATMULL_ROM_CURVE;
        return true;
    case ty::CurveGeometryRepresentation::hermite:
        *geometryType = ribbon
            ? RTC_GEOMETRY_TYPE_NORMAL_ORIENTED_HERMITE_CURVE
            : RTC_GEOMETRY_TYPE_ROUND_HERMITE_CURVE;
        return true;
    case ty::CurveGeometryRepresentation::roundLinear:
    case ty::CurveGeometryRepresentation::spherePoint:
        break;
    }
    return false;
}

ty::CurveGeometryRecordBuildResult
ty::BuildCurveGeometryRecords(
    ty::CurveTopologyResult const& topology,
    ty::CurveGeometryResult const& geometry)
{
    if (!topology.IsValid()) {
        return _Failure(ty::CurveRecordError::invalidTopology);
    }
    if (!geometry.IsValid()) {
        return _Failure(ty::CurveRecordError::invalidGeometry);
    }

    std::vector<ty::CanonicalCurveSegment const*> segmentsById;
    if (!_BuildCanonicalSegmentLookup(topology, &segmentsById)) {
        return _Failure(ty::CurveRecordError::invalidTopology);
    }

    std::vector<_PrimitiveGroup> groups;
    for (size_t primitiveIndex = 0;
         primitiveIndex < geometry.primitives.size();
         ++primitiveIndex) {
        ty::CurveGeometryPrimitive const& primitive =
            geometry.primitives[primitiveIndex];
        if (!_IsExpectedControlCount(primitive) ||
            !_IsRepresentationCompatible(topology, primitive) ||
            !_IsPrimitiveFinite(primitive) ||
            !_MetadataMatchesCanonicalSegment(
                topology, segmentsById, primitive.metadata)) {
            return _Failure(
                ty::CurveRecordError::invalidPrimitiveMetadata,
                primitiveIndex);
        }

        RTCGeometryType geometryType;
        if (!ty::GetEmbreeCurveGeometryType(
                primitive.shape,
                primitive.representation,
                &geometryType)) {
            return _Failure(
                ty::CurveRecordError::unsupportedRepresentation,
                primitiveIndex);
        }

        _PrimitiveGroup* group = nullptr;
        for (_PrimitiveGroup& candidate : groups) {
            if (candidate.shape == primitive.shape &&
                candidate.representation == primitive.representation) {
                group = &candidate;
                break;
            }
        }
        if (group == nullptr) {
            _PrimitiveGroup newGroup;
            newGroup.shape = primitive.shape;
            newGroup.representation = primitive.representation;
            newGroup.geometryType = geometryType;
            groups.push_back(std::move(newGroup));
            group = &groups.back();
        }
        group->primitiveIndices.push_back(primitiveIndex);
    }

    ty::CurveGeometryRecordBuildResult result;
    result.diagnostic.error = ty::CurveRecordError::none;
    result.records.reserve(groups.size());
    for (_PrimitiveGroup const& group : groups) {
        std::unique_ptr<ty::CurveGeometryRecordData> record(
            new ty::CurveGeometryRecordData(
                group.shape,
                group.representation,
                group.geometryType));
        record->_context->curvePrimitiveMetadata.reserve(
            group.primitiveIndices.size());
        record->_indices.reserve(group.primitiveIndices.size());

        bool packed = true;
        if (group.representation ==
            ty::CurveGeometryRepresentation::roundLinear) {
            packed = ty::CurveGeometryRecordBuilder::PackRoundLinear(
                topology,
                geometry,
                group.primitiveIndices,
                record.get());
        } else {
            for (size_t const primitiveIndex : group.primitiveIndices) {
                ty::CurveGeometryPrimitive const& primitive =
                    geometry.primitives[primitiveIndex];
                if (group.representation ==
                    ty::CurveGeometryRepresentation::spherePoint) {
                    if (!_CanAppend(record->_vertices.size(), 1)) {
                        packed = false;
                        break;
                    }
                    record->_vertices.push_back(_MakeVertex(
                        primitive.positionData[0], primitive.radiusData[0]));
                } else if (!ty::CurveGeometryRecordBuilder::PackCubicOrHermite(
                               primitive, record.get())) {
                    packed = false;
                    break;
                }
            }
        }
        if (!packed) {
            return _Failure(ty::CurveRecordError::bufferSizeOverflow);
        }

        for (size_t const primitiveIndex : group.primitiveIndices) {
            record->_context->curvePrimitiveMetadata.push_back(
                geometry.primitives[primitiveIndex].metadata);
        }
        result.records.push_back(std::move(record));
    }
    return result;
}

bool
ty::BindCurveGeometryBuffers(
    RTCGeometry geometry,
    ty::CurveGeometryRecordData& record)
{
    if (geometry == nullptr || record._vertices.empty() ||
        record.GetPrimitiveCount() == 0 ||
        record.GetPrimitiveCount() >
            static_cast<size_t>(
                std::numeric_limits<unsigned int>::max())) {
        return false;
    }

    ty::SetSharedGeometryBuffer(
        geometry,
        RTC_BUFFER_TYPE_VERTEX,
        0,
        RTC_FORMAT_FLOAT4,
        record._vertices.data(),
        sizeof(GfVec4f),
        record._vertices.size());
    if (!record._indices.empty()) {
        ty::SetSharedGeometryBuffer(
            geometry,
            RTC_BUFFER_TYPE_INDEX,
            0,
            RTC_FORMAT_UINT,
            record._indices.data(),
            sizeof(unsigned int),
            record._indices.size());
    }
    if (!record._tangents.empty()) {
        ty::SetSharedGeometryBuffer(
            geometry,
            RTC_BUFFER_TYPE_TANGENT,
            0,
            RTC_FORMAT_FLOAT4,
            record._tangents.data(),
            sizeof(GfVec4f),
            record._tangents.size());
    }
    if (!record._normals.empty()) {
        ty::SetSharedGeometryBuffer(
            geometry,
            RTC_BUFFER_TYPE_NORMAL,
            0,
            RTC_FORMAT_FLOAT3,
            record._normals.data(),
            sizeof(GfVec3f),
            record._normals.size());
    }
    if (!record._normalDerivatives.empty()) {
        ty::SetSharedGeometryBuffer(
            geometry,
            RTC_BUFFER_TYPE_NORMAL_DERIVATIVE,
            0,
            RTC_FORMAT_FLOAT3,
            record._normalDerivatives.data(),
            sizeof(GfVec3f),
            record._normalDerivatives.size());
    }
    if (!record._curveFlags.empty()) {
        ty::SetSharedGeometryBuffer(
            geometry,
            RTC_BUFFER_TYPE_FLAGS,
            0,
            RTC_FORMAT_UCHAR,
            record._curveFlags.data(),
            sizeof(std::uint32_t),
            record._curveFlags.size());
    }

    rtcSetGeometryUserData(geometry, record._context.get());
    ty::BindPrototypeGeometryFilter(geometry);
    return true;
}

char const*
ty::GetCurveRecordErrorName(ty::CurveRecordError error) noexcept
{
    switch (error) {
    case ty::CurveRecordError::notBuilt:
        return "notBuilt";
    case ty::CurveRecordError::none:
        return "none";
    case ty::CurveRecordError::invalidTopology:
        return "invalidTopology";
    case ty::CurveRecordError::invalidGeometry:
        return "invalidGeometry";
    case ty::CurveRecordError::invalidPrimitiveMetadata:
        return "invalidPrimitiveMetadata";
    case ty::CurveRecordError::unsupportedRepresentation:
        return "unsupportedRepresentation";
    case ty::CurveRecordError::bufferSizeOverflow:
        return "bufferSizeOverflow";
    }
    return "unknown";
}

PXR_NAMESPACE_CLOSE_SCOPE
