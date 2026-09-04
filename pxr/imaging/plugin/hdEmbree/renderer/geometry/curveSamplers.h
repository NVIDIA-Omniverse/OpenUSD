//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_CURVE_SAMPLERS_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_CURVE_SAMPLERS_H

#include "curveGeometry.h"
#include "primvarSampler.h"

#include "pxr/base/tf/token.h"
#include "pxr/pxr.h"

#include <cstddef>
#include <limits>
#include <memory>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE
namespace ty {

constexpr size_t CurveSamplerNoIndex = std::numeric_limits<size_t>::max();

/// General material-primvar validation result. A failure invalidates only the
/// requested primvar; it never invalidates curve topology or geometry.
enum class CurveSamplerError
{
    notCreated,
    none,
    invalidTopology,
    unsupportedValueType,
    invalidInterpolation,
    elementCountMismatch,
    indexCountMismatch,
    invalidIndex,
    invalidPrimitiveMetadata
};

struct CurveSamplerDiagnostic
{
    CurveSamplerError error = CurveSamplerError::notCreated;
    size_t elementIndex = CurveSamplerNoIndex;
    size_t affectedCount = 0;
    size_t expectedCount = 0;
    size_t actualCount = 0;
    int value = 0;
    TfToken interpolation;
};

/// Authored identity decoded from one record-local Embree hit.
struct DecodedCurveHit
{
    size_t authoredCurveId = 0;
    size_t authoredSegmentId = 0;
    float authoredU = 0.0f;
};

/// Decode record-local generated primitive ID and U through immutable
/// metadata. On failure, `decoded` is left unchanged.
bool DecodeCurveHit(
    std::vector<CurveSegmentMetadata> const& primitiveMetadata,
    unsigned int primitiveId,
    float localU,
    DecodedCurveHit* decoded) noexcept;

class CurvePrimvarSampler;

struct CurvePrimvarSamplerResult
{
    CurveSamplerDiagnostic diagnostic;
    std::unique_ptr<PrimvarSampler> sampler;

    bool IsValid() const
    {
        return diagnostic.error == CurveSamplerError::none &&
            sampler != nullptr;
    }
};

/// PrimvarSampler implementation for a single curve geometry record. The
/// factory validates and owns a flattened value array plus an immutable copy
/// of the record-local sampling map.
class CurvePrimvarSampler final : public PrimvarSampler
{
public:
    ~CurvePrimvarSampler() override;

    bool Sample(
        unsigned int element,
        float u,
        float v,
        void* value,
        HdTupleType dataType) const override;

private:
    struct _Impl;

    explicit CurvePrimvarSampler(std::unique_ptr<_Impl> implementation);

    std::unique_ptr<_Impl> const _implementation;

    friend CurvePrimvarSamplerResult CreateCurvePrimvarSampler(
        TfToken const&,
        CurvePrimvarInput const&,
        CurveTopologyResult const&,
        std::vector<CurveSegmentMetadata> const&);
};

/// Validate one authored general primvar and construct its record-local
/// sampler. Indexed values are range-checked and flattened before any sampler
/// is exposed. faceVarying and all malformed inputs return a diagnostic-only
/// result so callers can keep the geometry and every other primvar.
CurvePrimvarSamplerResult CreateCurvePrimvarSampler(
    TfToken const& name,
    CurvePrimvarInput const& input,
    CurveTopologyResult const& topology,
    std::vector<CurveSegmentMetadata> const& primitiveMetadata);

char const* GetCurveSamplerErrorName(CurveSamplerError error) noexcept;

} // namespace ty
PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_IMAGING_PLUGIN_HD_EMBREE_CURVE_SAMPLERS_H
