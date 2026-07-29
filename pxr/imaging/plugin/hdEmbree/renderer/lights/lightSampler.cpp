//
// Copyright 2025 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
// Dispatches renderer-facing light sampling and evaluation to per-type
// implementations.
//
#include "lightSampler.h"

#include "lightSamplerCommon.h"
#include "lightSamplerDispatch.h"

PXR_NAMESPACE_OPEN_SCOPE

static HdEmbreeLightSampler::LightSample
_EvaluateLightDirection(
    HdEmbree_LightData const& light,
    GfVec3f const& position,
    GfVec3f const& direction,
    HdEmbreeRenderColorSpace renderColorSpace)
{
    if (HdEmbree_Rect const* const rect =
            std::get_if<HdEmbree_Rect>(&light.lightVariant)) {
        return ty::EvaluateRectLightDirection(
            light, *rect, position, direction, renderColorSpace);
    }
    if (HdEmbree_Sphere const* const sphere =
            std::get_if<HdEmbree_Sphere>(&light.lightVariant)) {
        return ty::EvaluateSphereLightDirection(
            light, *sphere, position, direction, renderColorSpace);
    }
    if (HdEmbree_Disk const* const disk =
            std::get_if<HdEmbree_Disk>(&light.lightVariant)) {
        return ty::EvaluateDiskLightDirection(
            light, *disk, position, direction, renderColorSpace);
    }
    if (HdEmbree_Cylinder const* const cylinder =
            std::get_if<HdEmbree_Cylinder>(&light.lightVariant)) {
        return ty::EvaluateCylinderLightDirection(
            light, *cylinder, position, direction, renderColorSpace);
    }
    if (HdEmbree_Distant const* const distant =
            std::get_if<HdEmbree_Distant>(&light.lightVariant)) {
        return ty::EvaluateDistantLightDirection(
            light, *distant, direction, renderColorSpace);
    }
    if (std::holds_alternative<HdEmbree_Dome>(light.lightVariant)) {
        return ty::EvaluateDomeLightDirection(
            light, direction, renderColorSpace);
    }
    return ty::InvalidLightSample();
}

HdEmbreeLightSampler::LightSample
HdEmbreeLightSampler::GetLightSample(
    HdEmbree_LightData const& lightData, GfVec3f const& positionHitWld,
    GfVec3f const& normalShdWldOut, float u1, float u2,
    SamplingMode samplingMode,
    HdEmbreeRenderColorSpace renderColorSpace)
{
    HdEmbreeLightSampler lightSampler(lightData, positionHitWld,
                                      normalShdWldOut, u1, u2, samplingMode,
                                      renderColorSpace);
    return std::visit(lightSampler, lightData.lightVariant);
}

HdEmbreeLightSampler::LightSample
HdEmbreeLightSampler::EvaluateDomeLightDirection(
    HdEmbree_LightData const& lightData, GfVec3f const& omegaInWld,
    HdEmbreeRenderColorSpace renderColorSpace)
{
    return ty::EvaluateDomeLightDirection(
        lightData, omegaInWld, renderColorSpace);
}

HdEmbreeLightSampler::LightSample
HdEmbreeLightSampler::EvaluateDomeLightDirection(
    HdEmbree_LightData const& lightData, GfVec3f const& omegaInWld,
    GfVec3f const& normalShdWldOut, SamplingMode samplingMode,
    HdEmbreeRenderColorSpace renderColorSpace)
{
    return ty::EvaluateDomeLightDirection(lightData, omegaInWld,
        normalShdWldOut, samplingMode, renderColorSpace);
}

HdEmbreeLightSampler::LightSample
HdEmbreeLightSampler::EvaluateLightDirection(
    HdEmbree_LightData const& lightData, GfVec3f const& positionHitWld,
    GfVec3f const& omegaInWld,
    HdEmbreeRenderColorSpace renderColorSpace)
{
    return _EvaluateLightDirection(
        lightData, positionHitWld, omegaInWld, renderColorSpace);
}

HdEmbreeLightSampler::LightSample
HdEmbreeLightSampler::operator()(HdEmbree_UnknownLight const&)
{
    // The delegate already warns when constructing an unknown variant.
    // Warning per sample here would produce excessive diagnostic spam.
    return ty::InvalidLightSample();
}

HdEmbreeLightSampler::LightSample
HdEmbreeLightSampler::operator()(HdEmbree_Rect const& rect)
{
    return ty::SampleRectLight(_lightData, rect, _positionHitWld, _u1, _u2,
                               _renderColorSpace);
}

HdEmbreeLightSampler::LightSample
HdEmbreeLightSampler::operator()(HdEmbree_Sphere const& sphere)
{
    return ty::SampleSphereLight(_lightData, sphere, _positionHitWld, _u1,
                                 _u2, _renderColorSpace);
}

HdEmbreeLightSampler::LightSample
HdEmbreeLightSampler::operator()(HdEmbree_Disk const& disk)
{
    return ty::SampleDiskLight(_lightData, disk, _positionHitWld, _u1, _u2,
                               _renderColorSpace);
}

HdEmbreeLightSampler::LightSample
HdEmbreeLightSampler::operator()(HdEmbree_Distant const& distant)
{
    return ty::SampleDistantLight(
        _lightData, distant, _u1, _u2, _renderColorSpace);
}

HdEmbreeLightSampler::LightSample
HdEmbreeLightSampler::operator()(HdEmbree_Cylinder const& cylinder)
{
    return ty::SampleCylinderLight(_lightData, cylinder, _positionHitWld,
                                   _u1, _u2, _renderColorSpace);
}

HdEmbreeLightSampler::LightSample
HdEmbreeLightSampler::operator()(HdEmbree_Dome const&)
{
    return ty::SampleDomeLight(_lightData, _normalShdWldOut, _u1, _u2,
                               _samplingMode, _renderColorSpace);
}

PXR_NAMESPACE_CLOSE_SCOPE
