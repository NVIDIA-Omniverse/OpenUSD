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
_AreaDisk(GfMatrix4f const& xf, float radius)
{
    const float a =
        xf.TransformDir(GfVec3f{radius, 0.0f, 0.0f}).GetLength();
    const float b =
        xf.TransformDir(GfVec3f{0.0f, radius, 0.0f}).GetLength();
    return ty::Pi<float> * a * b;
}

static GfVec3f
_SampleDiskPolar(float u1, float u2)
{
    const float radius = sqrtf(u1);
    const float theta = 2.0f * ty::Pi<float> * u2;
    return GfVec3f(
        radius * cosf(theta), radius * sinf(theta), 0.0f);
}

static ty::ShapeSample
_SampleDisk(GfMatrix4f const& xf, GfMatrix3f const& normalXform, float radius,
            float u1, float u2)
{
    GfVec3f pLight = _SampleDiskPolar(u1, u2);
    const GfVec3f nLight(0.0f, 0.0f, -1.0f);
    const GfVec2f uv(pLight[0], pLight[1]);
    pLight *= radius;
    return ty::ShapeSample{
        xf.Transform(pLight), (nLight * normalXform).GetNormalized(), uv,
        _AreaDisk(xf, radius)};
}

static bool
_IntersectDiskLight(
    HdEmbree_LightData const& light, HdEmbree_Disk const& disk,
    GfVec3f const& position, GfVec3f const& direction,
    ty::ShapeSample* outSample)
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
    if (hitLight[0] * hitLight[0] +
            hitLight[1] * hitLight[1] >
        disk.radius * disk.radius) {
        return false;
    }

    *outSample = ty::MakeAreaShapeSample(
        light.xformLightToWorld, light.normalXformLightToWorld, hitLight,
        GfVec3f(0.0f, 0.0f, -1.0f),
        GfVec2f(
            (disk.radius != 0.0f)
                ? (hitLight[0] / disk.radius)
                : 0.0f,
            (disk.radius != 0.0f)
                ? (hitLight[1] / disk.radius)
                : 0.0f),
        _AreaDisk(light.xformLightToWorld, disk.radius));
    return true;
}

static HdEmbreeLightSampler::LightSample
_SampleDiskDirectionalShaping(
    HdEmbree_LightData const& light, HdEmbree_Disk const& disk,
    GfVec3f const& position, float u1, float u2,
    HdEmbreeRenderColorSpace renderColorSpace)
{
    const HdEmbree_DirectionalShapingSample directionalSample =
        HdEmbreeSampleDirectionalShaping(light.shaping, u1, u2);
    if (!directionalSample.valid) {
        return ty::InvalidLightSample();
    }

    GfVec3f localDirection = directionalSample.localDirection;
    if (localDirection[2] < 0.0f) {
        localDirection[2] = -localDirection[2];
    }

    const GfVec3f worldDirection =
        light.xformLightToWorld.TransformDir(
            localDirection).GetNormalized();
    ty::ShapeSample shapeSample;
    if (!_IntersectDiskLight(
            light, disk, position, worldDirection, &shapeSample)) {
        return ty::InvalidLightSample();
    }

    HdEmbreeLightSampler::LightSample sample =
        ty::EvalAreaLight(light, shapeSample, position, renderColorSpace);
    ty::ApplyShapingAwareFinitePdf(light, &sample, true);
    return sample;
}

HdEmbreeLightSampler::LightSample
ty::SampleDiskLight(
    HdEmbree_LightData const& light, HdEmbree_Disk const& disk,
    GfVec3f const& position, float u1, float u2,
    HdEmbreeRenderColorSpace renderColorSpace)
{
    const bool useShapingAwareSampling =
        light.shaping.directionalDistribution.IsValid();
    if (useShapingAwareSampling &&
        u1 >= ty::ShapingAwareFiniteAreaProposalWeight) {
        return _SampleDiskDirectionalShaping(
            light, disk, position,
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
    HdEmbreeLightSampler::LightSample sample =
        ty::EvalAreaLight(light, shapeSample, position, renderColorSpace);
    if (useShapingAwareSampling) {
        ty::ApplyShapingAwareFinitePdf(light, &sample, true);
    }
    return sample;
}

HdEmbreeLightSampler::LightSample
ty::EvaluateDiskLightDirection(
    HdEmbree_LightData const& light, HdEmbree_Disk const& disk,
    GfVec3f const& position, GfVec3f const& direction,
    HdEmbreeRenderColorSpace renderColorSpace)
{
    ty::ShapeSample shapeSample;
    if (!_IntersectDiskLight(
            light, disk, position, direction.GetNormalized(), &shapeSample)) {
        return ty::InvalidLightSample();
    }

    HdEmbreeLightSampler::LightSample sample =
        ty::EvalAreaLight(light, shapeSample, position, renderColorSpace);
    ty::ApplyShapingAwareFinitePdf(light, &sample, true);
    return sample;
}

PXR_NAMESPACE_CLOSE_SCOPE
