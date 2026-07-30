//
// Copyright 2025 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
// Samples and evaluates distant lights in directional measure.
//
#include "lightSamplerCommon.h"
#include "lightSamplerDispatch.h"

#include "pxr/base/gf/vec3f.h"

#include <algorithm>
#include <cmath>
#include <limits>

PXR_NAMESPACE_OPEN_SCOPE

static bool
_GetDistantLightDirection(
    ty::LightData const& light, GfVec3f* outDirWld)
{
    if (!outDirWld) {
        return false;
    }

    const GfVec3f dirWld =
        light.xformLightToWorld.TransformDir(GfVec3f::ZAxis());
    if (!ty::IsFinite(dirWld) || dirWld.GetLengthSq() <= 0.0f) {
        return false;
    }

    *outDirWld = dirWld.GetNormalized();
    return true;
}

static float
_DistantHalfAngleRadians(ty::DistantLight const& distant)
{
    const float angle = std::isfinite(distant.angle)
        ? GfClamp(distant.angle, 0.0f, 360.0f)
        : 0.0f;
    const float halfAngle =
        0.5f * static_cast<float>(GfDegreesToRadians(angle));
    return GfClamp(halfAngle, 0.0f, ty::Pi);
}

static float
_DistantConeSolidAngle(float thetaMax)
{
    if (thetaMax <= 0.0f) {
        return 0.0f;
    }
    return 2.0f * ty::Pi *
        (1.0f - std::cos(GfClamp(thetaMax, 0.0f, ty::Pi)));
}

static float
_DistantNormalizeSizeFactor(float thetaMax)
{
    if (thetaMax <= 0.0f) {
        return 1.0f;
    }

    const float sinTheta =
        std::sin(GfClamp(thetaMax, 0.0f, ty::Pi));
    const float sinTheta2 = sinTheta * sinTheta;
    if (thetaMax <= 0.5f * ty::Pi) {
        return sinTheta2 * ty::Pi;
    }
    return (2.0f - sinTheta2) * ty::Pi;
}

static GfVec3f
_EvalDistantLightRadiance(
    ty::LightData const& light, ty::DistantLight const& distant,
    ty::RenderColorSpace renderColorSpace)
{
    GfVec3f radianceIn = ty::EvalLightBasic(light, renderColorSpace);
    if (light.normalize) {
        const float sizeFactor =
            _DistantNormalizeSizeFactor(_DistantHalfAngleRadians(distant));
        if (sizeFactor > 0.0f) {
            radianceIn /= sizeFactor;
        }
    }
    return radianceIn;
}

ty::LightSampler::LightSample
ty::EvaluateDistantLightDirection(
    ty::LightData const& light, ty::DistantLight const& distant,
    GfVec3f const& dirWld,
    ty::RenderColorSpace renderColorSpace)
{
    if (!ty::IsFinite(dirWld) || dirWld.GetLengthSq() <= 0.0f) {
        return ty::InvalidLightSample();
    }

    GfVec3f axis;
    if (!_GetDistantLightDirection(light, &axis)) {
        return ty::InvalidLightSample();
    }

    const float thetaMax = _DistantHalfAngleRadians(distant);
    const GfVec3f omegaInWld = dirWld.GetNormalized();
    const float cosTheta = GfDot(omegaInWld, axis);
    const GfVec3f radianceIn =
        _EvalDistantLightRadiance(light, distant, renderColorSpace);

    if (thetaMax <= 0.0f) {
        constexpr float directionEps = 1.0e-5f;
        if (cosTheta < 1.0f - directionEps) {
            return ty::InvalidLightSample();
        }
        return ty::LightSampler::LightSample{
            radianceIn, axis, std::numeric_limits<float>::max(),
            1.0f, true, true};
    }

    const float solidAngle = _DistantConeSolidAngle(thetaMax);
    const float cosThetaMax = std::cos(thetaMax);
    constexpr float coneEps = 1.0e-6f;
    if (solidAngle <= 0.0f || cosTheta < cosThetaMax - coneEps) {
        return ty::InvalidLightSample();
    }

    return ty::LightSampler::LightSample{
        radianceIn, omegaInWld, std::numeric_limits<float>::max(),
        solidAngle, true, false};
}

ty::LightSampler::LightSample
ty::SampleDistantLight(
    ty::LightData const& light, ty::DistantLight const& distant,
    float u1, float u2,
    ty::RenderColorSpace renderColorSpace)
{
    GfVec3f axis;
    if (!_GetDistantLightDirection(light, &axis)) {
        return ty::InvalidLightSample();
    }

    const float thetaMax = _DistantHalfAngleRadians(distant);
    if (thetaMax <= 0.0f) {
        return ty::EvaluateDistantLightDirection(
            light, distant, axis, renderColorSpace);
    }

    const float solidAngle = _DistantConeSolidAngle(thetaMax);
    if (solidAngle <= 0.0f) {
        return ty::InvalidLightSample();
    }

    GfVec3f tangent;
    GfVec3f bitangent;
    GfBuildOrthonormalFrame(axis, &tangent, &bitangent);

    const float cosThetaMax = std::cos(thetaMax);
    const float cosTheta =
        1.0f - ty::ClampUnitHalfOpen(u1) * (1.0f - cosThetaMax);
    const float sinTheta =
        std::sqrt(std::max(0.0f, 1.0f - ty::Sqr(cosTheta)));
    const float phi = 2.0f * ty::Pi * ty::ClampUnitHalfOpen(u2);
    const GfVec3f omegaInWld =
        (tangent * (sinTheta * std::cos(phi)) +
         bitangent * (sinTheta * std::sin(phi)) +
         axis * cosTheta).GetNormalized();

    return ty::LightSampler::LightSample{
        _EvalDistantLightRadiance(light, distant, renderColorSpace),
        omegaInWld, std::numeric_limits<float>::max(),
        solidAngle, true, false};
}

PXR_NAMESPACE_CLOSE_SCOPE
