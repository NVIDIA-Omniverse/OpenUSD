//
// Copyright 2025 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "lightSamplers.h"

#include "pxr/base/gf/color.h"
#include "pxr/base/gf/colorSpace.h"
#include "pxr/base/gf/vec2f.h"
#include "pxr/base/gf/vec3f.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

PXR_NAMESPACE_USING_DIRECTIVE

// -------------------------------------------------------------------------
// General Math Utilities
// -------------------------------------------------------------------------

template <typename T>
constexpr T _pi = static_cast<T>(M_PI);

inline float
_Sqr(float x)
{
    return x*x;
}

// Dot product, but set to 0 if less than 0 - ie, 0 for backward-facing rays
inline float
_DotZeroClip(GfVec3f const& a, GfVec3f const& b)
{
    return std::max(0.0f, GfDot(a, b));
}

float
_AreaRect(GfMatrix4f const& xf, float width, float height)
{
    const GfVec3f U = xf.TransformDir(GfVec3f{width, 0.0f, 0.0f});
    const GfVec3f V = xf.TransformDir(GfVec3f{0.0f, height, 0.0f});
    return GfCross(U, V).GetLength();
}

float
_AreaSphere(GfMatrix4f const& xf, float radius)
{
    // Area of the ellipsoid
    const float a = xf.TransformDir(GfVec3f{radius, 0.0f, 0.0f}).GetLength();
    const float b = xf.TransformDir(GfVec3f{0.0f, radius, 0.0f}).GetLength();
    const float c = xf.TransformDir(GfVec3f{0.0f, 0.0f, radius}).GetLength();
    const float ab = powf(a*b, 1.6f);
    const float ac = powf(a*c, 1.6f);
    const float bc = powf(b*c, 1.6f);
    return powf((ab + ac + bc) / 3.0f, 1.0f / 1.6f) * 4.0f * _pi<float>;
}

float
_AreaDisk(GfMatrix4f const& xf, float radius)
{
    // Calculate surface area of the ellipse
    const float a = xf.TransformDir(GfVec3f{radius, 0.0f, 0.0f}).GetLength();
    const float b = xf.TransformDir(GfVec3f{0.0f, radius, 0.0f}).GetLength();
    return _pi<float> * a * b;
}

float
_AreaCylinder(GfMatrix4f const& xf, float radius, float length)
{
    const float c = xf.TransformDir(GfVec3f{length, 0.0f, 0.0f}).GetLength();
    const float a = xf.TransformDir(GfVec3f{0.0f, radius, 0.0f}).GetLength();
    const float b = xf.TransformDir(GfVec3f{0.0f, 0.0f, radius}).GetLength();
    // Ramanujan's approximation to perimeter of ellipse
    const float e =
        _pi<float> * (3.0f * (a + b) - sqrtf((3.0f * a + b) * (a + 3.0f * b)));
    return e * c;
}

// -------------------------------------------------------------------------
// Color utilities
// -------------------------------------------------------------------------

// Recreates UsdLuxBlackbodyTemperatureAsRgb in "pxr/usd/usdLux/blackbody.h"...
/// But uses new GfColor functionality, since we shouldn't import usd into
// imaging

// Perhaps UsdLuxBlackbodyTemperatureAsRgb should be deprecated, and this made
// a new utility function somewhere, for use by other HdRenderDelegates?
// (Maybe in gf/color.h?)
inline GfVec3f
_BlackbodyTemperatureAsRgb(
    float kelvinColorTemp,
    HdEmbreeRenderColorSpace renderColorSpace)
{
    const GfColorSpace workingColorSpace(
        HdEmbreeGetWorkingColorSpaceToken(renderColorSpace));
    auto tempColor = GfColor(workingColorSpace);
    // Get color in the numerical working space with luminance 1.0.
    tempColor.SetFromPlanckianLocus(kelvinColorTemp, 1.0f);
    GfVec3f tempColorRGB = tempColor.GetRGB();
    const float luminance = GfDot(
        tempColorRGB,
        HdEmbreeGetLuminanceCoefficients(renderColorSpace));
    return luminance > 0.0f ? tempColorRGB / luminance : GfVec3f(1.0f);
}

// -------------------------------------------------------------------------
// Light sampling structures / utilities
// -------------------------------------------------------------------------

struct _ShapeSample {
    GfVec3f pWorld;
    GfVec3f nWorld;
    GfVec2f uv;
    float pdfAreaInverse;
};

inline float
_ClampUnit(float u)
{
    return GfClamp(
        u, 0.0f, std::nextafter(1.0f, 0.0f));
}

inline float
_WrapUnit(float u)
{
    u -= std::floor(u);
    if (u < 0.0f) {
        u += 1.0f;
    }
    return u;
}

bool
_HasDomeDistribution(HdEmbree_LightTexture const& texture)
{
    const size_t texelCount =
        static_cast<size_t>(texture.width) * static_cast<size_t>(texture.height);
    return texture.width > 0 &&
           texture.height > 0 &&
           texture.texelWeights.size() == texelCount &&
           texture.conditionalCdf.size() ==
               static_cast<size_t>(texture.height) *
                   static_cast<size_t>(texture.width + 1) &&
           texture.marginalCdf.size() ==
               static_cast<size_t>(texture.height + 1) &&
           texture.weightSum > 0.0f;
}

GfVec2f
_DirectionToLatLongUv(const GfVec3f& localDirection)
{
    const GfVec3f normalized = localDirection.GetNormalized();
    const float t = acosf(GfClamp(normalized[1], -1.0f, 1.0f)) / _pi<float>;
    const float s = _WrapUnit(
        0.5f - atan2f(normalized[0], normalized[2]) / (2.0f * _pi<float>));
    return GfVec2f(s, _ClampUnit(t));
}

GfVec3f
_LatLongUvToDirection(const GfVec2f& uv)
{
    const float theta = _pi<float> * _ClampUnit(uv[1]);
    const float sinTheta = sinf(theta);
    const float cosTheta = cosf(theta);
    const float phi = 2.0f * _pi<float> * (0.5f - uv[0]);
    return GfVec3f(
        sinTheta * sinf(phi),
        cosTheta,
        sinTheta * cosf(phi));
}

float
_TexelDirectionalPdf(
    HdEmbree_LightTexture const& texture,
    int x,
    int y,
    float theta)
{
    if (!_HasDomeDistribution(texture) || x < 0 || y < 0 ||
        x >= texture.width || y >= texture.height) {
        return 0.0f;
    }

    const float sinTheta = sinf(theta);
    if (sinTheta <= 0.0f) {
        return 0.0f;
    }

    const size_t idx =
        static_cast<size_t>(y) * static_cast<size_t>(texture.width) + x;
    const float texelMass = texture.texelWeights[idx] / texture.weightSum;
    const float pdfUv =
        texelMass * static_cast<float>(texture.width * texture.height);
    return pdfUv / (2.0f * _pi<float> * _pi<float> * sinTheta);
}

GfVec2f
_SampleDomeUv(HdEmbree_LightTexture const& texture, float u1, float u2)
{
    const float sampleX = _ClampUnit(u1);
    const float sampleY = _ClampUnit(u2);

    const auto marginalBegin = texture.marginalCdf.begin();
    const auto marginalIt = std::upper_bound(
        marginalBegin + 1, texture.marginalCdf.end(), sampleY);
    const int y = std::clamp(
        static_cast<int>(marginalIt - (marginalBegin + 1)),
        0,
        texture.height - 1);

    const float cdfY0 = texture.marginalCdf[y];
    const float cdfY1 = texture.marginalCdf[y + 1];
    const float remappedY =
        (cdfY1 > cdfY0) ? ((sampleY - cdfY0) / (cdfY1 - cdfY0)) : 0.0f;

    const float* const rowBegin = texture.conditionalCdf.data() +
        static_cast<size_t>(y) * static_cast<size_t>(texture.width + 1);
    const float* const rowIt = std::upper_bound(
        rowBegin + 1, rowBegin + texture.width + 1, sampleX);
    const int x = std::clamp(
        static_cast<int>(rowIt - (rowBegin + 1)),
        0,
        texture.width - 1);

    const float cdfX0 = rowBegin[x];
    const float cdfX1 = rowBegin[x + 1];
    const float remappedX =
        (cdfX1 > cdfX0) ? ((sampleX - cdfX0) / (cdfX1 - cdfX0)) : 0.0f;

    return GfVec2f(
        (static_cast<float>(x) + remappedX) / static_cast<float>(texture.width),
        (static_cast<float>(y) + remappedY) / static_cast<float>(texture.height));
}

bool
_IsFinite(GfVec3f const& v)
{
    return std::isfinite(v[0]) && std::isfinite(v[1]) &&
           std::isfinite(v[2]);
}

float
_DomeDirectionalPdf(
    HdEmbree_LightData const& light,
    GfVec2f const& uv)
{
    float pdfSolidAngle = 1.0f / (4.0f * _pi<float>);
    if (_HasDomeDistribution(light.texture)) {
        const int x = std::clamp(
            static_cast<int>(
                static_cast<float>(light.texture.width) * _WrapUnit(uv[0])),
            0,
            light.texture.width - 1);
        const int y = std::clamp(
            static_cast<int>(
                static_cast<float>(light.texture.height) * _ClampUnit(uv[1])),
            0,
            light.texture.height - 1);
        const float theta = _pi<float> * _ClampUnit(uv[1]);
        pdfSolidAngle = _TexelDirectionalPdf(light.texture, x, y, theta);
    }
    return pdfSolidAngle;
}

float
_DomeDirectionalPdf(
    HdEmbree_LightData const& light,
    GfVec3f const& worldDirection)
{
    if (!_IsFinite(worldDirection) || worldDirection.GetLengthSq() <= 0.0f) {
        return 0.0f;
    }

    const GfVec3f localDirection =
        light.xformWorldToLight.TransformDir(
            worldDirection.GetNormalized()).GetNormalized();
    return _DomeDirectionalPdf(light, _DirectionToLatLongUv(localDirection));
}

bool
_GetReflectionHemisphereNormal(
    GfVec3f const& normal,
    GfVec3f* normalizedNormal)
{
    if (!normalizedNormal || !_IsFinite(normal) ||
        normal.GetLengthSq() <= 0.0f) {
        return false;
    }
    *normalizedNormal = normal.GetNormalized();
    return true;
}

GfVec3f
_ReflectAcrossPlane(
    GfVec3f const& direction,
    GfVec3f const& normal)
{
    return (direction - normal * (2.0f * GfDot(direction, normal)))
        .GetNormalized();
}

float
_ReflectionHemispherePdf(
    HdEmbree_LightData const& light,
    GfVec3f const& normal,
    GfVec3f const& direction)
{
    GfVec3f n;
    if (!_GetReflectionHemisphereNormal(normal, &n) ||
        !_IsFinite(direction) || direction.GetLengthSq() <= 0.0f) {
        return 0.0f;
    }

    const GfVec3f omegaInWld = direction.GetNormalized();
    if (GfDot(n, omegaInWld) <= 0.0f) {
        return 0.0f;
    }

    const GfVec3f mirrored = _ReflectAcrossPlane(omegaInWld, n);
    return _DomeDirectionalPdf(light, omegaInWld) +
           _DomeDirectionalPdf(light, mirrored);
}

GfVec3f
_FoldDirectionToReflectionHemisphere(
    GfVec3f const& direction,
    GfVec3f const& normal)
{
    GfVec3f n;
    if (!_GetReflectionHemisphereNormal(normal, &n) ||
        !_IsFinite(direction) || direction.GetLengthSq() <= 0.0f) {
        return GfVec3f(0.0f);
    }

    const GfVec3f omegaInWld = direction.GetNormalized();
    return (GfDot(n, omegaInWld) > 0.0f) ? omegaInWld
                                         : _ReflectAcrossPlane(omegaInWld, n);
}

GfVec3f
_SampleLightTexture(
    HdEmbree_LightTexture const& texture,
    float s,
    float t,
    HdEmbreeRenderColorSpace renderColorSpace)
{
    if (texture.pixels.empty()) {
        return GfVec3f(0.0f);
    }

    const int x = std::clamp(
        static_cast<int>(static_cast<float>(texture.width) * _WrapUnit(s)),
        0,
        texture.width - 1);
    const int y = std::clamp(
        static_cast<int>(static_cast<float>(texture.height) * _ClampUnit(t)),
        0,
        texture.height - 1);

    GfVec3f result = texture.pixels.at(y * texture.width + x);
    HdEmbreeConvertToRenderColorSpace(
        texture.colorSpaceName.GetString(), renderColorSpace, &result);
    return result;
}

GfVec3f
_SampleRectLightTexture(HdEmbree_LightTexture const& texture,
                        GfVec2f const& uv,
                        HdEmbreeRenderColorSpace renderColorSpace)
{
    if (texture.pixels.empty() || texture.width <= 0 || texture.height <= 0) {
        return GfVec3f(0.0f);
    }

    const float s = 1.0f - uv[0];
    const float t = 1.0f - uv[1];
    const int x = std::clamp(
        static_cast<int>(static_cast<float>(texture.width) * _ClampUnit(s)),
        0,
        texture.width - 1);
    const int y = std::clamp(
        static_cast<int>(static_cast<float>(texture.height) * _ClampUnit(t)),
        0,
        texture.height - 1);

    GfVec3f result = texture.pixels.at(y * texture.width + x);
    HdEmbreeConvertToRenderColorSpace(
        texture.colorSpaceName.GetString(), renderColorSpace, &result);
    return result;
}

_ShapeSample
_SampleRect(GfMatrix4f const& xf, GfMatrix3f const& normalXform, float width,
            float height, float u1, float u2)
{
    // Sample rectangle in object space
    const GfVec3f pLight(
      (u1 - 0.5f) * width,
      (u2 - 0.5f) * height,
      0.0f
    );
    const GfVec3f nLight(0.0f, 0.0f, -1.0f);
    const GfVec2f uv(u1, u2);

    // Transform to world space
    const GfVec3f pWorld = xf.Transform(pLight);
    const GfVec3f nWorld = (nLight * normalXform).GetNormalized();

    const float area = _AreaRect(xf, width, height);

    return _ShapeSample {
        pWorld,
        nWorld,
        uv,
        area
    };
}

_ShapeSample
_SampleSphere(GfMatrix4f const& xf, GfMatrix3f const& normalXform, float radius,
              float u1, float u2)
{
    // Sample sphere in light space
    const float z = 1.0 - 2.0 * u1;
    const float r = sqrtf(std::max(0.0f, 1.0f - z*z));
    const float phi = 2.0f * _pi<float> * u2;
    GfVec3f pLight{r * std::cos(phi), r * std::sin(phi), z};
    const GfVec3f nLight = pLight;
    pLight *= radius;
    const GfVec2f uv(u2, z);

    // Transform to world space
    const GfVec3f pWorld = xf.Transform(pLight);
    const GfVec3f nWorld = (nLight * normalXform).GetNormalized();

    const float area = _AreaSphere(xf, radius);

    return _ShapeSample {
        pWorld,
        nWorld,
        uv,
        area
    };
}

bool
_CanSampleSphereBySolidAngle(HdEmbree_LightData const& light)
{
    const GfVec3f x =
        light.xformLightToWorld.TransformDir(GfVec3f::XAxis());
    const GfVec3f y =
        light.xformLightToWorld.TransformDir(GfVec3f::YAxis());
    const GfVec3f z =
        light.xformLightToWorld.TransformDir(GfVec3f::ZAxis());

    const float lx = x.GetLength();
    const float ly = y.GetLength();
    const float lz = z.GetLength();
    const float maxLen = std::max({lx, ly, lz});
    if (maxLen <= 0.0f) {
        return false;
    }

    const float scaleEps = 1.0e-4f * maxLen;
    const float orthoEps = 1.0e-4f * maxLen * maxLen;
    return std::abs(lx - ly) <= scaleEps &&
           std::abs(lx - lz) <= scaleEps &&
           std::abs(GfDot(x, y)) <= orthoEps &&
           std::abs(GfDot(x, z)) <= orthoEps &&
           std::abs(GfDot(y, z)) <= orthoEps;
}

float
_SphereSolidAngle(
    HdEmbree_LightData const& light,
    HdEmbree_Sphere const& sphere,
    GfVec3f const& position)
{
    if (!_CanSampleSphereBySolidAngle(light) || sphere.radius <= 0.0f) {
        return 0.0f;
    }

    const GfVec3f pLight = light.xformWorldToLight.Transform(position);
    const float dist2 = pLight.GetLengthSq();
    const float radius2 = sphere.radius * sphere.radius;
    if (dist2 <= radius2 || !std::isfinite(dist2)) {
        return 0.0f;
    }

    const float sinThetaMax2 = radius2 / dist2;
    const float cosThetaMax =
        sqrtf(std::max(0.0f, 1.0f - sinThetaMax2));
    const float oneMinusCosThetaMax =
        sinThetaMax2 / (1.0f + cosThetaMax);
    const float solidAngle =
        2.0f * _pi<float> * oneMinusCosThetaMax;
    return std::isfinite(solidAngle) ? solidAngle : 0.0f;
}

HdEmbreeLightSampler::LightSample
_EvalSphereLightSolidAngle(
    HdEmbree_LightData const& light,
    HdEmbree_Sphere const& sphere,
    GfVec3f const& position,
    float u1,
    float u2,
    HdEmbreeRenderColorSpace renderColorSpace);

GfVec3f
_SampleDiskPolar(float u1, float u2)
{
    const float r = sqrtf(u1);
    const float theta = 2.0f * _pi<float> * u2;
    return GfVec3f(r * cosf(theta), r * sinf(theta), 0.0f);
}

_ShapeSample
_SampleDisk(GfMatrix4f const& xf, GfMatrix3f const& normalXform, float radius,
            float u1, float u2)
{
    // Sample disk in light space
    GfVec3f pLight = _SampleDiskPolar(u1, u2);
    const GfVec3f nLight(0.0f, 0.0f, -1.0f);
    const GfVec2f uv(pLight[0], pLight[1]);
    pLight *= radius;

    // Transform to world space
    const GfVec3f pWorld = xf.Transform(pLight);
    const GfVec3f nWorld = (nLight * normalXform).GetNormalized();

    const float area = _AreaDisk(xf, radius);

    return _ShapeSample {
        pWorld,
        nWorld,
        uv,
        area
    };
}

_ShapeSample
_SampleCylinder(GfMatrix4f const& xf, GfMatrix3f const& normalXform,
                float radius,float length, float u1, float u2) {
    float z = GfLerp(u1, -length/2.0f, length/2.0f);
    float phi = u2 * 2.0f * _pi<float>;
    // Compute cylinder sample position _pi_ and normal _n_ from $z$ and $\phi$
    GfVec3f pLight = GfVec3f(z, radius * cosf(phi), radius * sinf(phi));
    // Reproject _pObj_ to cylinder surface and compute _pObjError_
    float hitRad = sqrtf(_Sqr(pLight[1]) + _Sqr(pLight[2]));
    pLight[1] *= radius / hitRad;
    pLight[2] *= radius / hitRad;

    GfVec3f nLight(0.0f, pLight[1], pLight[2]);
    nLight.Normalize();

    // Transform to world space
    const GfVec3f pWorld = xf.Transform(pLight);
    const GfVec3f nWorld = (nLight * normalXform).GetNormalized();

    const float area = _AreaCylinder(xf, radius, length);

    return _ShapeSample {
        pWorld,
        nWorld,
        GfVec2f(u2, u1),
        area
    };
}

_ShapeSample
_MakeAreaShapeSample(
    GfMatrix4f const& xf,
    GfMatrix3f const& normalXform,
    GfVec3f const& pLight,
    GfVec3f const& nLight,
    GfVec2f const& uv,
    float area)
{
    return _ShapeSample {
        xf.Transform(pLight),
        (nLight * normalXform).GetNormalized(),
        uv,
        area
    };
}

HdEmbreeLightSampler::LightSample
_InvalidLightSample()
{
    return HdEmbreeLightSampler::LightSample {
        GfVec3f(0.0f),
        GfVec3f(0.0f),
        0.0f,
        0.0f,
        false
    };
}

GfVec3f
_EvalLightBasic(
    HdEmbree_LightData const& light,
    HdEmbreeRenderColorSpace renderColorSpace);

bool
_GetDistantLightDirection(
    HdEmbree_LightData const& light,
    GfVec3f* outDirection)
{
    if (!outDirection) {
        return false;
    }

    const GfVec3f direction =
        light.xformLightToWorld.TransformDir(GfVec3f::ZAxis());
    if (!_IsFinite(direction) || direction.GetLengthSq() <= 0.0f) {
        return false;
    }

    *outDirection = direction.GetNormalized();
    return true;
}

float
_DistantHalfAngleRadians(HdEmbree_Distant const& distant)
{
    const float angle = std::isfinite(distant.angle)
        ? GfClamp(distant.angle, 0.0f, 360.0f)
        : 0.0f;
    const float halfAngle =
        0.5f * static_cast<float>(GfDegreesToRadians(angle));
    return GfClamp(
        halfAngle,
        0.0f,
        _pi<float>);
}

float
_DistantConeSolidAngle(float thetaMax)
{
    if (thetaMax <= 0.0f) {
        return 0.0f;
    }
    return 2.0f * _pi<float> *
        (1.0f - std::cos(GfClamp(thetaMax, 0.0f, _pi<float>)));
}

float
_DistantNormalizeSizeFactor(float thetaMax)
{
    if (thetaMax <= 0.0f) {
        return 1.0f;
    }

    const float sinTheta = std::sin(GfClamp(thetaMax, 0.0f, _pi<float>));
    const float sinTheta2 = sinTheta * sinTheta;
    if (thetaMax <= 0.5f * _pi<float>) {
        return sinTheta2 * _pi<float>;
    }
    return (2.0f - sinTheta2) * _pi<float>;
}

GfVec3f
_EvalDistantLightRadiance(
    HdEmbree_LightData const& light,
    HdEmbree_Distant const& distant,
    HdEmbreeRenderColorSpace renderColorSpace)
{
    GfVec3f radianceIn = _EvalLightBasic(light, renderColorSpace);
    if (light.normalize) {
        const float sizeFactor =
            _DistantNormalizeSizeFactor(_DistantHalfAngleRadians(distant));
        if (sizeFactor > 0.0f) {
            radianceIn /= sizeFactor;
        }
    }
    return radianceIn;
}

HdEmbreeLightSampler::LightSample
_EvaluateDistantLightDirection(
    HdEmbree_LightData const& light,
    HdEmbree_Distant const& distant,
    GfVec3f const& direction,
    HdEmbreeRenderColorSpace renderColorSpace)
{
    if (!_IsFinite(direction) || direction.GetLengthSq() <= 0.0f) {
        return _InvalidLightSample();
    }

    GfVec3f axis;
    if (!_GetDistantLightDirection(light, &axis)) {
        return _InvalidLightSample();
    }

    const float thetaMax = _DistantHalfAngleRadians(distant);
    const GfVec3f omegaInWld = direction.GetNormalized();
    const float cosTheta = GfDot(omegaInWld, axis);
    const GfVec3f radianceIn =
        _EvalDistantLightRadiance(light, distant, renderColorSpace);

    if (thetaMax <= 0.0f) {
        constexpr float directionEps = 1.0e-5f;
        if (cosTheta < 1.0f - directionEps) {
            return _InvalidLightSample();
        }
        return HdEmbreeLightSampler::LightSample{
            radianceIn, axis, std::numeric_limits<float>::max(),
            1.0f,       true, true};
    }

    const float solidAngle = _DistantConeSolidAngle(thetaMax);
    const float cosThetaMax = std::cos(thetaMax);
    constexpr float coneEps = 1.0e-6f;
    if (solidAngle <= 0.0f || cosTheta < cosThetaMax - coneEps) {
        return _InvalidLightSample();
    }

    return HdEmbreeLightSampler::LightSample{
        radianceIn, omegaInWld, std::numeric_limits<float>::max(),
        solidAngle, true,       false};
}

HdEmbreeLightSampler::LightSample
_EvalDistantLight(
    HdEmbree_LightData const& light,
    HdEmbree_Distant const& distant,
    float u1,
    float u2,
    HdEmbreeRenderColorSpace renderColorSpace)
{
    GfVec3f axis;
    if (!_GetDistantLightDirection(light, &axis)) {
        return _InvalidLightSample();
    }

    const float thetaMax = _DistantHalfAngleRadians(distant);
    if (thetaMax <= 0.0f) {
        return _EvaluateDistantLightDirection(
            light, distant, axis, renderColorSpace);
    }

    const float solidAngle = _DistantConeSolidAngle(thetaMax);
    if (solidAngle <= 0.0f) {
        return _InvalidLightSample();
    }

    GfVec3f tangent;
    GfVec3f bitangent;
    GfBuildOrthonormalFrame(axis, &tangent, &bitangent);

    const float cosThetaMax = std::cos(thetaMax);
    const float cosTheta =
        1.0f - _ClampUnit(u1) * (1.0f - cosThetaMax);
    const float sinTheta =
        std::sqrt(std::max(0.0f, 1.0f - _Sqr(cosTheta)));
    const float phi = 2.0f * _pi<float> * _ClampUnit(u2);
    const GfVec3f omegaInWld =
        (tangent * (sinTheta * std::cos(phi)) +
         bitangent * (sinTheta * std::sin(phi)) + axis * cosTheta)
            .GetNormalized();

    return HdEmbreeLightSampler::LightSample{
        _EvalDistantLightRadiance(light, distant, renderColorSpace),
        omegaInWld,
        std::numeric_limits<float>::max(),
        solidAngle,
        true,
        false};
}

HdEmbreeLightSampler::LightSample
_EvaluateDomeLightDirection(
    HdEmbree_LightData const& light,
    GfVec3f const& direction,
    HdEmbreeRenderColorSpace renderColorSpace);

HdEmbreeLightSampler::LightSample
_EvaluateDomeLightDirection(
    HdEmbree_LightData const& light,
    GfVec3f const& direction,
    GfVec3f const& normal,
    HdEmbreeLightSampler::SamplingMode samplingMode,
    HdEmbreeRenderColorSpace renderColorSpace);

bool
_IntersectSphereLight(
    HdEmbree_LightData const& light,
    HdEmbree_Sphere const& sphere,
    GfVec3f const& position,
    GfVec3f const& direction,
    _ShapeSample* outSample);

GfVec3f
_EvalLightBasic(
    HdEmbree_LightData const& light,
    HdEmbreeRenderColorSpace renderColorSpace)
{
    // Our current material model is always 100% diffuse, so diffuse parameter
    // is a straight multiplier
    GfVec3f radianceEmitted = light.color * light.intensity * light.diffuse *
                              powf(2.0f, light.exposure);
    if (light.enableColorTemperature) {
        radianceEmitted =
            GfCompMult(
                radianceEmitted,
                _BlackbodyTemperatureAsRgb(
                    light.colorTemperature, renderColorSpace));
    }
    return radianceEmitted;
}

// TODO: This fixed split is a temporary baseline for finite lights.  When
// MeshLight/arbitrary-emitter sampling is added, replace this with a shared
// emitter sampler that can build a receiver-dependent product proposal, e.g.
// p_area(x) * shaping(x -> shadingPoint), with consistent sample/evaluate PDFs.
constexpr float _ShapingAwareFiniteDirectionalProposalWeight = 0.5f;
constexpr float _ShapingAwareFiniteAreaProposalWeight =
    1.0f - _ShapingAwareFiniteDirectionalProposalWeight;

float
_PdfSolidAngleFromInverse(float pdfSolidAngleInverse)
{
    return (pdfSolidAngleInverse > 0.0f && std::isfinite(pdfSolidAngleInverse))
               ? (1.0f / pdfSolidAngleInverse)
               : 0.0f;
}

float
_WorldToLocalDirectionPdfScale(
    HdEmbree_LightData const& light,
    GfVec3f const& worldDirection,
    GfVec3f* localDirection)
{
    if (!localDirection ||
        worldDirection.GetLengthSq() <= 0.0f ||
        !_IsFinite(worldDirection)) {
        return 0.0f;
    }

    const GfVec3f omegaInWld = worldDirection.GetNormalized();
    const GfVec3f localUnnormalized =
        light.xformWorldToLight.TransformDir(omegaInWld);
    const float localLength = localUnnormalized.GetLength();
    if (localLength <= 0.0f || !std::isfinite(localLength)) {
        return 0.0f;
    }

    const GfVec3f bx =
        light.xformWorldToLight.TransformDir(GfVec3f::XAxis());
    const GfVec3f by =
        light.xformWorldToLight.TransformDir(GfVec3f::YAxis());
    const GfVec3f bz =
        light.xformWorldToLight.TransformDir(GfVec3f::ZAxis());
    const float detWorldToLight = std::abs(GfDot(bx, GfCross(by, bz)));
    if (detWorldToLight <= 0.0f || !std::isfinite(detWorldToLight)) {
        return 0.0f;
    }

    *localDirection = localUnnormalized / localLength;
    return detWorldToLight / (localLength * localLength * localLength);
}

float
_DirectionalShapingPdfSolidAngle(HdEmbree_LightData const& light,
                                 GfVec3f const& worldDirection,
                                 bool foldToFrontHemisphere)
{
    if (!light.shaping.directionalDistribution.IsValid() ||
        worldDirection.GetLengthSq() <= 0.0f ||
        !_IsFinite(worldDirection)) {
        return 0.0f;
    }

    GfVec3f localDirection;
    const float pdfScale = _WorldToLocalDirectionPdfScale(
        light, worldDirection, &localDirection);
    if (pdfScale <= 0.0f) {
        return 0.0f;
    }
    float localPdf =
        HdEmbreeDirectionalShapingPdf(light.shaping, localDirection);
    if (foldToFrontHemisphere) {
        if (localDirection[2] < 0.0f) {
            return 0.0f;
        }
        const GfVec3f mirrored(
            localDirection[0],
            localDirection[1],
            -localDirection[2]);
        localPdf += HdEmbreeDirectionalShapingPdf(light.shaping, mirrored);
    }
    return localPdf * pdfScale;
}

float
_ShapingAwareFinitePdfSolidAngle(HdEmbree_LightData const& light,
                                 float pdfAreaProposalSolidAngleInverse,
                                 GfVec3f const& worldDirection,
                                 bool foldToFrontHemisphere)
{
    const float pdfAreaProposalSolidAngle =
        _PdfSolidAngleFromInverse(pdfAreaProposalSolidAngleInverse);
    if (!light.shaping.directionalDistribution.IsValid()) {
        return pdfAreaProposalSolidAngle;
    }

    // The finite emitter remains the source of truth.  The extra directional
    // proposal only changes how we sample the same emitter, so both proposals
    // are folded into the solid-angle PDF used by direct lighting and emitter
    // hit MIS.
    const float pdfShapingSolidAngle = _DirectionalShapingPdfSolidAngle(
        light, worldDirection, foldToFrontHemisphere);
    return _ShapingAwareFiniteAreaProposalWeight * pdfAreaProposalSolidAngle +
           _ShapingAwareFiniteDirectionalProposalWeight * pdfShapingSolidAngle;
}

void
_ApplyShapingAwareFinitePdf(
    HdEmbree_LightData const& light,
    HdEmbreeLightSampler::LightSample* sample,
    bool foldToFrontHemisphere)
{
    if (!sample || !sample->valid || sample->delta) {
        return;
    }

    const float pdfSolidAngle = _ShapingAwareFinitePdfSolidAngle(
        light, sample->pdfSolidAngleInverse, sample->omegaInWld,
        foldToFrontHemisphere);
    sample->pdfSolidAngleInverse =
        (pdfSolidAngle > 0.0f) ? (1.0f / pdfSolidAngle) : 0.0f;
    sample->valid = sample->valid && sample->pdfSolidAngleInverse > 0.0f;
}

HdEmbreeLightSampler::LightSample
_EvalAreaLight(HdEmbree_LightData const& light, _ShapeSample const& ss,
               GfVec3f const& position,
               HdEmbreeRenderColorSpace renderColorSpace)
{
    // Transform PDF from area measure to solid angle measure. We use the
    // inverse PDF here to avoid division by zero when the surface point is
    // behind the light
    GfVec3f omegaInWld = ss.pWorld - position;
    const float distanceWld = omegaInWld.GetLength();
    if (distanceWld <= 0.0f || !std::isfinite(distanceWld)) {
        return _InvalidLightSample();
    }
    omegaInWld /= distanceWld;
    const float cosThetaOffNormal = _DotZeroClip(-omegaInWld, ss.nWorld);
    float pdfSolidAngleInverse =
        cosThetaOffNormal / _Sqr(distanceWld) * ss.pdfAreaInverse;
    // Combine the brightness parameters to get initial emission luminance
    // (nits)
    GfVec3f radianceEmitted =
        cosThetaOffNormal > 0.0f
            ? _EvalLightBasic(light, renderColorSpace)
            : GfVec3f(0.0f);

    // Multiply by the texture, if there is one
    if (!light.texture.pixels.empty()) {
        const GfVec3f textureColor =
            std::holds_alternative<HdEmbree_Rect>(light.lightVariant)
                ? _SampleRectLightTexture(
                    light.texture, ss.uv, renderColorSpace)
                : _SampleLightTexture(light.texture, ss.uv[0],
                                      1.0f - ss.uv[1], renderColorSpace);
        radianceEmitted = GfCompMult(radianceEmitted, textureColor);
    }

    // If normalize is enabled, we need to divide the luminance by the surface
    // area of the light, which for an area light is equivalent to multiplying
    // by the area pdf, which is itself the reciprocal of the surface area
    if (light.normalize && ss.pdfAreaInverse != 0) {
        radianceEmitted /= ss.pdfAreaInverse;
    }

    const GfVec3f omegaInLocal =
        light.xformWorldToLight.TransformDir(omegaInWld).GetNormalized();
    radianceEmitted = GfCompMult(
        radianceEmitted,
        HdEmbreeEvaluateDirectionalShaping(light.shaping, omegaInLocal));

    return HdEmbreeLightSampler::LightSample{
        radianceEmitted, omegaInWld, distanceWld, pdfSolidAngleInverse,
        pdfSolidAngleInverse > 0.0f && std::isfinite(distanceWld)};
}

HdEmbreeLightSampler::LightSample
_EvalSphereLightSolidAngle(
    HdEmbree_LightData const& light,
    HdEmbree_Sphere const& sphere,
    GfVec3f const& position,
    float u1,
    float u2,
    HdEmbreeRenderColorSpace renderColorSpace)
{
    const float solidAngle = _SphereSolidAngle(light, sphere, position);
    if (solidAngle <= 0.0f) {
        return _InvalidLightSample();
    }

    const GfVec3f pLight = light.xformWorldToLight.Transform(position);
    const GfVec3f axis = (-pLight).GetNormalized();
    GfVec3f tangent;
    GfVec3f bitangent;
    GfBuildOrthonormalFrame(axis, &tangent, &bitangent);

    const float cosThetaMax =
        1.0f - solidAngle / (2.0f * _pi<float>);
    const float cosTheta =
        1.0f - _ClampUnit(u1) * (1.0f - cosThetaMax);
    const float sinTheta =
        sqrtf(std::max(0.0f, 1.0f - _Sqr(cosTheta)));
    const float phi = 2.0f * _pi<float> * _ClampUnit(u2);
    const GfVec3f localDirection =
        (tangent * (sinTheta * cosf(phi)) +
         bitangent * (sinTheta * sinf(phi)) +
         axis * cosTheta).GetNormalized();
    const GfVec3f worldDirection =
        light.xformLightToWorld.TransformDir(localDirection).GetNormalized();

    _ShapeSample shapeSample;
    if (!_IntersectSphereLight(
            light, sphere, position, worldDirection, &shapeSample)) {
        return _InvalidLightSample();
    }

    HdEmbreeLightSampler::LightSample sample =
        _EvalAreaLight(light, shapeSample, position, renderColorSpace);
    sample.pdfSolidAngleInverse = solidAngle;
    sample.valid = sample.valid && sample.pdfSolidAngleInverse > 0.0f;
    return sample;
}

bool
_IntersectRectLight(
    HdEmbree_LightData const& light,
    HdEmbree_Rect const& rect,
    GfVec3f const& position,
    GfVec3f const& direction,
    _ShapeSample* outSample)
{
    if (!outSample) {
        return false;
    }

    const GfVec3f pLight = light.xformWorldToLight.Transform(position);
    const GfVec3f dLight = light.xformWorldToLight.TransformDir(direction);
    if (std::abs(dLight[2]) <= 1.0e-6f) {
        return false;
    }

    const float t = -pLight[2] / dLight[2];
    if (t <= 1.0e-6f || !std::isfinite(t)) {
        return false;
    }

    const GfVec3f hitLight = pLight + dLight * t;
    const float halfWidth = rect.width * 0.5f;
    const float halfHeight = rect.height * 0.5f;
    if (std::abs(hitLight[0]) > halfWidth || std::abs(hitLight[1]) > halfHeight) {
        return false;
    }

    *outSample = _MakeAreaShapeSample(
        light.xformLightToWorld,
        light.normalXformLightToWorld,
        hitLight,
        GfVec3f(0.0f, 0.0f, -1.0f),
        GfVec2f(
            (rect.width != 0.0f) ? (hitLight[0] / rect.width + 0.5f) : 0.5f,
            (rect.height != 0.0f) ? (hitLight[1] / rect.height + 0.5f) : 0.5f),
        _AreaRect(light.xformLightToWorld, rect.width, rect.height));
    return true;
}

bool
_IntersectDiskLight(
    HdEmbree_LightData const& light,
    HdEmbree_Disk const& disk,
    GfVec3f const& position,
    GfVec3f const& direction,
    _ShapeSample* outSample)
{
    if (!outSample) {
        return false;
    }

    const GfVec3f pLight = light.xformWorldToLight.Transform(position);
    const GfVec3f dLight = light.xformWorldToLight.TransformDir(direction);
    if (std::abs(dLight[2]) <= 1.0e-6f) {
        return false;
    }

    const float t = -pLight[2] / dLight[2];
    if (t <= 1.0e-6f || !std::isfinite(t)) {
        return false;
    }

    const GfVec3f hitLight = pLight + dLight * t;
    if (hitLight[0] * hitLight[0] + hitLight[1] * hitLight[1] >
        disk.radius * disk.radius) {
        return false;
    }

    *outSample = _MakeAreaShapeSample(
        light.xformLightToWorld,
        light.normalXformLightToWorld,
        hitLight,
        GfVec3f(0.0f, 0.0f, -1.0f),
        GfVec2f(
            (disk.radius != 0.0f) ? (hitLight[0] / disk.radius) : 0.0f,
            (disk.radius != 0.0f) ? (hitLight[1] / disk.radius) : 0.0f),
        _AreaDisk(light.xformLightToWorld, disk.radius));
    return true;
}

bool
_IntersectSphereLight(
    HdEmbree_LightData const& light,
    HdEmbree_Sphere const& sphere,
    GfVec3f const& position,
    GfVec3f const& direction,
    _ShapeSample* outSample)
{
    if (!outSample) {
        return false;
    }

    const GfVec3f pLight = light.xformWorldToLight.Transform(position);
    const GfVec3f dLight = light.xformWorldToLight.TransformDir(direction);
    const float a = GfDot(dLight, dLight);
    const float b = 2.0f * GfDot(pLight, dLight);
    const float c = GfDot(pLight, pLight) - sphere.radius * sphere.radius;
    const float disc = b * b - 4.0f * a * c;
    if (a <= 0.0f || disc < 0.0f) {
        return false;
    }

    const float sqrtDisc = std::sqrt(disc);
    float t0 = (-b - sqrtDisc) / (2.0f * a);
    float t1 = (-b + sqrtDisc) / (2.0f * a);
    if (t0 > t1) {
        std::swap(t0, t1);
    }
    const float t = (t0 > 1.0e-6f) ? t0 : t1;
    if (t <= 1.0e-6f || !std::isfinite(t)) {
        return false;
    }

    const GfVec3f hitLight = pLight + dLight * t;
    GfVec3f nLight = hitLight;
    if (sphere.radius != 0.0f) {
        nLight /= sphere.radius;
    }
    nLight.Normalize();

    float phi = std::atan2(hitLight[1], hitLight[0]);
    if (phi < 0.0f) {
        phi += 2.0f * _pi<float>;
    }

    *outSample = _MakeAreaShapeSample(
        light.xformLightToWorld,
        light.normalXformLightToWorld,
        hitLight,
        nLight,
        GfVec2f(
            phi / (2.0f * _pi<float>),
            (sphere.radius != 0.0f) ? (hitLight[2] / sphere.radius) : 0.0f),
        _AreaSphere(light.xformLightToWorld, sphere.radius));
    return true;
}

bool
_IntersectCylinderLight(
    HdEmbree_LightData const& light,
    HdEmbree_Cylinder const& cylinder,
    GfVec3f const& position,
    GfVec3f const& direction,
    _ShapeSample* outSample)
{
    if (!outSample) {
        return false;
    }

    const GfVec3f pLight = light.xformWorldToLight.Transform(position);
    const GfVec3f dLight = light.xformWorldToLight.TransformDir(direction);

    const float a = dLight[1] * dLight[1] + dLight[2] * dLight[2];
    const float b = 2.0f * (pLight[1] * dLight[1] + pLight[2] * dLight[2]);
    const float c =
        pLight[1] * pLight[1] + pLight[2] * pLight[2] -
        cylinder.radius * cylinder.radius;
    const float disc = b * b - 4.0f * a * c;
    if (a <= 0.0f || disc < 0.0f) {
        return false;
    }

    const float sqrtDisc = std::sqrt(disc);
    float t0 = (-b - sqrtDisc) / (2.0f * a);
    float t1 = (-b + sqrtDisc) / (2.0f * a);
    if (t0 > t1) {
        std::swap(t0, t1);
    }

    const float halfLength = cylinder.length * 0.5f;
    float t = std::numeric_limits<float>::infinity();
    if (t0 > 1.0e-6f) {
        const float x = pLight[0] + dLight[0] * t0;
        if (x >= -halfLength && x <= halfLength) {
            t = t0;
        }
    }
    if (!std::isfinite(t) && t1 > 1.0e-6f) {
        const float x = pLight[0] + dLight[0] * t1;
        if (x >= -halfLength && x <= halfLength) {
            t = t1;
        }
    }
    if (!std::isfinite(t)) {
        return false;
    }

    const GfVec3f hitLight = pLight + dLight * t;
    GfVec3f nLight(0.0f, hitLight[1], hitLight[2]);
    nLight.Normalize();
    float phi = std::atan2(hitLight[2], hitLight[1]);
    if (phi < 0.0f) {
        phi += 2.0f * _pi<float>;
    }

    *outSample = _MakeAreaShapeSample(
        light.xformLightToWorld,
        light.normalXformLightToWorld,
        hitLight,
        nLight,
        GfVec2f(
            phi / (2.0f * _pi<float>),
            (cylinder.length != 0.0f)
                ? ((hitLight[0] + halfLength) / cylinder.length)
                : 0.0f),
        _AreaCylinder(light.xformLightToWorld, cylinder.radius, cylinder.length));
    return true;
}

HdEmbreeLightSampler::LightSample
_EvaluateLightDirection(
    HdEmbree_LightData const& light,
    GfVec3f const& position,
    GfVec3f const& direction,
    HdEmbreeRenderColorSpace renderColorSpace)
{
    const GfVec3f normalizedDirection = direction.GetNormalized();
    _ShapeSample shapeSample;
    bool hit = false;

    bool useShapingAwareFinitePdf = false;

    if (auto const* rect = std::get_if<HdEmbree_Rect>(&light.lightVariant)) {
        hit = _IntersectRectLight(light, *rect, position, normalizedDirection,
                                  &shapeSample);
        useShapingAwareFinitePdf = true;
    } else if (auto const* sphere =
                   std::get_if<HdEmbree_Sphere>(&light.lightVariant)) {
        hit = _IntersectSphereLight(
            light, *sphere, position, normalizedDirection, &shapeSample);
        if (hit) {
            HdEmbreeLightSampler::LightSample sample =
                _EvalAreaLight(
                    light, shapeSample, position, renderColorSpace);
            const float solidAngle =
                _SphereSolidAngle(light, *sphere, position);
            if (solidAngle > 0.0f) {
                sample.pdfSolidAngleInverse = solidAngle;
                sample.valid =
                    sample.valid && sample.pdfSolidAngleInverse > 0.0f;
            }
            return sample;
        }
    } else if (auto const* disk =
                   std::get_if<HdEmbree_Disk>(&light.lightVariant)) {
        hit = _IntersectDiskLight(
            light, *disk, position, normalizedDirection, &shapeSample);
        useShapingAwareFinitePdf = true;
    } else if (auto const* cylinder =
                   std::get_if<HdEmbree_Cylinder>(&light.lightVariant)) {
        hit = _IntersectCylinderLight(
            light, *cylinder, position, normalizedDirection, &shapeSample);
    } else if (auto const* distant =
                   std::get_if<HdEmbree_Distant>(&light.lightVariant)) {
        return _EvaluateDistantLightDirection(
            light, *distant, normalizedDirection, renderColorSpace);
    } else if (std::holds_alternative<HdEmbree_Dome>(light.lightVariant)) {
        return _EvaluateDomeLightDirection(
            light, normalizedDirection, renderColorSpace);
    }

    if (!hit) {
        return _InvalidLightSample();
    }

    HdEmbreeLightSampler::LightSample sample =
        _EvalAreaLight(light, shapeSample, position, renderColorSpace);
    if (useShapingAwareFinitePdf) {
        _ApplyShapingAwareFinitePdf(light, &sample, true);
    }
    return sample;
}

HdEmbreeLightSampler::LightSample
_SampleRectDirectionalShaping(
    HdEmbree_LightData const& light,
    HdEmbree_Rect const& rect,
    GfVec3f const& position,
    float u1,
    float u2,
    HdEmbreeRenderColorSpace renderColorSpace)
{
    const HdEmbree_DirectionalShapingSample directionalSample =
        HdEmbreeSampleDirectionalShaping(light.shaping, u1, u2);
    if (!directionalSample.valid) {
        return _InvalidLightSample();
    }

    GfVec3f localDirection = directionalSample.localDirection;
    if (localDirection[2] < 0.0f) {
        localDirection[2] = -localDirection[2];
    }

    const GfVec3f worldDirection =
        light.xformLightToWorld.TransformDir(localDirection).GetNormalized();
    _ShapeSample shapeSample;
    if (!_IntersectRectLight(light, rect, position, worldDirection,
                             &shapeSample)) {
        return _InvalidLightSample();
    }

    HdEmbreeLightSampler::LightSample sample =
        _EvalAreaLight(light, shapeSample, position, renderColorSpace);
    _ApplyShapingAwareFinitePdf(light, &sample, true);
    return sample;
}

HdEmbreeLightSampler::LightSample
_SampleDiskDirectionalShaping(
    HdEmbree_LightData const& light,
    HdEmbree_Disk const& disk,
    GfVec3f const& position,
    float u1,
    float u2,
    HdEmbreeRenderColorSpace renderColorSpace)
{
    const HdEmbree_DirectionalShapingSample directionalSample =
        HdEmbreeSampleDirectionalShaping(light.shaping, u1, u2);
    if (!directionalSample.valid) {
        return _InvalidLightSample();
    }

    GfVec3f localDirection = directionalSample.localDirection;
    if (localDirection[2] < 0.0f) {
        localDirection[2] = -localDirection[2];
    }

    const GfVec3f worldDirection =
        light.xformLightToWorld.TransformDir(localDirection).GetNormalized();
    _ShapeSample shapeSample;
    if (!_IntersectDiskLight(light, disk, position, worldDirection,
                             &shapeSample)) {
        return _InvalidLightSample();
    }

    HdEmbreeLightSampler::LightSample sample =
        _EvalAreaLight(light, shapeSample, position, renderColorSpace);
    _ApplyShapingAwareFinitePdf(light, &sample, true);
    return sample;
}

HdEmbreeLightSampler::LightSample
_EvaluateDomeLightDirection(
    HdEmbree_LightData const& light,
    GfVec3f const& direction,
    HdEmbreeRenderColorSpace renderColorSpace)
{
    if (!_IsFinite(direction) || direction.GetLengthSq() <= 0.0f) {
        return _InvalidLightSample();
    }

    const GfVec3f normalizedDirection = direction.GetNormalized();
    const GfVec3f localDirection =
        light.xformWorldToLight.TransformDir(normalizedDirection).GetNormalized();
    const GfVec2f uv = _DirectionToLatLongUv(localDirection);

    GfVec3f radianceIn = light.texture.pixels.empty()
                             ? GfVec3f(1.0f)
                             : _SampleLightTexture(
                                   light.texture, uv[0], uv[1],
                                   renderColorSpace);

    // Apply LightAPI radiometric parameters (intensity, exposure, color,
    // color temperature) consistently with area lights.
    radianceIn =
        GfCompMult(radianceIn, _EvalLightBasic(light, renderColorSpace));

    const float pdfSolidAngle = _DomeDirectionalPdf(light, uv);

    return HdEmbreeLightSampler::LightSample{
        radianceIn, normalizedDirection, std::numeric_limits<float>::max(),
        (pdfSolidAngle > 0.0f) ? (1.0f / pdfSolidAngle) : 0.0f,
        pdfSolidAngle > 0.0f};
}

HdEmbreeLightSampler::LightSample
_EvaluateDomeLightDirection(
    HdEmbree_LightData const& light,
    GfVec3f const& direction,
    GfVec3f const& normal,
    HdEmbreeLightSampler::SamplingMode samplingMode,
    HdEmbreeRenderColorSpace renderColorSpace)
{
    if (samplingMode !=
        HdEmbreeLightSampler::SamplingMode::ReflectionHemisphere) {
        return _EvaluateDomeLightDirection(
            light, direction, renderColorSpace);
    }

    if (!_IsFinite(direction) || direction.GetLengthSq() <= 0.0f) {
        return _InvalidLightSample();
    }

    const GfVec3f normalizedDirection = direction.GetNormalized();
    const GfVec3f localDirection =
        light.xformWorldToLight.TransformDir(normalizedDirection).GetNormalized();
    const GfVec2f uv = _DirectionToLatLongUv(localDirection);

    GfVec3f radianceIn = light.texture.pixels.empty()
                             ? GfVec3f(1.0f)
                             : _SampleLightTexture(
                                   light.texture, uv[0], uv[1],
                                   renderColorSpace);
    radianceIn =
        GfCompMult(radianceIn, _EvalLightBasic(light, renderColorSpace));

    const float pdfSolidAngle =
        _ReflectionHemispherePdf(light, normal, normalizedDirection);

    return HdEmbreeLightSampler::LightSample{
        radianceIn, normalizedDirection, std::numeric_limits<float>::max(),
        (pdfSolidAngle > 0.0f) ? (1.0f / pdfSolidAngle) : 0.0f,
        pdfSolidAngle > 0.0f};
}

HdEmbreeLightSampler::LightSample
_EvalDomeLight(HdEmbree_LightData const& light, GfVec3f const& normal,
               float u1, float u2,
               HdEmbreeLightSampler::SamplingMode samplingMode,
               HdEmbreeRenderColorSpace renderColorSpace)
{
    GfVec3f worldDirection;
    if (!_HasDomeDistribution(light.texture)) {
        const float localY = 1.0f - 2.0f * _ClampUnit(u1);
        const float localR = sqrtf(std::max(0.0f, 1.0f - _Sqr(localY)));
        const float phi = 2.0f * _pi<float> * _ClampUnit(u2);
        const GfVec3f localDirection(
            localR * sinf(phi),
            localY,
            localR * cosf(phi));
        worldDirection =
            light.xformLightToWorld.TransformDir(localDirection).GetNormalized();
    } else {
        const GfVec2f uv = _SampleDomeUv(light.texture, u1, u2);
        const GfVec3f localDirection = _LatLongUvToDirection(uv);
        worldDirection =
            light.xformLightToWorld.TransformDir(localDirection).GetNormalized();
    }

    if (samplingMode ==
        HdEmbreeLightSampler::SamplingMode::ReflectionHemisphere) {
        const GfVec3f hemisphereDirection =
            _FoldDirectionToReflectionHemisphere(worldDirection, normal);
        if (hemisphereDirection.GetLengthSq() > 0.0f) {
            return _EvaluateDomeLightDirection(
                light, hemisphereDirection, normal, samplingMode,
                renderColorSpace);
        }
    }

    return _EvaluateDomeLightDirection(
        light, worldDirection, renderColorSpace);
}

} // namespace ""

PXR_NAMESPACE_OPEN_SCOPE

HdEmbreeLightSampler::LightSample
HdEmbreeLightSampler::GetLightSample(HdEmbree_LightData const& lightData,
                                     GfVec3f const& positionHitWld,
                                     GfVec3f const& normalShdWldOut, float u1,
                                     float u2, SamplingMode samplingMode,
                                     HdEmbreeRenderColorSpace renderColorSpace)
{
    HdEmbreeLightSampler lightSampler(lightData, positionHitWld,
                                      normalShdWldOut, u1, u2, samplingMode,
                                      renderColorSpace);
    return std::visit(lightSampler, lightData.lightVariant);
}

HdEmbreeLightSampler::LightSample
HdEmbreeLightSampler::EvaluateDomeLightDirection(
    HdEmbree_LightData const& lightData, GfVec3f const& omegaInWld,
    HdEmbreeRenderColorSpace renderColorSpace)
{
    return _EvaluateDomeLightDirection(
        lightData, omegaInWld, renderColorSpace);
}

HdEmbreeLightSampler::LightSample
HdEmbreeLightSampler::EvaluateDomeLightDirection(
    HdEmbree_LightData const& lightData, GfVec3f const& omegaInWld,
    GfVec3f const& normalShdWldOut, SamplingMode samplingMode,
    HdEmbreeRenderColorSpace renderColorSpace)
{
    return _EvaluateDomeLightDirection(lightData, omegaInWld, normalShdWldOut,
                                       samplingMode, renderColorSpace);
}

HdEmbreeLightSampler::LightSample
HdEmbreeLightSampler::EvaluateLightDirection(
    HdEmbree_LightData const& lightData, GfVec3f const& positionHitWld,
    GfVec3f const& omegaInWld,
    HdEmbreeRenderColorSpace renderColorSpace)
{
    return _EvaluateLightDirection(
        lightData, positionHitWld, omegaInWld, renderColorSpace);
}

HdEmbreeLightSampler::LightSample HdEmbreeLightSampler::operator()(
        HdEmbree_UnknownLight const& unk) {
    // Could warn, but we should have already warned when lightVariant
    // first created / set to HdEmbree_UnknownLight... and warning here
    // could result in a LOT of spam
    return HdEmbreeLightSampler::LightSample {
        GfVec3f(0.0f),
        GfVec3f(0.0f),
        0.0f,
        0.0f,
        false,
    };
}

HdEmbreeLightSampler::LightSample HdEmbreeLightSampler::operator()(
        HdEmbree_Rect const& rect) {
    const bool useShapingAwareSampling =
        _lightData.shaping.directionalDistribution.IsValid();
    if (useShapingAwareSampling &&
        _u1 >= _ShapingAwareFiniteAreaProposalWeight) {
        return _SampleRectDirectionalShaping(
            _lightData, rect, _positionHitWld,
            (_u1 - _ShapingAwareFiniteAreaProposalWeight) /
                _ShapingAwareFiniteDirectionalProposalWeight,
            _u2,
            _renderColorSpace);
    }

    _ShapeSample shapeSample = _SampleRect(
            _lightData.xformLightToWorld,
            _lightData.normalXformLightToWorld,
            rect.width,
            rect.height,
            useShapingAwareSampling
                ? (_u1 / _ShapingAwareFiniteAreaProposalWeight)
                : _u1,
            _u2);
    HdEmbreeLightSampler::LightSample sample =
        _EvalAreaLight(
            _lightData, shapeSample, _positionHitWld, _renderColorSpace);
    if (useShapingAwareSampling) {
        _ApplyShapingAwareFinitePdf(_lightData, &sample, true);
    }
    return sample;
}

HdEmbreeLightSampler::LightSample HdEmbreeLightSampler::operator()(
        HdEmbree_Sphere const& sphere) {
    const HdEmbreeLightSampler::LightSample solidAngleSample =
        _EvalSphereLightSolidAngle(_lightData, sphere, _positionHitWld, _u1,
                                   _u2, _renderColorSpace);
    if (solidAngleSample.valid) {
        return solidAngleSample;
    }

    _ShapeSample shapeSample = _SampleSphere(
            _lightData.xformLightToWorld,
            _lightData.normalXformLightToWorld,
            sphere.radius,
            _u1,
            _u2);
    return _EvalAreaLight(
        _lightData, shapeSample, _positionHitWld, _renderColorSpace);
}

HdEmbreeLightSampler::LightSample HdEmbreeLightSampler::operator()(
        HdEmbree_Disk const& disk) {
    const bool useShapingAwareSampling =
        _lightData.shaping.directionalDistribution.IsValid();
    if (useShapingAwareSampling &&
        _u1 >= _ShapingAwareFiniteAreaProposalWeight) {
        return _SampleDiskDirectionalShaping(
            _lightData, disk, _positionHitWld,
            (_u1 - _ShapingAwareFiniteAreaProposalWeight) /
                _ShapingAwareFiniteDirectionalProposalWeight,
            _u2,
            _renderColorSpace);
    }

    _ShapeSample shapeSample = _SampleDisk(
            _lightData.xformLightToWorld,
            _lightData.normalXformLightToWorld,
            disk.radius,
            useShapingAwareSampling
                ? (_u1 / _ShapingAwareFiniteAreaProposalWeight)
                : _u1,
            _u2);
    HdEmbreeLightSampler::LightSample sample =
        _EvalAreaLight(
            _lightData, shapeSample, _positionHitWld, _renderColorSpace);
    if (useShapingAwareSampling) {
        _ApplyShapingAwareFinitePdf(_lightData, &sample, true);
    }
    return sample;
}

HdEmbreeLightSampler::LightSample HdEmbreeLightSampler::operator()(
        HdEmbree_Distant const& distant) {
    return _EvalDistantLight(
        _lightData, distant, _u1, _u2, _renderColorSpace);
}

HdEmbreeLightSampler::LightSample HdEmbreeLightSampler::operator()(
        HdEmbree_Cylinder const& cylinder) {
    _ShapeSample shapeSample = _SampleCylinder(
            _lightData.xformLightToWorld,
            _lightData.normalXformLightToWorld,
            cylinder.radius,
            cylinder.length,
            _u1,
            _u2);
    return _EvalAreaLight(
        _lightData, shapeSample, _positionHitWld, _renderColorSpace);
}

HdEmbreeLightSampler::LightSample HdEmbreeLightSampler::operator()(
        HdEmbree_Dome const& dome) {
    return _EvalDomeLight(_lightData, _normalShdWldOut, _u1, _u2,
                          _samplingMode, _renderColorSpace);
}

PXR_NAMESPACE_CLOSE_SCOPE
