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
_AreaRect(GfMatrix4f const& xf, float width, float height)
{
    const GfVec3f U = xf.TransformDir(GfVec3f{width, 0.0f, 0.0f});
    const GfVec3f V = xf.TransformDir(GfVec3f{0.0f, height, 0.0f});
    return GfCross(U, V).GetLength();
}

static ty::ShapeSample
_SampleRect(GfMatrix4f const& xf, GfMatrix3f const& normalXform, float width,
            float height, float u1, float u2)
{
    const GfVec3f pLight(
        (u1 - 0.5f) * width, (u2 - 0.5f) * height, 0.0f);
    const GfVec3f nLight(0.0f, 0.0f, -1.0f);
    return ty::ShapeSample{
        xf.Transform(pLight), (nLight * normalXform).GetNormalized(),
        GfVec2f(u1, u2), _AreaRect(xf, width, height)};
}

static bool
_IntersectRectLight(
    ty::LightData const& light, ty::RectLight const& rect,
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
    const float halfWidth = rect.width * 0.5f;
    const float halfHeight = rect.height * 0.5f;
    if (std::abs(hitLight[0]) > halfWidth ||
        std::abs(hitLight[1]) > halfHeight) {
        return false;
    }

    *outSample = ty::MakeAreaShapeSample(
        light.xformLightToWorld, light.normalXformLightToWorld, hitLight,
        GfVec3f(0.0f, 0.0f, -1.0f),
        GfVec2f(
            (rect.width != 0.0f) ? (hitLight[0] / rect.width + 0.5f) : 0.5f,
            (rect.height != 0.0f) ? (hitLight[1] / rect.height + 0.5f) : 0.5f),
        _AreaRect(light.xformLightToWorld, rect.width, rect.height));
    return true;
}

static ty::LightSampler::LightSample
_SampleRectDirectionalShaping(
    ty::LightData const& light, ty::RectLight const& rect,
    GfVec3f const& position, float u1, float u2,
    ty::RenderColorSpace renderColorSpace)
{
    const ty::DirectionalShapingSample directionalSample =
        ty::SampleDirectionalShaping(light.shaping, u1, u2);
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
    if (!_IntersectRectLight(
            light, rect, position, worldDirection, &shapeSample)) {
        return ty::InvalidLightSample();
    }

    ty::LightSampler::LightSample sample =
        ty::EvalAreaLight(light, shapeSample, position, renderColorSpace);
    ty::ApplyShapingAwareFinitePdf(light, &sample, true);
    return sample;
}

ty::LightSampler::LightSample
ty::SampleRectLight(
    ty::LightData const& light, ty::RectLight const& rect,
    GfVec3f const& position, float u1, float u2,
    ty::RenderColorSpace renderColorSpace)
{
    const bool useShapingAwareSampling =
        light.shaping.directionalDistribution.IsValid();
    if (useShapingAwareSampling &&
        u1 >= ty::ShapingAwareFiniteAreaProposalWeight) {
        return _SampleRectDirectionalShaping(
            light, rect, position,
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
        ty::EvalAreaLight(light, shapeSample, position, renderColorSpace);
    if (useShapingAwareSampling) {
        ty::ApplyShapingAwareFinitePdf(light, &sample, true);
    }
    return sample;
}

ty::LightSampler::LightSample
ty::EvaluateRectLightDirection(
    ty::LightData const& light, ty::RectLight const& rect,
    GfVec3f const& position, GfVec3f const& direction,
    ty::RenderColorSpace renderColorSpace)
{
    ty::ShapeSample shapeSample;
    if (!_IntersectRectLight(
            light, rect, position, direction.GetNormalized(), &shapeSample)) {
        return ty::InvalidLightSample();
    }

    ty::LightSampler::LightSample sample =
        ty::EvalAreaLight(light, shapeSample, position, renderColorSpace);
    ty::ApplyShapingAwareFinitePdf(light, &sample, true);
    return sample;
}

PXR_NAMESPACE_CLOSE_SCOPE
