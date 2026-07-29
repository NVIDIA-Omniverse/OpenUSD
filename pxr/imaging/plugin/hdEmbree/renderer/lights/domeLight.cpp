//
// Copyright 2025 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
// Samples and evaluates textured or uniform dome lights in directional
// measure.
//
#include "lightSamplerCommon.h"
#include "lightSamplerDispatch.h"

#include "pxr/base/gf/vec2f.h"
#include "pxr/base/gf/vec3f.h"

#include <algorithm>
#include <cmath>
#include <limits>

PXR_NAMESPACE_OPEN_SCOPE

static bool
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

static GfVec2f
_DirectionToLatLongUv(GfVec3f const& localDirection)
{
    const GfVec3f normalized = localDirection.GetNormalized();
    const float t = acosf(GfClamp(normalized[1], -1.0f, 1.0f)) /
        ty::Pi<float>;
    const float s = ty::WrapUnit(
        0.5f - atan2f(normalized[0], normalized[2]) /
            (2.0f * ty::Pi<float>));
    return GfVec2f(s, ty::ClampUnitHalfOpen(t));
}

static GfVec3f
_LatLongUvToDirection(GfVec2f const& uv)
{
    const float theta = ty::Pi<float> * ty::ClampUnitHalfOpen(uv[1]);
    const float sinTheta = sinf(theta);
    const float cosTheta = cosf(theta);
    const float phi = 2.0f * ty::Pi<float> * (0.5f - uv[0]);
    return GfVec3f(
        sinTheta * sinf(phi), cosTheta, sinTheta * cosf(phi));
}

static float
_TexelDirectionalPdf(
    HdEmbree_LightTexture const& texture, int x, int y, float theta)
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
    return pdfUv /
        (2.0f * ty::Pi<float> * ty::Pi<float> * sinTheta);
}

static GfVec2f
_SampleDomeUv(HdEmbree_LightTexture const& texture, float u1, float u2)
{
    const float sampleX = ty::ClampUnitHalfOpen(u1);
    const float sampleY = ty::ClampUnitHalfOpen(u2);

    const auto marginalBegin = texture.marginalCdf.begin();
    const auto marginalIt = std::upper_bound(
        marginalBegin + 1, texture.marginalCdf.end(), sampleY);
    const int y = std::clamp(
        static_cast<int>(marginalIt - (marginalBegin + 1)),
        0, texture.height - 1);

    const float cdfY0 = texture.marginalCdf[y];
    const float cdfY1 = texture.marginalCdf[y + 1];
    const float remappedY = (cdfY1 > cdfY0)
        ? ((sampleY - cdfY0) / (cdfY1 - cdfY0))
        : 0.0f;

    const float* const rowBegin = texture.conditionalCdf.data() +
        static_cast<size_t>(y) *
            static_cast<size_t>(texture.width + 1);
    const float* const rowIt = std::upper_bound(
        rowBegin + 1, rowBegin + texture.width + 1, sampleX);
    const int x = std::clamp(
        static_cast<int>(rowIt - (rowBegin + 1)),
        0, texture.width - 1);

    const float cdfX0 = rowBegin[x];
    const float cdfX1 = rowBegin[x + 1];
    const float remappedX = (cdfX1 > cdfX0)
        ? ((sampleX - cdfX0) / (cdfX1 - cdfX0))
        : 0.0f;

    return GfVec2f(
        (static_cast<float>(x) + remappedX) / static_cast<float>(texture.width),
        (static_cast<float>(y) + remappedY) /
        static_cast<float>(texture.height));
}

static float
_DomeDirectionalPdf(
    HdEmbree_LightData const& light, GfVec2f const& uv)
{
    float pdfSolidAngle = 1.0f / (4.0f * ty::Pi<float>);
    if (_HasDomeDistribution(light.texture)) {
        const int x = std::clamp(
            static_cast<int>(
                static_cast<float>(light.texture.width) *
                ty::WrapUnit(uv[0])),
            0,
            light.texture.width - 1);
        const int y = std::clamp(
            static_cast<int>(
                static_cast<float>(light.texture.height) *
                ty::ClampUnitHalfOpen(uv[1])),
            0,
            light.texture.height - 1);
        const float theta = ty::Pi<float> * ty::ClampUnitHalfOpen(uv[1]);
        pdfSolidAngle = _TexelDirectionalPdf(light.texture, x, y, theta);
    }
    return pdfSolidAngle;
}

static float
_DomeDirectionalPdf(
    HdEmbree_LightData const& light, GfVec3f const& worldDirection)
{
    if (!ty::IsFinite(worldDirection) || worldDirection.GetLengthSq() <= 0.0f) {
        return 0.0f;
    }

    const GfVec3f localDirection =
        light.xformWorldToLight.TransformDir(
            worldDirection.GetNormalized()).GetNormalized();
    return _DomeDirectionalPdf(light, _DirectionToLatLongUv(localDirection));
}

static bool
_GetReflectionHemisphereNormal(
    GfVec3f const& normal, GfVec3f* normalizedNormal)
{
    if (!normalizedNormal || !ty::IsFinite(normal) ||
        normal.GetLengthSq() <= 0.0f) {
        return false;
    }
    *normalizedNormal = normal.GetNormalized();
    return true;
}

static GfVec3f
_ReflectAcrossPlane(
    GfVec3f const& direction, GfVec3f const& normal)
{
    return (direction - normal * (2.0f * GfDot(direction, normal)))
        .GetNormalized();
}

static float
_ReflectionHemispherePdf(
    HdEmbree_LightData const& light, GfVec3f const& normal,
    GfVec3f const& direction)
{
    GfVec3f n;
    if (!_GetReflectionHemisphereNormal(normal, &n) ||
        !ty::IsFinite(direction) ||
        direction.GetLengthSq() <= 0.0f) {
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

static GfVec3f
_FoldDirectionToReflectionHemisphere(
    GfVec3f const& direction, GfVec3f const& normal)
{
    GfVec3f n;
    if (!_GetReflectionHemisphereNormal(normal, &n) ||
        !ty::IsFinite(direction) ||
        direction.GetLengthSq() <= 0.0f) {
        return GfVec3f(0.0f);
    }

    const GfVec3f omegaInWld = direction.GetNormalized();
    return GfDot(n, omegaInWld) > 0.0f
        ? omegaInWld
        : _ReflectAcrossPlane(omegaInWld, n);
}

HdEmbreeLightSampler::LightSample
ty::EvaluateDomeLightDirection(
    HdEmbree_LightData const& light, GfVec3f const& direction,
    HdEmbreeRenderColorSpace renderColorSpace)
{
    if (!ty::IsFinite(direction) || direction.GetLengthSq() <= 0.0f) {
        return ty::InvalidLightSample();
    }

    const GfVec3f normalizedDirection = direction.GetNormalized();
    const GfVec3f localDirection =
        light.xformWorldToLight.TransformDir(
            normalizedDirection).GetNormalized();
    const GfVec2f uv = _DirectionToLatLongUv(localDirection);

    GfVec3f radianceIn = light.texture.pixels.empty()
        ? GfVec3f(1.0f)
        : ty::SampleLightTexture(
              light.texture, uv[0], uv[1], renderColorSpace);
    // Apply LightAPI radiometric parameters consistently with area lights.
    radianceIn = GfCompMult(
        radianceIn, ty::EvalLightBasic(light, renderColorSpace));

    const float pdfSolidAngle = _DomeDirectionalPdf(light, uv);
    return HdEmbreeLightSampler::LightSample{
        radianceIn, normalizedDirection, std::numeric_limits<float>::max(),
        (pdfSolidAngle > 0.0f) ? (1.0f / pdfSolidAngle) : 0.0f,
        pdfSolidAngle > 0.0f};
}

HdEmbreeLightSampler::LightSample
ty::EvaluateDomeLightDirection(
    HdEmbree_LightData const& light, GfVec3f const& direction,
    GfVec3f const& normal,
    HdEmbreeLightSampler::SamplingMode samplingMode,
    HdEmbreeRenderColorSpace renderColorSpace)
{
    if (samplingMode !=
        HdEmbreeLightSampler::SamplingMode::ReflectionHemisphere) {
        return ty::EvaluateDomeLightDirection(
            light, direction, renderColorSpace);
    }

    if (!ty::IsFinite(direction) || direction.GetLengthSq() <= 0.0f) {
        return ty::InvalidLightSample();
    }

    const GfVec3f normalizedDirection = direction.GetNormalized();
    const GfVec3f localDirection =
        light.xformWorldToLight.TransformDir(
            normalizedDirection).GetNormalized();
    const GfVec2f uv = _DirectionToLatLongUv(localDirection);

    GfVec3f radianceIn = light.texture.pixels.empty()
        ? GfVec3f(1.0f)
        : ty::SampleLightTexture(
              light.texture, uv[0], uv[1], renderColorSpace);
    radianceIn = GfCompMult(
        radianceIn, ty::EvalLightBasic(light, renderColorSpace));

    const float pdfSolidAngle =
        _ReflectionHemispherePdf(light, normal, normalizedDirection);
    return HdEmbreeLightSampler::LightSample{
        radianceIn, normalizedDirection, std::numeric_limits<float>::max(),
        (pdfSolidAngle > 0.0f) ? (1.0f / pdfSolidAngle) : 0.0f,
        pdfSolidAngle > 0.0f};
}

HdEmbreeLightSampler::LightSample
ty::SampleDomeLight(
    HdEmbree_LightData const& light, GfVec3f const& normal,
    float u1, float u2,
    HdEmbreeLightSampler::SamplingMode samplingMode,
    HdEmbreeRenderColorSpace renderColorSpace)
{
    GfVec3f worldDirection;
    if (!_HasDomeDistribution(light.texture)) {
        const float localY = 1.0f - 2.0f * ty::ClampUnitHalfOpen(u1);
        const float localR =
            sqrtf(std::max(0.0f, 1.0f - ty::Sqr(localY)));
        const float phi = 2.0f * ty::Pi<float> * ty::ClampUnitHalfOpen(u2);
        const GfVec3f localDirection(
            localR * sinf(phi), localY, localR * cosf(phi));
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
            return ty::EvaluateDomeLightDirection(
                light, hemisphereDirection, normal, samplingMode,
                renderColorSpace);
        }
    }

    return ty::EvaluateDomeLightDirection(
        light, worldDirection, renderColorSpace);
}

PXR_NAMESPACE_CLOSE_SCOPE
