//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_PRIMVAR_SAMPLING_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_PRIMVAR_SAMPLING_H

#include "pxr/pxr.h"

#include "pxr/imaging/plugin/hdEmbree/renderer/geometry/primvarSampler.h"
#include "pxr/imaging/plugin/hdEmbree/renderer/materials/MaterialXCpp/value.h"

#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/gf/matrix4f.h"
#include "pxr/base/gf/vec2f.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/gf/vec4f.h"

#include <string>
#include <unordered_map>

PXR_NAMESPACE_OPEN_SCOPE

// Surface shading and subdivision displacement evaluate the same MaterialX
// geomprop nodes at different times. Keeping their lookup here ensures both
// consumers use the interpolation mode chosen by the mesh primvar sampler.
struct HdEmbreePrimvarLookup
{
    std::unordered_map<std::string, HdEmbreePrimvarSampler*> const* primvars;
    unsigned int primId;
    float u;
    float v;
};

namespace HdEmbreePrimvarSamplingDetail {

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

} // namespace HdEmbreePrimvarSamplingDetail

// MaterialX treats both float2 and float3 texcoords as a 2D lookup domain.
// Centralizing the fallback keeps displacement-time and hit-time evaluation on
// the same authored primvar representation.
inline bool
HdEmbreeSampleTexcoord(
    HdEmbreePrimvarSampler const* sampler,
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
HdEmbreeSamplePrimvar(void const* userData, std::string const& name)
{
    auto const* lookup = static_cast<HdEmbreePrimvarLookup const*>(userData);
    if (!lookup || !lookup->primvars) {
        return mxcpp::Value();
    }
    auto const it = lookup->primvars->find(name);
    if (it == lookup->primvars->end()) {
        return mxcpp::Value();
    }

    // Sample requires an exact tuple type. Trying supported MaterialX types in
    // likely-use order is therefore deterministic and avoids storing another
    // type tag in the graph-facing lookup table.
    HdEmbreePrimvarSampler* sampler = it->second;
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
            HdEmbreePrimvarSamplingDetail::ToMxMatrix(matrix4f));
    }
    GfMatrix4d matrix4d;
    if (sampler->Sample(lookup->primId, lookup->u, lookup->v, &matrix4d)) {
        return mxcpp::Value(
            HdEmbreePrimvarSamplingDetail::ToMxMatrix(matrix4d));
    }
    return mxcpp::Value();
}

PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_IMAGING_PLUGIN_HD_EMBREE_PRIMVAR_SAMPLING_H
