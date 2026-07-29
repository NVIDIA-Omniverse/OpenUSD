//
// Copyright 2025 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
// Applies shared light radiometry after type-specific geometry is resolved.
//
#include "lightSamplerCommon.h"

#include "pxr/base/gf/color.h"
#include "pxr/base/gf/colorSpace.h"

#include <algorithm>
#include <cmath>

PXR_NAMESPACE_OPEN_SCOPE

// Dot product clipped to zero for backward-facing rays.
static float
_DotZeroClip(GfVec3f const& a, GfVec3f const& b)
{
    return std::max(0.0f, GfDot(a, b));
}

static GfVec3f
_BlackbodyTemperatureAsRgb(
    float kelvinColorTemp, HdEmbreeRenderColorSpace renderColorSpace)
{
    // Recreate the UsdLux utility without adding a USD dependency to imaging.
    const GfColorSpace workingColorSpace(
        HdEmbreeGetWorkingColorSpaceToken(renderColorSpace));
    GfColor tempColor(workingColorSpace);
    tempColor.SetFromPlanckianLocus(kelvinColorTemp, 1.0f);
    const GfVec3f tempColorRGB = tempColor.GetRGB();
    const float luminance = GfDot(
        tempColorRGB, HdEmbreeGetLuminanceCoefficients(renderColorSpace));
    return luminance > 0.0f ? tempColorRGB / luminance : GfVec3f(1.0f);
}

GfVec3f
ty::SampleLightTexture(
    HdEmbree_LightTexture const& texture, float s, float t,
    HdEmbreeRenderColorSpace renderColorSpace)
{
    if (texture.pixels.empty()) {
        return GfVec3f(0.0f);
    }

    const int x = std::clamp(
        static_cast<int>(static_cast<float>(texture.width) * ty::WrapUnit(s)),
        0,
        texture.width - 1);
    const int y = std::clamp(
        static_cast<int>(static_cast<float>(texture.height) *
                         ty::ClampUnitHalfOpen(t)),
        0,
        texture.height - 1);

    GfVec3f result = texture.pixels.at(y * texture.width + x);
    HdEmbreeConvertToRenderColorSpace(
        texture.colorSpaceName.GetString(), renderColorSpace, &result);
    return result;
}

static GfVec3f
_SampleRectLightTexture(
    HdEmbree_LightTexture const& texture, GfVec2f const& uv,
    HdEmbreeRenderColorSpace renderColorSpace)
{
    if (texture.pixels.empty() || texture.width <= 0 || texture.height <= 0) {
        return GfVec3f(0.0f);
    }

    // Rect texture coordinates retain the delegate's established flipped
    // local-axis convention.
    const float s = 1.0f - uv[0];
    const float t = 1.0f - uv[1];
    const int x = std::clamp(
        static_cast<int>(static_cast<float>(texture.width) *
                         ty::ClampUnitHalfOpen(s)),
        0, texture.width - 1);
    const int y = std::clamp(
        static_cast<int>(static_cast<float>(texture.height) *
                         ty::ClampUnitHalfOpen(t)),
        0, texture.height - 1);

    GfVec3f result = texture.pixels.at(y * texture.width + x);
    HdEmbreeConvertToRenderColorSpace(
        texture.colorSpaceName.GetString(), renderColorSpace, &result);
    return result;
}

GfVec3f
ty::EvalLightBasic(
    HdEmbree_LightData const& light,
    HdEmbreeRenderColorSpace renderColorSpace)
{
    // The material model is fully diffuse, so the USD diffuse parameter is a
    // direct radiance multiplier.
    GfVec3f radianceEmitted = light.color * light.intensity * light.diffuse *
                              powf(2.0f, light.exposure);
    if (light.enableColorTemperature) {
        radianceEmitted = GfCompMult(radianceEmitted,
            _BlackbodyTemperatureAsRgb(
                light.colorTemperature, renderColorSpace));
    }
    return radianceEmitted;
}

static float
_PdfSolidAngleFromInverse(float pdfSolidAngleInverse)
{
    return (pdfSolidAngleInverse > 0.0f &&
            std::isfinite(pdfSolidAngleInverse))
        ? (1.0f / pdfSolidAngleInverse)
        : 0.0f;
}

static float
_WorldToLocalDirectionPdfScale(
    HdEmbree_LightData const& light, GfVec3f const& worldDirection,
    GfVec3f* localDirection)
{
    if (!localDirection || worldDirection.GetLengthSq() <= 0.0f ||
        !ty::IsFinite(worldDirection)) {
        return 0.0f;
    }

    const GfVec3f omegaInWld = worldDirection.GetNormalized();
    const GfVec3f localUnnormalized =
        light.xformWorldToLight.TransformDir(omegaInWld);
    const float localLength = localUnnormalized.GetLength();
    if (localLength <= 0.0f || !std::isfinite(localLength)) {
        return 0.0f;
    }

    const GfVec3f bx =
        light.xformWorldToLight.TransformDir(GfVec3f::XAxis());
    const GfVec3f by =
        light.xformWorldToLight.TransformDir(GfVec3f::YAxis());
    const GfVec3f bz =
        light.xformWorldToLight.TransformDir(GfVec3f::ZAxis());
    const float detWorldToLight = std::abs(GfDot(bx, GfCross(by, bz)));
    if (detWorldToLight <= 0.0f || !std::isfinite(detWorldToLight)) {
        return 0.0f;
    }

    *localDirection = localUnnormalized / localLength;
    return detWorldToLight / (localLength * localLength * localLength);
}

static float
_DirectionalShapingPdfSolidAngle(
    HdEmbree_LightData const& light, GfVec3f const& worldDirection,
    bool foldToFrontHemisphere)
{
    if (!light.shaping.directionalDistribution.IsValid() ||
        worldDirection.GetLengthSq() <= 0.0f ||
        !ty::IsFinite(worldDirection)) {
        return 0.0f;
    }

    GfVec3f localDirection;
    const float pdfScale =
        _WorldToLocalDirectionPdfScale(light, worldDirection, &localDirection);
    if (pdfScale <= 0.0f) {
        return 0.0f;
    }
    float localPdf =
        HdEmbreeDirectionalShapingPdf(light.shaping, localDirection);
    if (foldToFrontHemisphere) {
        if (localDirection[2] < 0.0f) {
            return 0.0f;
        }
        const GfVec3f mirrored(
            localDirection[0], localDirection[1], -localDirection[2]);
        localPdf += HdEmbreeDirectionalShapingPdf(light.shaping, mirrored);
    }
    return localPdf * pdfScale;
}

static float
_ShapingAwareFinitePdfSolidAngle(
    HdEmbree_LightData const& light, float pdfAreaProposalSolidAngleInverse,
    GfVec3f const& worldDirection,
    bool foldToFrontHemisphere)
{
    const float pdfAreaProposalSolidAngle =
        _PdfSolidAngleFromInverse(pdfAreaProposalSolidAngleInverse);
    if (!light.shaping.directionalDistribution.IsValid()) {
        return pdfAreaProposalSolidAngle;
    }

    // The finite emitter remains the source of truth. The extra directional
    // proposal only changes how we sample it, so both proposals contribute to
    // the PDF used by direct-light and emitter-hit MIS.
    const float pdfShapingSolidAngle = _DirectionalShapingPdfSolidAngle(
        light, worldDirection, foldToFrontHemisphere);
    return ty::ShapingAwareFiniteAreaProposalWeight *
            pdfAreaProposalSolidAngle +
        ty::ShapingAwareFiniteDirectionalProposalWeight *
            pdfShapingSolidAngle;
}

void
ty::ApplyShapingAwareFinitePdf(
    HdEmbree_LightData const& light, HdEmbreeLightSampler::LightSample* sample,
    bool foldToFrontHemisphere)
{
    if (!sample || !sample->valid || sample->delta) {
        return;
    }

    const float pdfSolidAngle = _ShapingAwareFinitePdfSolidAngle(
        light, sample->pdfSolidAngleInverse, sample->omegaInWld,
        foldToFrontHemisphere);
    sample->pdfSolidAngleInverse =
        (pdfSolidAngle > 0.0f) ? (1.0f / pdfSolidAngle) : 0.0f;
    sample->valid = sample->valid && sample->pdfSolidAngleInverse > 0.0f;
}

HdEmbreeLightSampler::LightSample
ty::EvalAreaLight(
    HdEmbree_LightData const& light, ty::ShapeSample const& ss,
    GfVec3f const& position,
    HdEmbreeRenderColorSpace renderColorSpace)
{
    // Transform the PDF from area measure to solid-angle measure.
    GfVec3f omegaInWld = ss.pWorld - position;
    const float distanceWld = omegaInWld.GetLength();
    if (distanceWld <= 0.0f || !std::isfinite(distanceWld)) {
        return ty::InvalidLightSample();
    }
    omegaInWld /= distanceWld;
    const float cosThetaOffNormal =
        _DotZeroClip(-omegaInWld, ss.nWorld);
    const float pdfSolidAngleInverse =
        cosThetaOffNormal / ty::Sqr(distanceWld) * ss.pdfAreaInverse;

    // Combine the brightness parameters only on the emitting side.
    GfVec3f radianceEmitted =
        cosThetaOffNormal > 0.0f
        ? ty::EvalLightBasic(light, renderColorSpace)
        : GfVec3f(0.0f);

    // Rect textures retain their distinct coordinate convention until that
    // behavior is changed and image-tested independently.
    if (!light.texture.pixels.empty()) {
        const GfVec3f textureColor =
            std::holds_alternative<HdEmbree_Rect>(light.lightVariant)
            ? _SampleRectLightTexture(
                  light.texture, ss.uv, renderColorSpace)
            : ty::SampleLightTexture(
                  light.texture, ss.uv[0], 1.0f - ss.uv[1],
                  renderColorSpace);
        radianceEmitted = GfCompMult(radianceEmitted, textureColor);
    }

    // Normalized area lights preserve power as their surface area changes.
    if (light.normalize && ss.pdfAreaInverse != 0.0f) {
        radianceEmitted /= ss.pdfAreaInverse;
    }

    const GfVec3f omegaInLocal =
        light.xformWorldToLight.TransformDir(omegaInWld).GetNormalized();
    radianceEmitted = GfCompMult(
        radianceEmitted,
        HdEmbreeEvaluateDirectionalShaping(light.shaping, omegaInLocal));

    return HdEmbreeLightSampler::LightSample{
        radianceEmitted, omegaInWld, distanceWld, pdfSolidAngleInverse,
        pdfSolidAngleInverse > 0.0f && std::isfinite(distanceWld)};
}

PXR_NAMESPACE_CLOSE_SCOPE
