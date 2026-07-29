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

static ty::LightSampler::LightSample
_EvaluateLightDirection(
    ty::LightData const& light,
    GfVec3f const& posWld,
    GfVec3f const& dirWld,
    ty::RenderColorSpace renderColorSpace)
{
    if (ty::RectLight const* const rect =
            std::get_if<ty::RectLight>(&light.lightVariant)) {
        return ty::EvaluateRectLightDirection(
            light, *rect, posWld, dirWld, renderColorSpace);
    }
    if (ty::SphereLight const* const sphere =
            std::get_if<ty::SphereLight>(&light.lightVariant)) {
        return ty::EvaluateSphereLightDirection(
            light, *sphere, posWld, dirWld, renderColorSpace);
    }
    if (ty::DiskLight const* const disk =
            std::get_if<ty::DiskLight>(&light.lightVariant)) {
        return ty::EvaluateDiskLightDirection(
            light, *disk, posWld, dirWld, renderColorSpace);
    }
    if (ty::CylinderLight const* const cylinder =
            std::get_if<ty::CylinderLight>(&light.lightVariant)) {
        return ty::EvaluateCylinderLightDirection(
            light, *cylinder, posWld, dirWld, renderColorSpace);
    }
    if (ty::DistantLight const* const distant =
            std::get_if<ty::DistantLight>(&light.lightVariant)) {
        return ty::EvaluateDistantLightDirection(
            light, *distant, dirWld, renderColorSpace);
    }
    if (std::holds_alternative<ty::DomeLight>(light.lightVariant)) {
        return ty::EvaluateDomeLightDirection(
            light, dirWld, renderColorSpace);
    }
    return ty::InvalidLightSample();
}

ty::LightSampler::LightSample
ty::LightSampler::GetLightSample(
    ty::LightData const& lightData, GfVec3f const& posHitWld,
    GfVec3f const& normalShdWldOut, float u1, float u2,
    SamplingMode samplingMode,
    ty::RenderColorSpace renderColorSpace)
{
    ty::LightSampler lightSampler(lightData, posHitWld,
                                      normalShdWldOut, u1, u2, samplingMode,
                                      renderColorSpace);
    return std::visit(lightSampler, lightData.lightVariant);
}

ty::LightSampler::LightSample
ty::LightSampler::EvaluateDomeLightDirection(
    ty::LightData const& lightData, GfVec3f const& omegaInWld,
    ty::RenderColorSpace renderColorSpace)
{
    return ty::EvaluateDomeLightDirection(
        lightData, omegaInWld, renderColorSpace);
}

ty::LightSampler::LightSample
ty::LightSampler::EvaluateDomeLightDirection(
    ty::LightData const& lightData, GfVec3f const& omegaInWld,
    GfVec3f const& normalShdWldOut, SamplingMode samplingMode,
    ty::RenderColorSpace renderColorSpace)
{
    return ty::EvaluateDomeLightDirection(lightData, omegaInWld,
        normalShdWldOut, samplingMode, renderColorSpace);
}

ty::LightSampler::LightSample
ty::LightSampler::EvaluateLightDirection(
    ty::LightData const& lightData, GfVec3f const& posHitWld,
    GfVec3f const& omegaInWld,
    ty::RenderColorSpace renderColorSpace)
{
    return _EvaluateLightDirection(
        lightData, posHitWld, omegaInWld, renderColorSpace);
}

ty::LightSampler::LightSample
ty::LightSampler::operator()(ty::UnknownLight const&)
{
    // The delegate already warns when constructing an unknown variant.
    // Warning per sample here would produce excessive diagnostic spam.
    return ty::InvalidLightSample();
}

ty::LightSampler::LightSample
ty::LightSampler::operator()(ty::RectLight const& rect)
{
    return ty::SampleRectLight(_lightData, rect, _posHitWld, _u1, _u2,
                               _renderColorSpace);
}

ty::LightSampler::LightSample
ty::LightSampler::operator()(ty::SphereLight const& sphere)
{
    return ty::SampleSphereLight(_lightData, sphere, _posHitWld, _u1,
                                 _u2, _renderColorSpace);
}

ty::LightSampler::LightSample
ty::LightSampler::operator()(ty::DiskLight const& disk)
{
    return ty::SampleDiskLight(_lightData, disk, _posHitWld, _u1, _u2,
                               _renderColorSpace);
}

ty::LightSampler::LightSample
ty::LightSampler::operator()(ty::DistantLight const& distant)
{
    return ty::SampleDistantLight(
        _lightData, distant, _u1, _u2, _renderColorSpace);
}

ty::LightSampler::LightSample
ty::LightSampler::operator()(ty::CylinderLight const& cylinder)
{
    return ty::SampleCylinderLight(_lightData, cylinder, _posHitWld,
                                   _u1, _u2, _renderColorSpace);
}

ty::LightSampler::LightSample
ty::LightSampler::operator()(ty::DomeLight const&)
{
    return ty::SampleDomeLight(_lightData, _normalShdWldOut, _u1, _u2,
                               _samplingMode, _renderColorSpace);
}

PXR_NAMESPACE_CLOSE_SCOPE
