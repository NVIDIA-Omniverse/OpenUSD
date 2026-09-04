//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_CURVE_EMBREE_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_CURVE_EMBREE_H

#include "context.h"
#include "curveGeometry.h"

#include <renderer/embreeCompat.h>

#include "pxr/base/gf/vec3f.h"
#include "pxr/base/gf/vec4f.h"
#include "pxr/pxr.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE
namespace ty {

constexpr size_t CurveRecordNoIndex = std::numeric_limits<size_t>::max();

enum class CurveRecordError
{
    notBuilt,
    none,
    invalidTopology,
    invalidGeometry,
    invalidPrimitiveMetadata,
    unsupportedRepresentation,
    bufferSizeOverflow
};

struct CurveRecordDiagnostic
{
    CurveRecordError error = CurveRecordError::notBuilt;
    size_t primitiveIndex = CurveRecordNoIndex;
};

struct CurveGeometryRecordBuildResult;
struct CurveGeometryRecordBuilder;

/// Stable-address storage bound non-owningly to one Embree curve geometry.
/// The record deliberately owns no RTCGeometry, scene attachment, or geometry
/// ID; the BasisCurves Rprim owns those lifecycle values.
class CurveGeometryRecordData final
{
public:
    CurveGeometryRecordData(CurveGeometryRecordData const&) = delete;
    CurveGeometryRecordData& operator=(
        CurveGeometryRecordData const&) = delete;
    CurveGeometryRecordData(CurveGeometryRecordData&&) = delete;
    CurveGeometryRecordData& operator=(CurveGeometryRecordData&&) = delete;
    ~CurveGeometryRecordData();

    RTCGeometryType GetGeometryType() const noexcept;
    CurveGeometryShape GetShape() const noexcept;
    CurveGeometryRepresentation GetRepresentation() const noexcept;
    size_t GetPrimitiveCount() const noexcept;

    PrototypeContext& GetContext() noexcept;
    PrototypeContext const& GetContext() const noexcept;

    std::vector<GfVec4f> const& GetVertices() const noexcept;
    std::vector<unsigned int> const& GetIndices() const noexcept;
    std::vector<GfVec4f> const& GetTangents() const noexcept;
    std::vector<GfVec3f> const& GetNormals() const noexcept;
    std::vector<GfVec3f> const& GetNormalDerivatives() const noexcept;
    std::vector<std::uint32_t> const& GetCurveFlags() const noexcept;

private:
    CurveGeometryRecordData(
        CurveGeometryShape shape,
        CurveGeometryRepresentation representation,
        RTCGeometryType geometryType);

    CurveGeometryShape const _shape;
    CurveGeometryRepresentation const _representation;
    RTCGeometryType const _geometryType;
    std::unique_ptr<PrototypeContext> const _context;
    std::vector<GfVec4f> _vertices;
    std::vector<unsigned int> _indices;
    std::vector<GfVec4f> _tangents;
    std::vector<GfVec3f> _normals;
    std::vector<GfVec3f> _normalDerivatives;
    /// Low byte contains RTCCurveFlags. A four-byte stride satisfies Embree's
    /// shared-buffer alignment contract for RTC_FORMAT_UCHAR.
    std::vector<std::uint32_t> _curveFlags;

    friend CurveGeometryRecordBuildResult BuildCurveGeometryRecords(
        CurveTopologyResult const&,
        CurveGeometryResult const&);
    friend bool BindCurveGeometryBuffers(
        RTCGeometry,
        CurveGeometryRecordData&);
    friend struct CurveGeometryRecordBuilder;
};

struct CurveGeometryRecordBuildResult
{
    CurveRecordDiagnostic diagnostic;
    std::vector<std::unique_ptr<CurveGeometryRecordData>> records;

    bool IsValid() const
    {
        return diagnostic.error == CurveRecordError::none;
    }
};

/// Map surface family and buffer representation to an Embree geometry type.
/// Returns false for combinations that the geometry planner never emits.
bool GetEmbreeCurveGeometryType(
    CurveGeometryShape shape,
    CurveGeometryRepresentation representation,
    RTCGeometryType* geometryType) noexcept;

/// Convert a pure curve geometry plan into stable, representation-homogeneous
/// records. A valid empty geometry plan produces a valid empty record list.
CurveGeometryRecordBuildResult BuildCurveGeometryRecords(
    CurveTopologyResult const& topology,
    CurveGeometryResult const& geometry);

/// Bind one record's immutable storage, stable context, and unified filters to
/// an externally owned RTCGeometry. This function does not commit, attach,
/// release, or retain the handle.
bool BindCurveGeometryBuffers(
    RTCGeometry geometry,
    CurveGeometryRecordData& record);

char const* GetCurveRecordErrorName(CurveRecordError error) noexcept;

} // namespace ty
PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_IMAGING_PLUGIN_HD_EMBREE_CURVE_EMBREE_H
