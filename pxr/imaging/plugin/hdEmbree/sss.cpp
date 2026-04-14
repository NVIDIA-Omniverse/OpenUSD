//
// hdEmbree random-walk SSS helpers.
//
#include "pxr/imaging/plugin/hdEmbree/sss.h"

#include <algorithm>
#include <cmath>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

constexpr float _kPi = 3.14159265358979323846f;
constexpr float _kBias = 1.0e-4f;

GfVec3f
_SafeNormalize(GfVec3f const& value, GfVec3f const& fallback)
{
    if (value.GetLengthSq() <= 1.0e-12f) {
        return fallback;
    }
    return value.GetNormalized();
}

void
_CoordinateSystem(
    GfVec3f const& zAxis,
    GfVec3f* xAxis,
    GfVec3f* yAxis)
{
    const GfVec3f z = _SafeNormalize(zAxis, GfVec3f(0.0f, 0.0f, 1.0f));
    if (std::abs(z[2]) < 0.999f) {
        *xAxis = GfVec3f(-z[1], z[0], 0.0f);
    } else {
        *xAxis = GfCross(GfVec3f(0.0f, 1.0f, 0.0f), z);
    }
    if (xAxis->GetLengthSq() <= 1.0e-12f) {
        *xAxis = GfVec3f(1.0f, 0.0f, 0.0f);
    } else {
        *xAxis = xAxis->GetNormalized();
    }
    *yAxis = GfCross(z, *xAxis).GetNormalized();
}

}  // namespace

HdEmbreeSubsurfaceEntry
HdEmbreeSampleSubsurfaceEntry(
    GfVec3f const& position,
    GfVec3f const& outwardNormal,
    float u1,
    float u2)
{
    const GfVec3f inwardNormal =
        -_SafeNormalize(outwardNormal, GfVec3f(0.0f, 1.0f, 0.0f));
    GfVec3f tangent(1.0f, 0.0f, 0.0f);
    GfVec3f bitangent(0.0f, 0.0f, 1.0f);
    _CoordinateSystem(inwardNormal, &tangent, &bitangent);

    const float clampedU1 = std::clamp(u1, 0.0f, 1.0f);
    const float clampedU2 = std::clamp(u2, 0.0f, 1.0f);
    const float r = std::sqrt(clampedU1);
    const float phi = 2.0f * _kPi * clampedU2;
    const float x = r * std::cos(phi);
    const float z = r * std::sin(phi);
    const float y = std::sqrt(std::max(0.0f, 1.0f - clampedU1));

    GfVec3f direction =
        tangent * x + inwardNormal * y + bitangent * z;
    direction.Normalize();

    return HdEmbreeSubsurfaceEntry{
        position + inwardNormal * _kBias,
        direction,
    };
}

mxcpp::SurfaceClosure
HdEmbreeMakeSubsurfaceExitClosure(
    mxcpp::SurfaceClosure const& /*surfaceClosure*/)
{
    mxcpp::SurfaceClosure closure;
    closure.baseColor = mxcpp::Vec3f(1.0f);
    closure.roughness = 1.0f;
    closure.metallic = 0.0f;
    closure.specular = 0.0f;
    closure.specularColor = mxcpp::Vec3f(1.0f);
    closure.specularIor = 1.5f;
    closure.transmission = 0.0f;
    closure.opacity = 1.0f;
    closure.presence = 1.0f;
    closure.coat = 0.0f;
    closure.sheen = 0.0f;
    closure.emissiveColor = mxcpp::Vec3f(0.0f);
    return closure;
}

PXR_NAMESPACE_CLOSE_SCOPE
