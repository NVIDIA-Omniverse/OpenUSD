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
_AreaSphere(GfMatrix4f const& lightToWorld, float radius)
{
    // Approximate transformed sphere area as an ellipsoid.
    const float radiusAxisXWld =
        lightToWorld.TransformDir(GfVec3f{radius, 0.0f, 0.0f}).GetLength();
    const float radiusAxisYWld =
        lightToWorld.TransformDir(GfVec3f{0.0f, radius, 0.0f}).GetLength();
    const float radiusAxisZWld =
        lightToWorld.TransformDir(GfVec3f{0.0f, 0.0f, radius}).GetLength();
    const float areaPowerXY =
        powf(radiusAxisXWld * radiusAxisYWld, 1.6f);
    const float areaPowerXZ =
        powf(radiusAxisXWld * radiusAxisZWld, 1.6f);
    const float areaPowerYZ =
        powf(radiusAxisYWld * radiusAxisZWld, 1.6f);
    return powf(
               (areaPowerXY + areaPowerXZ + areaPowerYZ) / 3.0f,
               1.0f / 1.6f) *
           4.0f * ty::Pi<float>;
}

static ty::ShapeSample
_SampleSphere(
    GfMatrix4f const& lightToWorld,
    GfMatrix3f const& normalLightToWorld,
    float radius, float u1, float u2)
{
    const float coordinateSphereZ = 1.0 - 2.0 * u1;
    const float radiusSphereXy = sqrtf(
        std::max(
            0.0f,
            1.0f - coordinateSphereZ * coordinateSphereZ));
    const float phi = 2.0f * ty::Pi<float> * u2;
    GfVec3f posLight{
        radiusSphereXy * std::cos(phi),
        radiusSphereXy * std::sin(phi),
        coordinateSphereZ};
    const GfVec3f normalGeomLightExt = posLight;
    posLight *= radius;
    return ty::ShapeSample{
        lightToWorld.Transform(posLight),
        (normalGeomLightExt * normalLightToWorld).GetNormalized(),
        GfVec2f(u2, coordinateSphereZ), _AreaSphere(lightToWorld, radius)};
}

static bool
_CanSampleSphereBySolidAngle(ty::LightData const& light)
{
    const GfVec3f axisXWld =
        light.xformLightToWorld.TransformDir(GfVec3f::XAxis());
    const GfVec3f axisYWld =
        light.xformLightToWorld.TransformDir(GfVec3f::YAxis());
    const GfVec3f axisZWld =
        light.xformLightToWorld.TransformDir(GfVec3f::ZAxis());

    const float lengthAxisXWld = axisXWld.GetLength();
    const float lengthAxisYWld = axisYWld.GetLength();
    const float lengthAxisZWld = axisZWld.GetLength();
    const float maximumAxisLengthWld =
        std::max({lengthAxisXWld, lengthAxisYWld, lengthAxisZWld});
    if (maximumAxisLengthWld <= 0.0f) {
        return false;
    }

    // Solid-angle sampling assumes a uniformly scaled orthogonal transform.
    const float scaleEpsilon = 1.0e-4f * maximumAxisLengthWld;
    const float orthogonalityEpsilon =
        1.0e-4f * maximumAxisLengthWld * maximumAxisLengthWld;
    return std::abs(lengthAxisXWld - lengthAxisYWld) <= scaleEpsilon &&
           std::abs(lengthAxisXWld - lengthAxisZWld) <= scaleEpsilon &&
           std::abs(GfDot(axisXWld, axisYWld)) <= orthogonalityEpsilon &&
           std::abs(GfDot(axisXWld, axisZWld)) <= orthogonalityEpsilon &&
           std::abs(GfDot(axisYWld, axisZWld)) <= orthogonalityEpsilon;
}

static float
_SphereSolidAngle(
    ty::LightData const& light, ty::SphereLight const& sphere,
    GfVec3f const& posWld)
{
    if (!_CanSampleSphereBySolidAngle(light) ||
        sphere.radius <= 0.0f) {
        return 0.0f;
    }

    const GfVec3f posLight = light.xformWorldToLight.Transform(posWld);
    const float dist2 = posLight.GetLengthSq();
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
    GfVec3f const& posWld, GfVec3f const& dirWld,
    ty::ShapeSample* outSample)
{
    if (!outSample) {
        return false;
    }

    const GfVec3f posLight = light.xformWorldToLight.Transform(posWld);
    const GfVec3f dirLight = light.xformWorldToLight.TransformDir(dirWld);
    const float quadraticCoefficientA =
        GfDot(dirLight, dirLight);
    const float quadraticCoefficientB =
        2.0f * GfDot(posLight, dirLight);
    const float quadraticCoefficientC =
        GfDot(posLight, posLight) -
        sphere.radius * sphere.radius;
    const float discriminant =
        quadraticCoefficientB * quadraticCoefficientB -
        4.0f * quadraticCoefficientA * quadraticCoefficientC;
    if (quadraticCoefficientA <= 0.0f || discriminant < 0.0f) {
        return false;
    }

    const float sqrtDiscriminant = std::sqrt(discriminant);
    float rayParameterNear = (-quadraticCoefficientB - sqrtDiscriminant) /
        (2.0f * quadraticCoefficientA);
    float rayParameterFar = (-quadraticCoefficientB + sqrtDiscriminant) /
        (2.0f * quadraticCoefficientA);
    if (rayParameterNear > rayParameterFar) {
        std::swap(rayParameterNear, rayParameterFar);
    }
    const float t =
        (rayParameterNear > 1.0e-6f) ? rayParameterNear : rayParameterFar;
    if (t <= 1.0e-6f || !std::isfinite(t)) {
        return false;
    }

    const GfVec3f posHitLight = posLight + dirLight * t;
    GfVec3f normalGeomLightExt = posHitLight;
    if (sphere.radius != 0.0f) {
        normalGeomLightExt /= sphere.radius;
    }
    normalGeomLightExt.Normalize();

    float phi = std::atan2(posHitLight[1], posHitLight[0]);
    if (phi < 0.0f) {
        phi += 2.0f * ty::Pi<float>;
    }

    *outSample = ty::MakeAreaShapeSample(
        light.xformLightToWorld, light.normalXformLightToWorld,
        posHitLight, normalGeomLightExt,
        GfVec2f(
            phi / (2.0f * ty::Pi<float>),
            (sphere.radius != 0.0f)
                ? (posHitLight[2] / sphere.radius)
                : 0.0f),
        _AreaSphere(light.xformLightToWorld, sphere.radius));
    return true;
}

static ty::LightSampler::LightSample
_EvalSphereLightSolidAngle(
    ty::LightData const& light, ty::SphereLight const& sphere,
    GfVec3f const& posWld, float u1, float u2,
    ty::RenderColorSpace renderColorSpace)
{
    const float solidAngle =
        _SphereSolidAngle(light, sphere, posWld);
    if (solidAngle <= 0.0f) {
        return ty::InvalidLightSample();
    }

    const GfVec3f posLight = light.xformWorldToLight.Transform(posWld);
    const GfVec3f axis = (-posLight).GetNormalized();
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
    const GfVec3f dirLight =
        (tangent * (sinTheta * cosf(phi)) +
         bitangent * (sinTheta * sinf(phi)) +
         axis * cosTheta).GetNormalized();
    const GfVec3f dirWld =
        light.xformLightToWorld.TransformDir(
            dirLight).GetNormalized();

    ty::ShapeSample shapeSample;
    if (!_IntersectSphereLight(
            light, sphere, posWld, dirWld, &shapeSample)) {
        return ty::InvalidLightSample();
    }

    ty::LightSampler::LightSample sample =
        ty::EvalAreaLight(light, shapeSample, posWld, renderColorSpace);
    sample.pdfSolidAngleInverse = solidAngle;
    sample.valid = sample.valid && sample.pdfSolidAngleInverse > 0.0f;
    return sample;
}

ty::LightSampler::LightSample
ty::SampleSphereLight(
    ty::LightData const& light, ty::SphereLight const& sphere,
    GfVec3f const& posWld, float u1, float u2,
    ty::RenderColorSpace renderColorSpace)
{
    const ty::LightSampler::LightSample solidAngleSample =
        _EvalSphereLightSolidAngle(
            light, sphere, posWld, u1, u2, renderColorSpace);
    if (solidAngleSample.valid) {
        return solidAngleSample;
    }

    ty::ShapeSample shapeSample = _SampleSphere(
        light.xformLightToWorld, light.normalXformLightToWorld,
        sphere.radius, u1, u2);
    return ty::EvalAreaLight(light, shapeSample, posWld, renderColorSpace);
}

ty::LightSampler::LightSample
ty::EvaluateSphereLightDirection(
    ty::LightData const& light, ty::SphereLight const& sphere,
    GfVec3f const& posWld, GfVec3f const& dirWld,
    ty::RenderColorSpace renderColorSpace)
{
    ty::ShapeSample shapeSample;
    if (!_IntersectSphereLight(
            light, sphere, posWld, dirWld.GetNormalized(), &shapeSample)) {
        return ty::InvalidLightSample();
    }

    ty::LightSampler::LightSample sample =
        ty::EvalAreaLight(light, shapeSample, posWld, renderColorSpace);
    const float solidAngle =
        _SphereSolidAngle(light, sphere, posWld);
    if (solidAngle > 0.0f) {
        sample.pdfSolidAngleInverse = solidAngle;
        sample.valid = sample.valid && sample.pdfSolidAngleInverse > 0.0f;
    }
    return sample;
}

PXR_NAMESPACE_CLOSE_SCOPE
