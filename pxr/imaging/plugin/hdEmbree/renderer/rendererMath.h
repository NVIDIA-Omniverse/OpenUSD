//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
// Pure renderer numeric and Gf/MaterialX conversion helpers.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_RENDERER_MATH_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_RENDERER_MATH_H

#include <renderer/materials/MaterialXCpp/mathTypes.h>

#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/gf/matrix4f.h"
#include "pxr/base/gf/vec2f.h"
#include "pxr/base/gf/vec3f.h"

#include <algorithm>
#include <cmath>

PXR_NAMESPACE_OPEN_SCOPE
namespace ty {

inline constexpr float Pi = static_cast<float>(M_PI);

inline bool
IsFinite(GfVec3f const& value)
{
    return std::isfinite(value[0]) &&
           std::isfinite(value[1]) &&
           std::isfinite(value[2]);
}

inline bool
TryNormalizeDirection(
    GfVec3f const& value,
    GfVec3f* normalized,
    double* length = nullptr)
{
    if (!normalized || !IsFinite(value)) {
        return false;
    }

    const double maximumComponent = std::max({
        std::abs(static_cast<double>(value[0])),
        std::abs(static_cast<double>(value[1])),
        std::abs(static_cast<double>(value[2]))});
    if (!std::isfinite(maximumComponent) || maximumComponent == 0.0) {
        return false;
    }

    const double x = static_cast<double>(value[0]) / maximumComponent;
    const double y = static_cast<double>(value[1]) / maximumComponent;
    const double z = static_cast<double>(value[2]) / maximumComponent;
    const double scaledLength = std::sqrt(x * x + y * y + z * z);
    if (!std::isfinite(scaledLength) || scaledLength == 0.0) {
        return false;
    }

    *normalized = GfVec3f(
        static_cast<float>(x / scaledLength),
        static_cast<float>(y / scaledLength),
        static_cast<float>(z / scaledLength));
    if (length) {
        *length = maximumComponent * scaledLength;
    }
    return IsFinite(*normalized);
}

inline float
DifferenceOfProducts(float a, float b, float c, float d)
{
    float cd = c * d;
    float err = std::fma(-c, d, cd);
    float dop = std::fma(a, b, -cd);
    return dop + err;
}

inline GfVec3f
ToGf(const mxcpp::Vec3f& v)
{
    return GfVec3f(v[0], v[1], v[2]);
}

inline mxcpp::Vec3f
ToMx(const GfVec3f& v)
{
    return mxcpp::Vec3f(v[0], v[1], v[2]);
}

inline mxcpp::Vec2f
ToMx(const GfVec2f& v)
{
    return mxcpp::Vec2f(v[0], v[1]);
}

inline mxcpp::Mat4f
ToMx(const GfMatrix4f& m)
{
    mxcpp::Mat4f result;
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            result[row][col] = m[row][col];
        }
    }
    return result;
}

inline float
Clamp01(float value)
{
    return std::clamp(value, 0.0f, 1.0f);
}

inline GfVec3f
Clamp01(GfVec3f const& value)
{
    return GfVec3f(
        Clamp01(value[0]),
        Clamp01(value[1]),
        Clamp01(value[2]));
}

inline mxcpp::Mat4f
ToMx(const GfMatrix4d& m)
{
    mxcpp::Mat4f result;
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            result[row][col] = static_cast<float>(m[row][col]);
        }
    }
    return result;
}

} // namespace ty
PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_IMAGING_PLUGIN_HD_EMBREE_RENDERER_MATH_H
