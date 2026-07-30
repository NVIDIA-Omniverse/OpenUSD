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
_HasDomeDistribution(ty::LightTexture const& texture)
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
_DirectionToLatLongUv(GfVec3f const& dirLight)
{
    const GfVec3f normalized = dirLight.GetNormalized();
    const float coordinateTextureT =
        acosf(GfClamp(normalized[1], -1.0f, 1.0f)) /
        ty::Pi<float>;
    const float coordinateTextureS = ty::WrapUnit(
        0.5f - atan2f(normalized[0], normalized[2]) /
            (2.0f * ty::Pi<float>));
    return GfVec2f(
        coordinateTextureS,
        ty::ClampUnitHalfOpen(coordinateTextureT));
}

static GfVec3f
_LatLongUvToDirection(GfVec2f const& coordinateTexture)
{
    const float theta =
        ty::Pi<float> * ty::ClampUnitHalfOpen(coordinateTexture[1]);
    const float sinTheta = sinf(theta);
    const float cosTheta = cosf(theta);
    const float phi = 2.0f * ty::Pi<float> * (0.5f - coordinateTexture[0]);
    return GfVec3f(
        sinTheta * sinf(phi), cosTheta, sinTheta * cosf(phi));
}

static float
_TexelDirectionalPdf(
    ty::LightTexture const& texture,
    int indexTexelX,
    int indexTexelY,
    float theta)
{
    if (!_HasDomeDistribution(texture) ||
        indexTexelX < 0 || indexTexelY < 0 ||
        indexTexelX >= texture.width ||
        indexTexelY >= texture.height) {
        return 0.0f;
    }

    const float sinTheta = sinf(theta);
    if (sinTheta <= 0.0f) {
        return 0.0f;
    }

    const size_t indexTexel =
        static_cast<size_t>(indexTexelY) *
            static_cast<size_t>(texture.width) +
        indexTexelX;
    const float texelMass =
        texture.texelWeights[indexTexel] / texture.weightSum;
    const float pdfUv =
        texelMass * static_cast<float>(texture.width * texture.height);
    return pdfUv /
        (2.0f * ty::Pi<float> * ty::Pi<float> * sinTheta);
}

static GfVec2f
_SampleDomeUv(ty::LightTexture const& texture, float u1, float u2)
{
    const float sampleX = ty::ClampUnitHalfOpen(u1);
    const float sampleY = ty::ClampUnitHalfOpen(u2);

    std::vector<float>::const_iterator marginalBegin =
        texture.marginalCdf.begin();
    const auto marginalIt = std::upper_bound(
        marginalBegin + 1, texture.marginalCdf.end(), sampleY);
    const int indexTexelY = std::clamp(
        static_cast<int>(marginalIt - (marginalBegin + 1)),
        0, texture.height - 1);

    const float cdfY0 = texture.marginalCdf[indexTexelY];
    const float cdfY1 = texture.marginalCdf[indexTexelY + 1];
    const float remappedY = (cdfY1 > cdfY0)
        ? ((sampleY - cdfY0) / (cdfY1 - cdfY0))
        : 0.0f;

    const float* const rowBegin = texture.conditionalCdf.data() +
        static_cast<size_t>(indexTexelY) *
            static_cast<size_t>(texture.width + 1);
    const float* const rowIt = std::upper_bound(
        rowBegin + 1, rowBegin + texture.width + 1, sampleX);
    const int indexTexelX = std::clamp(
        static_cast<int>(rowIt - (rowBegin + 1)),
        0, texture.width - 1);

    const float cdfX0 = rowBegin[indexTexelX];
    const float cdfX1 = rowBegin[indexTexelX + 1];
    const float remappedX = (cdfX1 > cdfX0)
        ? ((sampleX - cdfX0) / (cdfX1 - cdfX0))
        : 0.0f;

    return GfVec2f(
        (static_cast<float>(indexTexelX) + remappedX) /
            static_cast<float>(texture.width),
        (static_cast<float>(indexTexelY) + remappedY) /
        static_cast<float>(texture.height));
}

static float
_DomeDirectionalPdf(
    ty::LightData const& light, GfVec2f const& coordinateTexture)
{
    float pdfSolidAngle = 1.0f / (4.0f * ty::Pi<float>);
    if (_HasDomeDistribution(light.texture)) {
        const int indexTexelX = std::clamp(
            static_cast<int>(
                static_cast<float>(light.texture.width) *
                ty::WrapUnit(coordinateTexture[0])),
            0,
            light.texture.width - 1);
        const int indexTexelY = std::clamp(
            static_cast<int>(
                static_cast<float>(light.texture.height) *
                ty::ClampUnitHalfOpen(coordinateTexture[1])),
            0,
            light.texture.height - 1);
        const float theta =
            ty::Pi<float> * ty::ClampUnitHalfOpen(coordinateTexture[1]);
        pdfSolidAngle = _TexelDirectionalPdf(
            light.texture, indexTexelX, indexTexelY, theta);
    }
    return pdfSolidAngle;
}

static float
_DomeDirectionalPdf(
    ty::LightData const& light, GfVec3f const& dirWld)
{
    if (!ty::IsFinite(dirWld) || dirWld.GetLengthSq() <= 0.0f) {
        return 0.0f;
    }

    const GfVec3f dirLight =
        light.xformWorldToLight.TransformDir(
            dirWld.GetNormalized()).GetNormalized();
    return _DomeDirectionalPdf(light, _DirectionToLatLongUv(dirLight));
}

static bool
_GetReflectionHemisphereNormal(
    GfVec3f const& normalShdWldOut,
    GfVec3f* outNormalShdWldOutNormalized)
{
    if (!outNormalShdWldOutNormalized ||
        !ty::IsFinite(normalShdWldOut) ||
        normalShdWldOut.GetLengthSq() <= 0.0f) {
        return false;
    }
    *outNormalShdWldOutNormalized = normalShdWldOut.GetNormalized();
    return true;
}

static GfVec3f
_ReflectAcrossPlane(
    GfVec3f const& dirWld, GfVec3f const& normalShdWldOut)
{
    return (dirWld -
            normalShdWldOut * (2.0f * GfDot(dirWld, normalShdWldOut)))
        .GetNormalized();
}

static float
_ReflectionHemispherePdf(
    ty::LightData const& light, GfVec3f const& normalShdWldOut,
    GfVec3f const& dirWld)
{
    GfVec3f normalShdWldOutNormalized;
    if (!_GetReflectionHemisphereNormal(
            normalShdWldOut, &normalShdWldOutNormalized) ||
        !ty::IsFinite(dirWld) ||
        dirWld.GetLengthSq() <= 0.0f) {
        return 0.0f;
    }

    const GfVec3f omegaInWld = dirWld.GetNormalized();
    if (GfDot(normalShdWldOutNormalized, omegaInWld) <= 0.0f) {
        return 0.0f;
    }

    const GfVec3f dirMirroredWld =
        _ReflectAcrossPlane(omegaInWld, normalShdWldOutNormalized);
    return _DomeDirectionalPdf(light, omegaInWld) +
           _DomeDirectionalPdf(light, dirMirroredWld);
}

static GfVec3f
_FoldDirectionToReflectionHemisphere(
    GfVec3f const& dirWld, GfVec3f const& normalShdWldOut)
{
    GfVec3f normalShdWldOutNormalized;
    if (!_GetReflectionHemisphereNormal(
            normalShdWldOut, &normalShdWldOutNormalized) ||
        !ty::IsFinite(dirWld) ||
        dirWld.GetLengthSq() <= 0.0f) {
        return GfVec3f(0.0f);
    }

    const GfVec3f omegaInWld = dirWld.GetNormalized();
    return GfDot(normalShdWldOutNormalized, omegaInWld) > 0.0f
        ? omegaInWld
        : _ReflectAcrossPlane(omegaInWld, normalShdWldOutNormalized);
}

ty::LightSampler::LightSample
ty::EvaluateDomeLightDirection(
    ty::LightData const& light, GfVec3f const& dirWld,
    ty::RenderColorSpace renderColorSpace)
{
    if (!ty::IsFinite(dirWld) || dirWld.GetLengthSq() <= 0.0f) {
        return ty::InvalidLightSample();
    }

    const GfVec3f dirWldNormalized = dirWld.GetNormalized();
    const GfVec3f dirLight =
        light.xformWorldToLight.TransformDir(
            dirWldNormalized).GetNormalized();
    const GfVec2f coordinateTexture = _DirectionToLatLongUv(dirLight);

    GfVec3f radianceIn = light.texture.pixels.empty()
        ? GfVec3f(1.0f)
        : ty::SampleLightTexture(
              light.texture, coordinateTexture[0], coordinateTexture[1],
              renderColorSpace);
    // Apply LightAPI radiometric parameters consistently with area lights.
    radianceIn = GfCompMult(
        radianceIn, ty::EvalLightBasic(light, renderColorSpace));

    const float pdfSolidAngle = _DomeDirectionalPdf(light, coordinateTexture);
    return ty::LightSampler::LightSample{
        radianceIn, dirWldNormalized, std::numeric_limits<float>::max(),
        (pdfSolidAngle > 0.0f) ? (1.0f / pdfSolidAngle) : 0.0f,
        pdfSolidAngle > 0.0f};
}

ty::LightSampler::LightSample
ty::EvaluateDomeLightDirection(
    ty::LightData const& light, GfVec3f const& dirWld,
    GfVec3f const& normalShdWldOut,
    ty::LightSampler::SamplingMode samplingMode,
    ty::RenderColorSpace renderColorSpace)
{
    if (samplingMode !=
        ty::LightSampler::SamplingMode::ReflectionHemisphere) {
        return ty::EvaluateDomeLightDirection(
            light, dirWld, renderColorSpace);
    }

    if (!ty::IsFinite(dirWld) || dirWld.GetLengthSq() <= 0.0f) {
        return ty::InvalidLightSample();
    }

    const GfVec3f dirWldNormalized = dirWld.GetNormalized();
    const GfVec3f dirLight =
        light.xformWorldToLight.TransformDir(
            dirWldNormalized).GetNormalized();
    const GfVec2f coordinateTexture = _DirectionToLatLongUv(dirLight);

    GfVec3f radianceIn = light.texture.pixels.empty()
        ? GfVec3f(1.0f)
        : ty::SampleLightTexture(
              light.texture, coordinateTexture[0], coordinateTexture[1],
              renderColorSpace);
    radianceIn = GfCompMult(
        radianceIn, ty::EvalLightBasic(light, renderColorSpace));

    const float pdfSolidAngle =
        _ReflectionHemispherePdf(
            light, normalShdWldOut, dirWldNormalized);
    return ty::LightSampler::LightSample{
        radianceIn, dirWldNormalized, std::numeric_limits<float>::max(),
        (pdfSolidAngle > 0.0f) ? (1.0f / pdfSolidAngle) : 0.0f,
        pdfSolidAngle > 0.0f};
}

ty::LightSampler::LightSample
ty::SampleDomeLight(
    ty::LightData const& light, GfVec3f const& normalShdWldOut,
    float u1, float u2,
    ty::LightSampler::SamplingMode samplingMode,
    ty::RenderColorSpace renderColorSpace)
{
    GfVec3f dirWld;
    if (!_HasDomeDistribution(light.texture)) {
        const float localY = 1.0f - 2.0f * ty::ClampUnitHalfOpen(u1);
        const float localR =
            sqrtf(std::max(0.0f, 1.0f - ty::Sqr(localY)));
        const float phi = 2.0f * ty::Pi<float> * ty::ClampUnitHalfOpen(u2);
        const GfVec3f dirLight(
            localR * sinf(phi), localY, localR * cosf(phi));
        dirWld =
            light.xformLightToWorld.TransformDir(dirLight).GetNormalized();
    } else {
        const GfVec2f coordinateTexture = _SampleDomeUv(light.texture, u1, u2);
        const GfVec3f dirLight = _LatLongUvToDirection(coordinateTexture);
        dirWld =
            light.xformLightToWorld.TransformDir(dirLight).GetNormalized();
    }

    if (samplingMode ==
        ty::LightSampler::SamplingMode::ReflectionHemisphere) {
        const GfVec3f dirHemisphereWld =
            _FoldDirectionToReflectionHemisphere(
                dirWld, normalShdWldOut);
        if (dirHemisphereWld.GetLengthSq() > 0.0f) {
            return ty::EvaluateDomeLightDirection(
                light, dirHemisphereWld, normalShdWldOut, samplingMode,
                renderColorSpace);
        }
    }

    return ty::EvaluateDomeLightDirection(
        light, dirWld, renderColorSpace);
}

PXR_NAMESPACE_CLOSE_SCOPE
