//
// Copyright 2025 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
// Samples and evaluates an open cylinder light's lateral geometry.
//
#include "lightSamplerCommon.h"
#include "lightSamplerDispatch.h"

#include "pxr/base/gf/matrix3f.h"
#include "pxr/base/gf/matrix4f.h"
#include "pxr/base/gf/vec2f.h"
#include "pxr/base/gf/vec3f.h"

#include <algorithm>
#include <cmath>
#include <limits>

PXR_NAMESPACE_OPEN_SCOPE

static float
_AreaCylinder(GfMatrix4f const& lightToWorld, float radius, float length)
{
    const float lengthWld =
        lightToWorld.TransformDir(GfVec3f{length, 0.0f, 0.0f}).GetLength();
    const float radiusAxisYWld =
        lightToWorld.TransformDir(GfVec3f{0.0f, radius, 0.0f}).GetLength();
    const float radiusAxisZWld =
        lightToWorld.TransformDir(GfVec3f{0.0f, 0.0f, radius}).GetLength();
    // Ramanujan's ellipse-perimeter approximation preserves the established
    // transformed-cylinder area estimate.
    const float perimeterWld =
        ty::Pi *
        (3.0f * (radiusAxisYWld + radiusAxisZWld) -
         sqrtf((3.0f * radiusAxisYWld + radiusAxisZWld) *
               (radiusAxisYWld + 3.0f * radiusAxisZWld)));
    return perimeterWld * lengthWld;
}

static ty::ShapeSample
_SampleCylinder(
    GfMatrix4f const& lightToWorld,
    GfMatrix3f const& normalLightToWorld,
    float radius, float length, float u1, float u2)
{
    const float posAxisLight =
        GfLerp(u1, -length / 2.0f, length / 2.0f);
    const float phi = u2 * 2.0f * ty::Pi;
    GfVec3f posLight(
        posAxisLight, radius * cosf(phi), radius * sinf(phi));

    // Keep the sampled point exactly on the lateral surface despite
    // trigonometric roundoff.
    const float radiusHit =
        sqrtf(ty::Sqr(posLight[1]) + ty::Sqr(posLight[2]));
    posLight[1] *= radius / radiusHit;
    posLight[2] *= radius / radiusHit;

    GfVec3f normalGeomLightExt(0.0f, posLight[1], posLight[2]);
    normalGeomLightExt.Normalize();

    return ty::ShapeSample{
        lightToWorld.Transform(posLight),
        (normalGeomLightExt * normalLightToWorld).GetNormalized(),
        GfVec2f(u2, u1), _AreaCylinder(lightToWorld, radius, length)};
}

static bool
_IntersectCylinderLight(
    ty::LightData const& light, ty::CylinderLight const& cylinder,
    GfVec3f const& posWld, GfVec3f const& dirWld,
    ty::ShapeSample* outSample)
{
    if (!outSample) {
        return false;
    }

    const GfVec3f posLight = light.xformWorldToLight.Transform(posWld);
    const GfVec3f dirLight = light.xformWorldToLight.TransformDir(dirWld);

    const float quadraticCoefficientA =
        dirLight[1] * dirLight[1] + dirLight[2] * dirLight[2];
    const float quadraticCoefficientB =
        2.0f * (posLight[1] * dirLight[1] + posLight[2] * dirLight[2]);
    const float quadraticCoefficientC =
        posLight[1] * posLight[1] + posLight[2] * posLight[2] -
        cylinder.radius * cylinder.radius;
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

    // Accept only roots on the open lateral surface, excluding both end caps.
    const float halfLength = cylinder.length * 0.5f;
    float t = std::numeric_limits<float>::infinity();
    if (rayParameterNear > 1.0e-6f) {
        const float posAxisLight =
            posLight[0] + dirLight[0] * rayParameterNear;
        if (posAxisLight >= -halfLength &&
            posAxisLight <= halfLength) {
            t = rayParameterNear;
        }
    }
    if (!std::isfinite(t) && rayParameterFar > 1.0e-6f) {
        const float posAxisLight =
            posLight[0] + dirLight[0] * rayParameterFar;
        if (posAxisLight >= -halfLength &&
            posAxisLight <= halfLength) {
            t = rayParameterFar;
        }
    }
    if (!std::isfinite(t)) {
        return false;
    }

    const GfVec3f posHitLight = posLight + dirLight * t;
    GfVec3f normalGeomLightExt(0.0f, posHitLight[1], posHitLight[2]);
    normalGeomLightExt.Normalize();
    float phi = std::atan2(posHitLight[2], posHitLight[1]);
    if (phi < 0.0f) {
        phi += 2.0f * ty::Pi;
    }

    *outSample = ty::MakeAreaShapeSample(
        light.xformLightToWorld, light.normalXformLightToWorld,
        posHitLight, normalGeomLightExt,
        GfVec2f(
            phi / (2.0f * ty::Pi),
            (cylinder.length != 0.0f)
                ? ((posHitLight[0] + halfLength) / cylinder.length)
                : 0.0f),
        _AreaCylinder(
            light.xformLightToWorld, cylinder.radius, cylinder.length));
    return true;
}

ty::LightSampler::LightSample
ty::SampleCylinderLight(
    ty::LightData const& light, ty::CylinderLight const& cylinder,
    GfVec3f const& posWld, float u1, float u2,
    ty::RenderColorSpace renderColorSpace)
{
    ty::ShapeSample shapeSample = _SampleCylinder(
        light.xformLightToWorld, light.normalXformLightToWorld,
        cylinder.radius, cylinder.length, u1, u2);
    return ty::EvalAreaLight(light, shapeSample, posWld, renderColorSpace);
}

ty::LightSampler::LightSample
ty::EvaluateCylinderLightDirection(
    ty::LightData const& light, ty::CylinderLight const& cylinder,
    GfVec3f const& posWld, GfVec3f const& dirWld,
    ty::RenderColorSpace renderColorSpace)
{
    ty::ShapeSample shapeSample;
    if (!_IntersectCylinderLight(
            light, cylinder, posWld, dirWld.GetNormalized(),
            &shapeSample)) {
        return ty::InvalidLightSample();
    }
    return ty::EvalAreaLight(light, shapeSample, posWld, renderColorSpace);
}

PXR_NAMESPACE_CLOSE_SCOPE
