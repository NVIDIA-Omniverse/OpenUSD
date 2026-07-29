//
// Copyright 2025 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
// Samples and evaluates sphere lights through solid-angle or area geometry.
//
#include "lightSamplerCommon.h"
#include "lightSamplerDispatch.h"

#include "pxr/base/gf/matrix3f.h"
#include "pxr/base/gf/matrix4f.h"
#include "pxr/base/gf/vec2f.h"
#include "pxr/base/gf/vec3f.h"

#include <algorithm>
#include <cmath>

PXR_NAMESPACE_OPEN_SCOPE

static float
_AreaSphere(GfMatrix4f const& xf, float radius)
{
    // Approximate transformed sphere area as an ellipsoid.
    const float a =
        xf.TransformDir(GfVec3f{radius, 0.0f, 0.0f}).GetLength();
    const float b =
        xf.TransformDir(GfVec3f{0.0f, radius, 0.0f}).GetLength();
    const float c =
        xf.TransformDir(GfVec3f{0.0f, 0.0f, radius}).GetLength();
    const float ab = powf(a * b, 1.6f);
    const float ac = powf(a * c, 1.6f);
    const float bc = powf(b * c, 1.6f);
    return powf((ab + ac + bc) / 3.0f, 1.0f / 1.6f) *
           4.0f * ty::Pi<float>;
}

static ty::ShapeSample
_SampleSphere(GfMatrix4f const& xf, GfMatrix3f const& normalXform, float radius,
              float u1, float u2)
{
    const float z = 1.0 - 2.0 * u1;
    const float r = sqrtf(std::max(0.0f, 1.0f - z * z));
    const float phi = 2.0f * ty::Pi<float> * u2;
    GfVec3f pLight{r * std::cos(phi), r * std::sin(phi), z};
    const GfVec3f nLight = pLight;
    pLight *= radius;
    return ty::ShapeSample{
        xf.Transform(pLight), (nLight * normalXform).GetNormalized(),
        GfVec2f(u2, z), _AreaSphere(xf, radius)};
}

static bool
_CanSampleSphereBySolidAngle(ty::LightData const& light)
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

    // Solid-angle sampling assumes a uniformly scaled orthogonal transform.
    const float scaleEps = 1.0e-4f * maxLen;
    const float orthoEps = 1.0e-4f * maxLen * maxLen;
    return std::abs(lx - ly) <= scaleEps &&
           std::abs(lx - lz) <= scaleEps &&
           std::abs(GfDot(x, y)) <= orthoEps &&
           std::abs(GfDot(x, z)) <= orthoEps &&
           std::abs(GfDot(y, z)) <= orthoEps;
}

static float
_SphereSolidAngle(
    ty::LightData const& light, ty::SphereLight const& sphere,
    GfVec3f const& position)
{
    if (!_CanSampleSphereBySolidAngle(light) ||
        sphere.radius <= 0.0f) {
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
        2.0f * ty::Pi<float> * oneMinusCosThetaMax;
    return std::isfinite(solidAngle) ? solidAngle : 0.0f;
}

static bool
_IntersectSphereLight(
    ty::LightData const& light, ty::SphereLight const& sphere,
    GfVec3f const& position, GfVec3f const& direction,
    ty::ShapeSample* outSample)
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
        phi += 2.0f * ty::Pi<float>;
    }

    *outSample = ty::MakeAreaShapeSample(
        light.xformLightToWorld, light.normalXformLightToWorld, hitLight, nLight,
        GfVec2f(
            phi / (2.0f * ty::Pi<float>),
            (sphere.radius != 0.0f)
                ? (hitLight[2] / sphere.radius)
                : 0.0f),
        _AreaSphere(light.xformLightToWorld, sphere.radius));
    return true;
}

static ty::LightSampler::LightSample
_EvalSphereLightSolidAngle(
    ty::LightData const& light, ty::SphereLight const& sphere,
    GfVec3f const& position, float u1, float u2,
    ty::RenderColorSpace renderColorSpace)
{
    const float solidAngle =
        _SphereSolidAngle(light, sphere, position);
    if (solidAngle <= 0.0f) {
        return ty::InvalidLightSample();
    }

    const GfVec3f pLight = light.xformWorldToLight.Transform(position);
    const GfVec3f axis = (-pLight).GetNormalized();
    GfVec3f tangent;
    GfVec3f bitangent;
    GfBuildOrthonormalFrame(axis, &tangent, &bitangent);

    const float cosThetaMax =
        1.0f - solidAngle / (2.0f * ty::Pi<float>);
    const float cosTheta =
        1.0f - ty::ClampUnitHalfOpen(u1) * (1.0f - cosThetaMax);
    const float sinTheta =
        sqrtf(std::max(0.0f, 1.0f - ty::Sqr(cosTheta)));
    const float phi =
        2.0f * ty::Pi<float> * ty::ClampUnitHalfOpen(u2);
    const GfVec3f localDirection =
        (tangent * (sinTheta * cosf(phi)) +
         bitangent * (sinTheta * sinf(phi)) +
         axis * cosTheta).GetNormalized();
    const GfVec3f worldDirection =
        light.xformLightToWorld.TransformDir(
            localDirection).GetNormalized();

    ty::ShapeSample shapeSample;
    if (!_IntersectSphereLight(
            light, sphere, position, worldDirection, &shapeSample)) {
        return ty::InvalidLightSample();
    }

    ty::LightSampler::LightSample sample =
        ty::EvalAreaLight(light, shapeSample, position, renderColorSpace);
    sample.pdfSolidAngleInverse = solidAngle;
    sample.valid = sample.valid && sample.pdfSolidAngleInverse > 0.0f;
    return sample;
}

ty::LightSampler::LightSample
ty::SampleSphereLight(
    ty::LightData const& light, ty::SphereLight const& sphere,
    GfVec3f const& position, float u1, float u2,
    ty::RenderColorSpace renderColorSpace)
{
    const ty::LightSampler::LightSample solidAngleSample =
        _EvalSphereLightSolidAngle(
            light, sphere, position, u1, u2, renderColorSpace);
    if (solidAngleSample.valid) {
        return solidAngleSample;
    }

    ty::ShapeSample shapeSample = _SampleSphere(
        light.xformLightToWorld, light.normalXformLightToWorld,
        sphere.radius, u1, u2);
    return ty::EvalAreaLight(light, shapeSample, position, renderColorSpace);
}

ty::LightSampler::LightSample
ty::EvaluateSphereLightDirection(
    ty::LightData const& light, ty::SphereLight const& sphere,
    GfVec3f const& position, GfVec3f const& direction,
    ty::RenderColorSpace renderColorSpace)
{
    ty::ShapeSample shapeSample;
    if (!_IntersectSphereLight(
            light, sphere, position, direction.GetNormalized(), &shapeSample)) {
        return ty::InvalidLightSample();
    }

    ty::LightSampler::LightSample sample =
        ty::EvalAreaLight(light, shapeSample, position, renderColorSpace);
    const float solidAngle =
        _SphereSolidAngle(light, sphere, position);
    if (solidAngle > 0.0f) {
        sample.pdfSolidAngleInverse = solidAngle;
        sample.valid = sample.valid && sample.pdfSolidAngleInverse > 0.0f;
    }
    return sample;
}

PXR_NAMESPACE_CLOSE_SCOPE
