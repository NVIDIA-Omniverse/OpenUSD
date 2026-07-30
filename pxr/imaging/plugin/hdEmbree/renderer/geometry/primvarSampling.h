//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_PRIMVAR_SAMPLING_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_PRIMVAR_SAMPLING_H

#include "meshSamplers.h"
#include "primvarSampler.h"

#include <renderer/materials/MaterialXCpp/value.h>

#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/gf/matrix4f.h"
#include "pxr/base/gf/vec2f.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/gf/vec4f.h"
#include "pxr/pxr.h"

#include <cmath>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE
namespace ty {

// Surface shading and subdivision displacement evaluate the same MaterialX
// geomprop nodes at different times. Keeping their lookup here ensures both
// consumers use the interpolation mode chosen by the mesh primvar sampler.
struct PrimvarLookup
{
    std::vector<PrimvarSampler*> const* primvars;
    unsigned int primId;
    float u;
    float v;
};

/// Inverse Jacobian that converts Embree patch-coordinate derivatives to
/// authored texture-coordinate derivatives.
struct SubdivTexcoordJacobian
{
    float duDs = 1.0f;
    float dvDs = 0.0f;
    float duDt = 0.0f;
    float dvDt = 1.0f;
    bool valid = false;
};

/// Sample a subdivision `st` primvar and build its inverse Jacobian.
///
/// Both displacement-time and hit-time material evaluation use this helper so
/// dPdu/dPdv and object-position derivatives have identical `st` semantics.
inline SubdivTexcoordJacobian
ComputeSubdivTexcoordJacobian(
    PrimvarSampler const* sampler,
    unsigned int primId,
    float u,
    float v)
{
    SubdivTexcoordJacobian result;
    SubdivSampler const* subdivSampler =
        dynamic_cast<SubdivSampler const*>(sampler);
    if (!subdivSampler) {
        return result;
    }

    GfVec2f dStdu(0.0f);
    GfVec2f dStdv(0.0f);
    bool sampled = false;
    {
        GfVec2f value;
        sampled = subdivSampler->SampleWithDerivatives(
            primId, u, v, &value, &dStdu, &dStdv);
    }
    if (!sampled) {
        GfVec3f value;
        GfVec3f dStdu3;
        GfVec3f dStdv3;
        sampled = subdivSampler->SampleWithDerivatives(
            primId, u, v, &value, &dStdu3, &dStdv3);
        if (sampled) {
            dStdu = GfVec2f(dStdu3[0], dStdu3[1]);
            dStdv = GfVec2f(dStdv3[0], dStdv3[1]);
        }
    }
    if (!sampled) {
        return result;
    }

    const double determinant =
        static_cast<double>(dStdu[0]) * dStdv[1] -
        static_cast<double>(dStdu[1]) * dStdv[0];
    const double uLength = std::hypot(
        static_cast<double>(dStdu[0]),
        static_cast<double>(dStdu[1]));
    const double vLength = std::hypot(
        static_cast<double>(dStdv[0]),
        static_cast<double>(dStdv[1]));
    if (!std::isfinite(determinant) ||
        !std::isfinite(uLength) || !std::isfinite(vLength) ||
        uLength == 0.0 || vLength == 0.0 ||
        determinant == 0.0) {
        return result;
    }
    const double relativeDeterminant = determinant / (uLength * vLength);
    if (!std::isfinite(relativeDeterminant) ||
        std::abs(relativeDeterminant) <= 1.0e-9) {
        return result;
    }

    const double inverseDeterminant = 1.0 / determinant;
    result.duDs = static_cast<float>(dStdv[1] * inverseDeterminant);
    result.dvDs = static_cast<float>(-dStdu[1] * inverseDeterminant);
    result.duDt = static_cast<float>(-dStdv[0] * inverseDeterminant);
    result.dvDt = static_cast<float>(dStdu[0] * inverseDeterminant);
    result.valid = std::isfinite(result.duDs) &&
        std::isfinite(result.dvDs) &&
        std::isfinite(result.duDt) &&
        std::isfinite(result.dvDt);
    return result;
}

namespace PrimvarSamplingDetail {

template <class Matrix>
inline mxcpp::Mat4f
ToMxMatrix(Matrix const& matrix)
{
    mxcpp::Mat4f result;
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            result[row][column] = static_cast<float>(matrix[row][column]);
        }
    }
    return result;
}

} // namespace PrimvarSamplingDetail

// MaterialX treats both float2 and float3 texcoords as a 2D lookup domain.
// Centralizing the fallback keeps displacement-time and hit-time evaluation on
// the same authored primvar representation.
inline bool
SampleTexcoord(
    PrimvarSampler const* sampler,
    unsigned int primId,
    float u,
    float v,
    mxcpp::Vec2f* result)
{
    if (!sampler || !result) {
        return false;
    }
    GfVec2f value2;
    if (sampler->Sample(primId, u, v, &value2)) {
        *result = mxcpp::Vec2f(value2[0], value2[1]);
        return true;
    }
    GfVec3f value3;
    if (sampler->Sample(primId, u, v, &value3)) {
        *result = mxcpp::Vec2f(value3[0], value3[1]);
        return true;
    }
    return false;
}

inline mxcpp::Value
SamplePrimvar(void const* userData, int geomPropHandle)
{
    PrimvarLookup const* lookup =
        static_cast<PrimvarLookup const*>(userData);
    if (!lookup || !lookup->primvars || geomPropHandle < 0 ||
        static_cast<size_t>(geomPropHandle) >= lookup->primvars->size()) {
        return mxcpp::Value();
    }
    PrimvarSampler* const sampler =
        (*lookup->primvars)[geomPropHandle];
    if (!sampler) {
        return mxcpp::Value();
    }

    // Sample requires an exact tuple type. Trying supported MaterialX types in
    // likely-use order is therefore deterministic and avoids storing another
    // type tag in the graph-facing lookup table.
    GfVec2f vec2;
    if (sampler->Sample(lookup->primId, lookup->u, lookup->v, &vec2)) {
        return mxcpp::Value(mxcpp::Vec2f(vec2[0], vec2[1]));
    }
    GfVec3f vec3;
    if (sampler->Sample(lookup->primId, lookup->u, lookup->v, &vec3)) {
        return mxcpp::Value(mxcpp::Vec3f(vec3[0], vec3[1], vec3[2]));
    }
    float scalar;
    if (sampler->Sample(lookup->primId, lookup->u, lookup->v, &scalar)) {
        return mxcpp::Value(scalar);
    }
    GfVec4f vec4;
    if (sampler->Sample(lookup->primId, lookup->u, lookup->v, &vec4)) {
        return mxcpp::Value(mxcpp::Vec4f(vec4[0], vec4[1], vec4[2], vec4[3]));
    }
    int integer;
    if (sampler->Sample(lookup->primId, lookup->u, lookup->v, &integer)) {
        return mxcpp::Value(integer);
    }
    bool boolean;
    if (sampler->Sample(lookup->primId, lookup->u, lookup->v, &boolean)) {
        return mxcpp::Value(boolean);
    }
    GfMatrix4f matrix4f;
    if (sampler->Sample(lookup->primId, lookup->u, lookup->v, &matrix4f)) {
        return mxcpp::Value(
            PrimvarSamplingDetail::ToMxMatrix(matrix4f));
    }
    GfMatrix4d matrix4d;
    if (sampler->Sample(lookup->primId, lookup->u, lookup->v, &matrix4d)) {
        return mxcpp::Value(
            PrimvarSamplingDetail::ToMxMatrix(matrix4d));
    }
    return mxcpp::Value();
}

} // namespace ty
PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_IMAGING_PLUGIN_HD_EMBREE_PRIMVAR_SAMPLING_H
