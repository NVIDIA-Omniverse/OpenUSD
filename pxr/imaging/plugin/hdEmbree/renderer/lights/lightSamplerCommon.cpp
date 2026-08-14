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

static float
_ProjectedDiskAreaSceneUnits(
    ty::LightData const& light,
    ty::DiskLight const& disk,
    GfVec3f const& omegaInWld)
{
    const GfVec3f radiusXWld = light.xformLightToWorld.TransformDir(
        GfVec3f(disk.radius, 0.0f, 0.0f));
    const GfVec3f radiusYWld = light.xformLightToWorld.TransformDir(
        GfVec3f(0.0f, disk.radius, 0.0f));
    return ty::Pi * std::abs(GfDot(
        omegaInWld, GfCross(radiusXWld, radiusYWld)));
}

static float
_ProjectedSphereAreaSceneUnits(
    ty::LightData const& light,
    ty::SphereLight const& sphere,
    GfVec3f const& omegaInWld)
{
    const GfVec3f axisXWld = light.xformLightToWorld.TransformDir(
        GfVec3f(sphere.radius, 0.0f, 0.0f));
    const GfVec3f axisYWld = light.xformLightToWorld.TransformDir(
        GfVec3f(0.0f, sphere.radius, 0.0f));
    const GfVec3f axisZWld = light.xformLightToWorld.TransformDir(
        GfVec3f(0.0f, 0.0f, sphere.radius));
    const float projectedYZ = GfDot(
        omegaInWld, GfCross(axisYWld, axisZWld));
    const float projectedZX = GfDot(
        omegaInWld, GfCross(axisZWld, axisXWld));
    const float projectedXY = GfDot(
        omegaInWld, GfCross(axisXWld, axisYWld));
    return ty::Pi * std::sqrt(
        ty::Sqr(projectedYZ) + ty::Sqr(projectedZX) +
        ty::Sqr(projectedXY));
}

static float
_ProjectedCylinderAreaSceneUnits(
    ty::LightData const& light,
    ty::CylinderLight const& cylinder,
    GfVec3f const& omegaInWld)
{
    const GfVec3f axisWld = light.xformLightToWorld.TransformDir(
        GfVec3f(cylinder.length, 0.0f, 0.0f));
    const GfVec3f radiusYWld = light.xformLightToWorld.TransformDir(
        GfVec3f(0.0f, cylinder.radius, 0.0f));
    const GfVec3f radiusZWld = light.xformLightToWorld.TransformDir(
        GfVec3f(0.0f, 0.0f, cylinder.radius));
    const float projectedY = GfDot(
        omegaInWld, GfCross(axisWld, radiusYWld));
    const float projectedZ = GfDot(
        omegaInWld, GfCross(axisWld, radiusZWld));
    return 2.0f * std::sqrt(
        ty::Sqr(projectedY) + ty::Sqr(projectedZ));
}

static float
_ProjectedEmitterAreaSceneUnits(
    ty::LightData const& light,
    ty::ShapeSample const& shapeSample,
    GfVec3f const& omegaInWld,
    float cosThetaOffNormal)
{
    if (std::holds_alternative<ty::RectLight>(light.lightVariant)) {
        return shapeSample.pdfAreaInverse * cosThetaOffNormal;
    }
    if (const ty::DiskLight* disk =
            std::get_if<ty::DiskLight>(&light.lightVariant)) {
        return _ProjectedDiskAreaSceneUnits(light, *disk, omegaInWld);
    }
    if (const ty::SphereLight* sphere =
            std::get_if<ty::SphereLight>(&light.lightVariant)) {
        return _ProjectedSphereAreaSceneUnits(light, *sphere, omegaInWld);
    }
    if (const ty::CylinderLight* cylinder =
            std::get_if<ty::CylinderLight>(&light.lightVariant)) {
        return _ProjectedCylinderAreaSceneUnits(
            light, *cylinder, omegaInWld);
    }
    return 0.0f;
}

static GfVec3f
_BlackbodyTemperatureAsRgb(
    float kelvinColorTemp, ty::RenderColorSpace renderColorSpace)
{
    // Recreate the UsdLux utility without adding a USD dependency to imaging.
    const GfColorSpace workingColorSpace(
        ty::GetWorkingColorSpaceToken(renderColorSpace));
    GfColor tempColor(workingColorSpace);
    tempColor.SetFromPlanckianLocus(kelvinColorTemp, 1.0f);
    const GfVec3f tempColorRGB = tempColor.GetRGB();
    const float luminance = GfDot(
        tempColorRGB, ty::GetLuminanceCoefficients(renderColorSpace));
    return luminance > 0.0f ? tempColorRGB / luminance : GfVec3f(1.0f);
}

GfVec3f
ty::SampleLightTexture(
    ty::LightTexture const& texture,
    float coordinateTextureS,
    float coordinateTextureT,
    ty::RenderColorSpace renderColorSpace)
{
    if (texture.pixels.empty()) {
        return GfVec3f(0.0f);
    }

    const int indexTexelX = std::clamp(
        static_cast<int>(
            static_cast<float>(texture.width) *
            ty::WrapUnit(coordinateTextureS)),
        0,
        texture.width - 1);
    const int indexTexelY = std::clamp(
        static_cast<int>(static_cast<float>(texture.height) *
                         ty::ClampUnitHalfOpen(coordinateTextureT)),
        0,
        texture.height - 1);

    GfVec3f result =
        texture.pixels.at(indexTexelY * texture.width + indexTexelX);
    ty::ConvertToRenderColorSpace(
        texture.colorSpaceName.GetString(), renderColorSpace, &result);
    return result;
}

static GfVec3f
_SampleRectLightTexture(
    ty::LightTexture const& texture, GfVec2f const& coordinateTexture,
    ty::RenderColorSpace renderColorSpace)
{
    if (texture.pixels.empty() || texture.width <= 0 || texture.height <= 0) {
        return GfVec3f(0.0f);
    }

    // Rect texture coordinates retain the delegate's established flipped
    // local-axis convention.
    const float coordinateTextureS = 1.0f - coordinateTexture[0];
    const float coordinateTextureT = 1.0f - coordinateTexture[1];
    const int indexTexelX = std::clamp(
        static_cast<int>(static_cast<float>(texture.width) *
                         ty::ClampUnitHalfOpen(coordinateTextureS)),
        0, texture.width - 1);
    const int indexTexelY = std::clamp(
        static_cast<int>(static_cast<float>(texture.height) *
                         ty::ClampUnitHalfOpen(coordinateTextureT)),
        0, texture.height - 1);

    GfVec3f result =
        texture.pixels.at(indexTexelY * texture.width + indexTexelX);
    ty::ConvertToRenderColorSpace(
        texture.colorSpaceName.GetString(), renderColorSpace, &result);
    return result;
}

GfVec3f
ty::EvalLightBasic(
    ty::LightData const& light,
    ty::RenderColorSpace renderColorSpace)
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
    radianceEmitted *= light.physicalScale;
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
    ty::LightData const& light, GfVec3f const& dirWld,
    GfVec3f* dirLight)
{
    if (!dirLight || dirWld.GetLengthSq() <= 0.0f ||
        !ty::IsFinite(dirWld)) {
        return 0.0f;
    }

    const GfVec3f omegaInWld = dirWld.GetNormalized();
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

    *dirLight = localUnnormalized / localLength;
    return detWorldToLight / (localLength * localLength * localLength);
}

static float
_DirectionalShapingPdfSolidAngle(
    ty::LightData const& light, GfVec3f const& dirWld,
    bool foldToFrontHemisphere)
{
    if (!light.shaping.directionalDistribution.IsValid() ||
        dirWld.GetLengthSq() <= 0.0f ||
        !ty::IsFinite(dirWld)) {
        return 0.0f;
    }

    GfVec3f dirLight;
    const float pdfScale =
        _WorldToLocalDirectionPdfScale(light, dirWld, &dirLight);
    if (pdfScale <= 0.0f) {
        return 0.0f;
    }
    float localPdf =
        ty::DirectionalShapingPdf(light.shaping, dirLight);
    if (foldToFrontHemisphere) {
        if (dirLight[2] < 0.0f) {
            return 0.0f;
        }
        const GfVec3f mirrored(
            dirLight[0], dirLight[1], -dirLight[2]);
        localPdf += ty::DirectionalShapingPdf(light.shaping, mirrored);
    }
    return localPdf * pdfScale;
}

static float
_ShapingAwareFinitePdfSolidAngle(
    ty::LightData const& light, float pdfAreaProposalSolidAngleInverse,
    GfVec3f const& dirWld,
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
        light, dirWld, foldToFrontHemisphere);
    return ty::ShapingAwareFiniteAreaProposalWeight *
            pdfAreaProposalSolidAngle +
        ty::ShapingAwareFiniteDirectionalProposalWeight *
            pdfShapingSolidAngle;
}

void
ty::ApplyShapingAwareFinitePdf(
    ty::LightData const& light, ty::LightSampler::LightSample* sample,
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

ty::LightSampler::LightSample
ty::EvalAreaLight(
    ty::LightData const& light, ty::ShapeSample const& ss,
    GfVec3f const& posWld,
    ty::RenderColorSpace renderColorSpace)
{
    // Transform the PDF from area measure to solid-angle measure.
    GfVec3f omegaInWld = ss.posWld - posWld;
    const float distanceWld = omegaInWld.GetLength();
    if (distanceWld <= 0.0f || !std::isfinite(distanceWld)) {
        return ty::InvalidLightSample();
    }
    omegaInWld /= distanceWld;
    const float cosThetaOffNormal =
        _DotZeroClip(-omegaInWld, ss.normalGeomWldExt);
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
            std::holds_alternative<ty::RectLight>(light.lightVariant)
            ? _SampleRectLightTexture(
                  light.texture, ss.coordinateTexture, renderColorSpace)
            : ty::SampleLightTexture(
                  light.texture, ss.coordinateTexture[0],
                  1.0f - ss.coordinateTexture[1],
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
        ty::EvaluateDirectionalShaping(light.shaping, omegaInLocal));

    if (light.shaping.ies.convertCandelaToLuminance) {
        const float projectedAreaSceneUnits =
            _ProjectedEmitterAreaSceneUnits(
                light, ss, omegaInWld, cosThetaOffNormal);
        const float metersPerUnit = light.shaping.ies.metersPerUnit;
        const float projectedAreaPhysical = projectedAreaSceneUnits *
            metersPerUnit * metersPerUnit;
        radianceEmitted = projectedAreaPhysical > 0.0f &&
                std::isfinite(projectedAreaPhysical)
            ? radianceEmitted / projectedAreaPhysical
            : GfVec3f(0.0f);
    }

    return ty::LightSampler::LightSample{
        radianceEmitted, omegaInWld, distanceWld, pdfSolidAngleInverse,
        pdfSolidAngleInverse > 0.0f && std::isfinite(distanceWld)};
}

PXR_NAMESPACE_CLOSE_SCOPE
