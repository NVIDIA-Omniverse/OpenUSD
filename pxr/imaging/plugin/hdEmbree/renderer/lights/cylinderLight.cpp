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
_AreaCylinder(GfMatrix4f const& xf, float radius, float length)
{
    const float c =
        xf.TransformDir(GfVec3f{length, 0.0f, 0.0f}).GetLength();
    const float a =
        xf.TransformDir(GfVec3f{0.0f, radius, 0.0f}).GetLength();
    const float b =
        xf.TransformDir(GfVec3f{0.0f, 0.0f, radius}).GetLength();
    // Ramanujan's ellipse-perimeter approximation preserves the established
    // transformed-cylinder area estimate.
    const float e =
        ty::Pi<float> *
        (3.0f * (a + b) - sqrtf((3.0f * a + b) * (a + 3.0f * b)));
    return e * c;
}

static ty::ShapeSample
_SampleCylinder(GfMatrix4f const& xf, GfMatrix3f const& normalXform,
                float radius, float length, float u1, float u2)
{
    const float z = GfLerp(u1, -length / 2.0f, length / 2.0f);
    const float phi = u2 * 2.0f * ty::Pi<float>;
    GfVec3f pLight(z, radius * cosf(phi), radius * sinf(phi));

    // Keep the sampled point exactly on the lateral surface despite
    // trigonometric roundoff.
    const float hitRad =
        sqrtf(ty::Sqr(pLight[1]) + ty::Sqr(pLight[2]));
    pLight[1] *= radius / hitRad;
    pLight[2] *= radius / hitRad;

    GfVec3f nLight(0.0f, pLight[1], pLight[2]);
    nLight.Normalize();

    return ty::ShapeSample{
        xf.Transform(pLight), (nLight * normalXform).GetNormalized(),
        GfVec2f(u2, u1), _AreaCylinder(xf, radius, length)};
}

static bool
_IntersectCylinderLight(
    HdEmbree_LightData const& light, HdEmbree_Cylinder const& cylinder,
    GfVec3f const& position, GfVec3f const& direction,
    ty::ShapeSample* outSample)
{
    if (!outSample) {
        return false;
    }

    const GfVec3f pLight = light.xformWorldToLight.Transform(position);
    const GfVec3f dLight = light.xformWorldToLight.TransformDir(direction);

    const float a =
        dLight[1] * dLight[1] + dLight[2] * dLight[2];
    const float b =
        2.0f * (pLight[1] * dLight[1] + pLight[2] * dLight[2]);
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

    // Accept only roots on the open lateral surface, excluding both end caps.
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
        phi += 2.0f * ty::Pi<float>;
    }

    *outSample = ty::MakeAreaShapeSample(
        light.xformLightToWorld, light.normalXformLightToWorld, hitLight, nLight,
        GfVec2f(
            phi / (2.0f * ty::Pi<float>),
            (cylinder.length != 0.0f)
                ? ((hitLight[0] + halfLength) / cylinder.length)
                : 0.0f),
        _AreaCylinder(
            light.xformLightToWorld, cylinder.radius, cylinder.length));
    return true;
}

HdEmbreeLightSampler::LightSample
ty::SampleCylinderLight(
    HdEmbree_LightData const& light, HdEmbree_Cylinder const& cylinder,
    GfVec3f const& position, float u1, float u2,
    HdEmbreeRenderColorSpace renderColorSpace)
{
    ty::ShapeSample shapeSample = _SampleCylinder(
        light.xformLightToWorld, light.normalXformLightToWorld,
        cylinder.radius, cylinder.length, u1, u2);
    return ty::EvalAreaLight(light, shapeSample, position, renderColorSpace);
}

HdEmbreeLightSampler::LightSample
ty::EvaluateCylinderLightDirection(
    HdEmbree_LightData const& light, HdEmbree_Cylinder const& cylinder,
    GfVec3f const& position, GfVec3f const& direction,
    HdEmbreeRenderColorSpace renderColorSpace)
{
    ty::ShapeSample shapeSample;
    if (!_IntersectCylinderLight(
            light, cylinder, position, direction.GetNormalized(),
            &shapeSample)) {
        return ty::InvalidLightSample();
    }
    return ty::EvalAreaLight(light, shapeSample, position, renderColorSpace);
}

PXR_NAMESPACE_CLOSE_SCOPE
