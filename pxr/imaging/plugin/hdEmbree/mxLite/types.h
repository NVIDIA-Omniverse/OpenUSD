//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_MXLITE_TYPES_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_MXLITE_TYPES_H

#include "pxr/pxr.h"
#include "pxr/base/gf/vec2f.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/gf/vec4f.h"
#include "pxr/base/gf/matrix4f.h"
#include "pxr/base/tf/token.h"
#include "pxr/base/vt/value.h"

#include <map>

PXR_NAMESPACE_OPEN_SCOPE

/// Geometric context provided to node evaluation at each shading point.
struct MxLiteShadingContext
{
    GfVec3f position    = GfVec3f(0.0f);
    GfVec3f normal      = GfVec3f(0.0f, 0.0f, 1.0f);
    GfVec3f tangent     = GfVec3f(1.0f, 0.0f, 0.0f);
    GfVec3f bitangent   = GfVec3f(0.0f, 1.0f, 0.0f);
    GfVec2f texcoord    = GfVec2f(0.0f);
    GfVec3f displayColor = GfVec3f(0.8f);
    float displayOpacity = 1.0f;
    int faceId = 0;
    float baryU = 0.0f;
    float baryV = 0.0f;
};

/// Surface closure produced by material model evaluation.
/// Contains all parameters needed for BSDF evaluation.
struct MxLiteSurfaceClosure
{
    GfVec3f baseColor        = GfVec3f(0.8f);
    float   roughness        = 0.5f;
    float   metallic         = 0.0f;
    float   specular         = 1.0f;
    float   specularIor      = 1.5f;
    GfVec3f specularColor    = GfVec3f(1.0f);
    GfVec3f emissiveColor    = GfVec3f(0.0f);
    float   transmission     = 0.0f;
    GfVec3f transmissionColor = GfVec3f(1.0f);
    float   opacity          = 1.0f;
    float   coat             = 0.0f;
    float   coatRoughness    = 0.1f;
    float   coatIor          = 1.5f;
    float   sheen            = 0.0f;
    GfVec3f sheenColor       = GfVec3f(1.0f);
    float   sheenRoughness   = 0.3f;
    GfVec3f normal           = GfVec3f(0.0f, 0.0f, 1.0f);
    bool    thinWalled       = false;
};

/// Named parameter map used for node inputs/outputs.
using MxLiteParamMap = std::map<TfToken, VtValue>;

/// Extract a typed value from a parameter map with a default fallback.
template<typename T>
T MxLiteGet(const MxLiteParamMap& params,
            const TfToken& name,
            const T& defaultVal)
{
    auto it = params.find(name);
    if (it != params.end() && it->second.IsHolding<T>()) {
        return it->second.UncheckedGet<T>();
    }
    return defaultVal;
}

/// Specialization for float: also accepts double and int.
template<>
inline float
MxLiteGet<float>(const MxLiteParamMap& params,
                 const TfToken& name,
                 const float& defaultVal)
{
    auto it = params.find(name);
    if (it == params.end()) return defaultVal;
    if (it->second.IsHolding<float>())
        return it->second.UncheckedGet<float>();
    if (it->second.IsHolding<double>())
        return static_cast<float>(it->second.UncheckedGet<double>());
    if (it->second.IsHolding<int>())
        return static_cast<float>(it->second.UncheckedGet<int>());
    return defaultVal;
}

/// Specialization for GfVec3f: also accepts GfVec3d.
template<>
inline GfVec3f
MxLiteGet<GfVec3f>(const MxLiteParamMap& params,
                   const TfToken& name,
                   const GfVec3f& defaultVal)
{
    auto it = params.find(name);
    if (it == params.end()) return defaultVal;
    if (it->second.IsHolding<GfVec3f>())
        return it->second.UncheckedGet<GfVec3f>();
    if (it->second.IsHolding<GfVec3d>()) {
        auto v = it->second.UncheckedGet<GfVec3d>();
        return GfVec3f(v[0], v[1], v[2]);
    }
    return defaultVal;
}

/// Zero value helpers for template-based node implementations.
template<typename T> inline T MxLiteZero();
template<> inline float  MxLiteZero<float>()  { return 0.0f; }
template<> inline int    MxLiteZero<int>()    { return 0; }
template<> inline bool   MxLiteZero<bool>()   { return false; }
template<> inline GfVec2f MxLiteZero<GfVec2f>() { return GfVec2f(0.0f); }
template<> inline GfVec3f MxLiteZero<GfVec3f>() { return GfVec3f(0.0f); }
template<> inline GfVec4f MxLiteZero<GfVec4f>() { return GfVec4f(0.0f); }

/// One value helpers for template-based node implementations.
template<typename T> inline T MxLiteOne();
template<> inline float  MxLiteOne<float>()  { return 1.0f; }
template<> inline int    MxLiteOne<int>()    { return 1; }
template<> inline GfVec2f MxLiteOne<GfVec2f>() { return GfVec2f(1.0f); }
template<> inline GfVec3f MxLiteOne<GfVec3f>() { return GfVec3f(1.0f); }
template<> inline GfVec4f MxLiteOne<GfVec4f>() { return GfVec4f(1.0f); }

/// Component-wise multiplication (scalars use operator*, vectors use
/// per-element multiplication).
template<typename T>
inline T MxLiteCompMul(const T& a, const T& b) { return a * b; }

template<>
inline GfVec2f MxLiteCompMul<GfVec2f>(const GfVec2f& a, const GfVec2f& b) {
    return GfVec2f(a[0]*b[0], a[1]*b[1]);
}
template<>
inline GfVec3f MxLiteCompMul<GfVec3f>(const GfVec3f& a, const GfVec3f& b) {
    return GfCompMult(a, b);
}
template<>
inline GfVec4f MxLiteCompMul<GfVec4f>(const GfVec4f& a, const GfVec4f& b) {
    return GfVec4f(a[0]*b[0], a[1]*b[1], a[2]*b[2], a[3]*b[3]);
}

/// Component-wise division.
template<typename T>
inline T MxLiteCompDiv(const T& a, const T& b) {
    return (b != T(0)) ? a / b : MxLiteZero<T>();
}
template<>
inline GfVec2f MxLiteCompDiv<GfVec2f>(const GfVec2f& a, const GfVec2f& b) {
    return GfVec2f(b[0] != 0.0f ? a[0]/b[0] : 0.0f,
                   b[1] != 0.0f ? a[1]/b[1] : 0.0f);
}
template<>
inline GfVec3f MxLiteCompDiv<GfVec3f>(const GfVec3f& a, const GfVec3f& b) {
    return GfVec3f(b[0] != 0.0f ? a[0]/b[0] : 0.0f,
                   b[1] != 0.0f ? a[1]/b[1] : 0.0f,
                   b[2] != 0.0f ? a[2]/b[2] : 0.0f);
}
template<>
inline GfVec4f MxLiteCompDiv<GfVec4f>(const GfVec4f& a, const GfVec4f& b) {
    return GfVec4f(b[0] != 0.0f ? a[0]/b[0] : 0.0f,
                   b[1] != 0.0f ? a[1]/b[1] : 0.0f,
                   b[2] != 0.0f ? a[2]/b[2] : 0.0f,
                   b[3] != 0.0f ? a[3]/b[3] : 0.0f);
}

PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_IMAGING_PLUGIN_HD_EMBREE_MXLITE_TYPES_H
