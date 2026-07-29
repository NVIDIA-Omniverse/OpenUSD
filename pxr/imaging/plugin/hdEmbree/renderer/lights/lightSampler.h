//
// Copyright 2025 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
// Defines the renderer-facing light sampling and directional-evaluation
// dispatch surface.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_LIGHT_SAMPLER_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_LIGHT_SAMPLER_H

#include "light.h"

#include <renderer/colorManagement.h>

#include "pxr/base/gf/vec3f.h"
#include "pxr/pxr.h"

PXR_NAMESPACE_OPEN_SCOPE
namespace ty {

/// Utility class that dispatches sampling and fixed-direction evaluation to
/// the active light type.
class LightSampler {
public:
    enum class SamplingMode {
        FullSphere,
        ReflectionHemisphere
    };

    struct LightSample {
        /// Incident radiance arriving from the sampled light.
        GfVec3f radianceIn;
        /// Normalized world-space direction from the shading point to light.
        GfVec3f omegaInWld;
        /// Non-negative world-space distance; infinity for infinite lights.
        float distanceWld;
        /// Reciprocal solid-angle PDF for the selected sampling technique.
        float pdfSolidAngleInverse;
        /// True only when every field above describes a usable sample.
        bool valid;
        /// True when the light has a delta directional distribution.
        bool delta = false;
    };

    static LightSample
    GetLightSample(LightData const& lightData,
                   GfVec3f const& posHitWld,
                   GfVec3f const& normalShdWldOut, float u1, float u2,
                   SamplingMode samplingMode = SamplingMode::FullSphere,
                   RenderColorSpace renderColorSpace =
                       RenderColorSpace::LinearRec709);

    /// Evaluates a dome light along a fixed direction and returns the
    /// corresponding radiance and directional PDF.
    static LightSample
    EvaluateDomeLightDirection(LightData const& lightData,
                               GfVec3f const& omegaInWld,
                               RenderColorSpace renderColorSpace =
                                   RenderColorSpace::LinearRec709);

    /// Evaluates a dome light along a fixed direction with the PDF used by
    /// the selected dome-light sampling mode.
    static LightSample EvaluateDomeLightDirection(
        LightData const& lightData, GfVec3f const& omegaInWld,
        GfVec3f const& normalShdWldOut, SamplingMode samplingMode,
        RenderColorSpace renderColorSpace =
            RenderColorSpace::LinearRec709);

    /// Evaluates a light along a fixed direction from a point and returns the
    /// corresponding radiance, distance, and directional PDF when the ray
    /// intersects the light shape.
    static LightSample
    EvaluateLightDirection(LightData const& lightData,
                           GfVec3f const& posHitWld,
                           GfVec3f const& omegaInWld,
                           RenderColorSpace renderColorSpace =
                               RenderColorSpace::LinearRec709);

    LightSample operator()(UnknownLight const& unknown);
    LightSample operator()(RectLight const& rect);
    LightSample operator()(SphereLight const& sphere);
    LightSample operator()(DiskLight const& disk);
    LightSample operator()(DistantLight const& distant);
    LightSample operator()(CylinderLight const& cylinder);
    LightSample operator()(DomeLight const& dome);

private:
    LightSampler(LightData const& lightData,
                         GfVec3f const& posHitWld,
                         GfVec3f const& normalShdWldOut, float u1, float u2,
                         SamplingMode samplingMode,
                         RenderColorSpace renderColorSpace)
        : _lightData(lightData), _posHitWld(posHitWld),
          _normalShdWldOut(normalShdWldOut), _u1(u1), _u2(u2),
          _samplingMode(samplingMode), _renderColorSpace(renderColorSpace)
    {
    }

    LightData const& _lightData;
    GfVec3f const& _posHitWld;
    GfVec3f const& _normalShdWldOut;
    float _u1;
    float _u2;
    SamplingMode _samplingMode;
    RenderColorSpace _renderColorSpace;
};

} // namespace ty
PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_IMAGING_PLUGIN_HD_EMBREE_LIGHT_SAMPLER_H
