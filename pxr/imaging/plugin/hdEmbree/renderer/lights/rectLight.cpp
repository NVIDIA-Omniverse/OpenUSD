//
// Copyright 2025 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
// Samples and evaluates rect-light geometry and its shaping proposal split.
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
_AreaRect(GfMatrix4f const& lightToWorld, float width, float height)
{
    const GfVec3f edgeWidthWld =
        lightToWorld.TransformDir(GfVec3f{width, 0.0f, 0.0f});
    const GfVec3f edgeHeightWld =
        lightToWorld.TransformDir(GfVec3f{0.0f, height, 0.0f});
    return GfCross(edgeWidthWld, edgeHeightWld).GetLength();
}

static ty::ShapeSample
_SampleRect(
    GfMatrix4f const& lightToWorld,
    GfMatrix3f const& normalLightToWorld,
    float width, float height, float u1, float u2)
{
    const GfVec3f posLight(
        (u1 - 0.5f) * width, (u2 - 0.5f) * height, 0.0f);
    const GfVec3f normalGeomLightExt(0.0f, 0.0f, -1.0f);
    return ty::ShapeSample{
        lightToWorld.Transform(posLight),
        (normalGeomLightExt * normalLightToWorld).GetNormalized(),
        GfVec2f(u1, u2), _AreaRect(lightToWorld, width, height)};
}

static bool
_IntersectRectLight(
    ty::LightData const& light, ty::RectLight const& rect,
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
    const float halfWidth = rect.width * 0.5f;
    const float halfHeight = rect.height * 0.5f;
    if (std::abs(posHitLight[0]) > halfWidth ||
        std::abs(posHitLight[1]) > halfHeight) {
        return false;
    }

    const float u = (rect.width != 0.0f)
        ? (posHitLight[0] / rect.width + 0.5f)
        : 0.5f;
    const float v = (rect.height != 0.0f)
        ? (posHitLight[1] / rect.height + 0.5f)
        : 0.5f;
    *outSample = ty::MakeAreaShapeSample(
        light.xformLightToWorld, light.normalXformLightToWorld, posHitLight,
        GfVec3f(0.0f, 0.0f, -1.0f),
        GfVec2f(u, v),
        _AreaRect(light.xformLightToWorld, rect.width, rect.height));
    return true;
}

static ty::LightSampler::LightSample
_SampleRectDirectionalShaping(
    ty::LightData const& light, ty::RectLight const& rect,
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
    if (!_IntersectRectLight(
            light, rect, posWld, dirWld, &shapeSample)) {
        return ty::InvalidLightSample();
    }

    ty::LightSampler::LightSample sample =
        ty::EvalAreaLight(light, shapeSample, posWld, renderColorSpace);
    ty::ApplyShapingAwareFinitePdf(light, &sample, true);
    return sample;
}

ty::LightSampler::LightSample
ty::SampleRectLight(
    ty::LightData const& light, ty::RectLight const& rect,
    GfVec3f const& posWld, float u1, float u2,
    ty::RenderColorSpace renderColorSpace)
{
    const bool useShapingAwareSampling =
        light.shaping.directionalDistribution.IsValid();
    if (useShapingAwareSampling &&
        u1 >= ty::ShapingAwareFiniteAreaProposalWeight) {
        return _SampleRectDirectionalShaping(
            light, rect, posWld,
            (u1 - ty::ShapingAwareFiniteAreaProposalWeight) /
                ty::ShapingAwareFiniteDirectionalProposalWeight,
            u2, renderColorSpace);
    }

    ty::ShapeSample shapeSample = _SampleRect(
        light.xformLightToWorld, light.normalXformLightToWorld,
        rect.width, rect.height,
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
ty::EvaluateRectLightDirection(
    ty::LightData const& light, ty::RectLight const& rect,
    GfVec3f const& posWld, GfVec3f const& dirWld,
    ty::RenderColorSpace renderColorSpace)
{
    ty::ShapeSample shapeSample;
    if (!_IntersectRectLight(
            light, rect, posWld, dirWld.GetNormalized(), &shapeSample)) {
        return ty::InvalidLightSample();
    }

    ty::LightSampler::LightSample sample =
        ty::EvalAreaLight(light, shapeSample, posWld, renderColorSpace);
    ty::ApplyShapingAwareFinitePdf(light, &sample, true);
    return sample;
}

PXR_NAMESPACE_CLOSE_SCOPE
