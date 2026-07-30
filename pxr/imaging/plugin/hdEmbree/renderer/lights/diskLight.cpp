//
// Copyright 2025 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
// Samples and evaluates disk-light geometry and its shaping proposal split.
//
#include "lightSamplerCommon.h"
#include "lightSamplerDispatch.h"

#include "pxr/base/gf/matrix3f.h"
#include "pxr/base/gf/matrix4f.h"
#include "pxr/base/gf/vec2f.h"
#include "pxr/base/gf/vec3f.h"

#include <cmath>

PXR_NAMESPACE_OPEN_SCOPE

static float
_AreaDisk(GfMatrix4f const& lightToWorld, float radius)
{
    const float radiusAxisXWld =
        lightToWorld.TransformDir(GfVec3f{radius, 0.0f, 0.0f}).GetLength();
    const float radiusAxisYWld =
        lightToWorld.TransformDir(GfVec3f{0.0f, radius, 0.0f}).GetLength();
    return ty::Pi * radiusAxisXWld * radiusAxisYWld;
}

static GfVec3f
_SampleDiskPolar(float u1, float u2)
{
    const float radius = sqrtf(u1);
    const float theta = 2.0f * ty::Pi * u2;
    return GfVec3f(
        radius * cosf(theta), radius * sinf(theta), 0.0f);
}

static ty::ShapeSample
_SampleDisk(
    GfMatrix4f const& lightToWorld,
    GfMatrix3f const& normalLightToWorld,
    float radius, float u1, float u2)
{
    GfVec3f posLight = _SampleDiskPolar(u1, u2);
    const GfVec3f normalGeomLightExt(0.0f, 0.0f, -1.0f);
    const GfVec2f coordinateTexture(posLight[0], posLight[1]);
    posLight *= radius;
    return ty::ShapeSample{
        lightToWorld.Transform(posLight),
        (normalGeomLightExt * normalLightToWorld).GetNormalized(),
        coordinateTexture,
        _AreaDisk(lightToWorld, radius)};
}

static bool
_IntersectDiskLight(
    ty::LightData const& light, ty::DiskLight const& disk,
    GfVec3f const& posWld, GfVec3f const& dirWld,
    ty::ShapeSample* outSample)
{
    if (!outSample) {
        return false;
    }

    const GfVec3f posLight = light.xformWorldToLight.Transform(posWld);
    const GfVec3f dirLight = light.xformWorldToLight.TransformDir(dirWld);
    if (std::abs(dirLight[2]) <= 1.0e-6f) {
        return false;
    }

    const float t = -posLight[2] / dirLight[2];
    if (t <= 1.0e-6f || !std::isfinite(t)) {
        return false;
    }

    const GfVec3f posHitLight = posLight + dirLight * t;
    if (posHitLight[0] * posHitLight[0] +
            posHitLight[1] * posHitLight[1] >
        disk.radius * disk.radius) {
        return false;
    }

    *outSample = ty::MakeAreaShapeSample(
        light.xformLightToWorld, light.normalXformLightToWorld, posHitLight,
        GfVec3f(0.0f, 0.0f, -1.0f),
        GfVec2f(
            (disk.radius != 0.0f)
                ? (posHitLight[0] / disk.radius)
                : 0.0f,
            (disk.radius != 0.0f)
                ? (posHitLight[1] / disk.radius)
                : 0.0f),
        _AreaDisk(light.xformLightToWorld, disk.radius));
    return true;
}

static ty::LightSampler::LightSample
_SampleDiskDirectionalShaping(
    ty::LightData const& light, ty::DiskLight const& disk,
    GfVec3f const& posWld, float u1, float u2,
    ty::RenderColorSpace renderColorSpace)
{
    const ty::DirectionalShapingSample directionalSample =
        ty::SampleDirectionalShaping(light.shaping, u1, u2);
    if (!directionalSample.valid) {
        return ty::InvalidLightSample();
    }

    GfVec3f dirLight = directionalSample.dirLight;
    if (dirLight[2] < 0.0f) {
        dirLight[2] = -dirLight[2];
    }

    const GfVec3f dirWld =
        light.xformLightToWorld.TransformDir(
            dirLight).GetNormalized();
    ty::ShapeSample shapeSample;
    if (!_IntersectDiskLight(
            light, disk, posWld, dirWld, &shapeSample)) {
        return ty::InvalidLightSample();
    }

    ty::LightSampler::LightSample sample =
        ty::EvalAreaLight(light, shapeSample, posWld, renderColorSpace);
    ty::ApplyShapingAwareFinitePdf(light, &sample, true);
    return sample;
}

ty::LightSampler::LightSample
ty::SampleDiskLight(
    ty::LightData const& light, ty::DiskLight const& disk,
    GfVec3f const& posWld, float u1, float u2,
    ty::RenderColorSpace renderColorSpace)
{
    const bool useShapingAwareSampling =
        light.shaping.directionalDistribution.IsValid();
    if (useShapingAwareSampling &&
        u1 >= ty::ShapingAwareFiniteAreaProposalWeight) {
        return _SampleDiskDirectionalShaping(
            light, disk, posWld,
            (u1 - ty::ShapingAwareFiniteAreaProposalWeight) /
                ty::ShapingAwareFiniteDirectionalProposalWeight,
            u2, renderColorSpace);
    }

    ty::ShapeSample shapeSample = _SampleDisk(
        light.xformLightToWorld, light.normalXformLightToWorld, disk.radius,
        useShapingAwareSampling
            ? (u1 / ty::ShapingAwareFiniteAreaProposalWeight)
            : u1,
        u2);
    ty::LightSampler::LightSample sample =
        ty::EvalAreaLight(light, shapeSample, posWld, renderColorSpace);
    if (useShapingAwareSampling) {
        ty::ApplyShapingAwareFinitePdf(light, &sample, true);
    }
    return sample;
}

ty::LightSampler::LightSample
ty::EvaluateDiskLightDirection(
    ty::LightData const& light, ty::DiskLight const& disk,
    GfVec3f const& posWld, GfVec3f const& dirWld,
    ty::RenderColorSpace renderColorSpace)
{
    ty::ShapeSample shapeSample;
    if (!_IntersectDiskLight(
            light, disk, posWld, dirWld.GetNormalized(), &shapeSample)) {
        return ty::InvalidLightSample();
    }

    ty::LightSampler::LightSample sample =
        ty::EvalAreaLight(light, shapeSample, posWld, renderColorSpace);
    ty::ApplyShapingAwareFinitePdf(light, &sample, true);
    return sample;
}

PXR_NAMESPACE_CLOSE_SCOPE
