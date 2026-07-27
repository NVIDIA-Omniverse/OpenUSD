//
// Copyright 2025 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_LIGHT_SAMPLERS_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_LIGHT_SAMPLERS_H

#include "pxr/pxr.h"
#include "pxr/base/gf/vec3f.h"

#include "pxr/imaging/plugin/hdEmbree/renderer/colorManagement.h"
#include "pxr/imaging/plugin/hdEmbree/renderer/lights/light.h"

PXR_NAMESPACE_OPEN_SCOPE

/// \class HdEmbreeLightSampler
///
/// Utility class to help sample Embree lights for direct lighting.
class HdEmbreeLightSampler {
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
    GetLightSample(HdEmbree_LightData const& lightData,
                   GfVec3f const& positionHitWld,
                   GfVec3f const& normalShdWldOut, float u1, float u2,
                   SamplingMode samplingMode = SamplingMode::FullSphere,
                   HdEmbreeRenderColorSpace renderColorSpace =
                       HdEmbreeRenderColorSpace::LinearRec709);

    /// Evaluates a dome light along a fixed direction and returns the
    /// corresponding radiance and directional PDF.
    static LightSample
    EvaluateDomeLightDirection(HdEmbree_LightData const& lightData,
                               GfVec3f const& omegaInWld,
                               HdEmbreeRenderColorSpace renderColorSpace =
                                   HdEmbreeRenderColorSpace::LinearRec709);

    /// Evaluates a dome light along a fixed direction with the PDF used by
    /// the selected dome-light sampling mode.
    static LightSample EvaluateDomeLightDirection(
        HdEmbree_LightData const& lightData, GfVec3f const& omegaInWld,
        GfVec3f const& normalShdWldOut, SamplingMode samplingMode,
        HdEmbreeRenderColorSpace renderColorSpace =
            HdEmbreeRenderColorSpace::LinearRec709);

    /// Evaluates a light along a fixed direction from a point and returns the
    /// corresponding radiance, distance, and directional PDF when the ray
    /// intersects the light shape.
    static LightSample
    EvaluateLightDirection(HdEmbree_LightData const& lightData,
                           GfVec3f const& positionHitWld,
                           GfVec3f const& omegaInWld,
                           HdEmbreeRenderColorSpace renderColorSpace =
                               HdEmbreeRenderColorSpace::LinearRec709);

    // callables to be used with std::visit
    LightSample operator()(HdEmbree_UnknownLight const& rect);
    LightSample operator()(HdEmbree_Rect const& rect);
    LightSample operator()(HdEmbree_Sphere const& sphere);
    LightSample operator()(HdEmbree_Disk const& disk);
    LightSample operator()(HdEmbree_Distant const& distant);
    LightSample operator()(HdEmbree_Cylinder const& cylinder);
    LightSample operator()(HdEmbree_Dome const& dome);

private:
    HdEmbreeLightSampler(HdEmbree_LightData const& lightData,
                         GfVec3f const& positionHitWld,
                         GfVec3f const& normalShdWldOut, float u1, float u2,
                         SamplingMode samplingMode,
                         HdEmbreeRenderColorSpace renderColorSpace)
        : _lightData(lightData), _positionHitWld(positionHitWld),
          _normalShdWldOut(normalShdWldOut), _u1(u1), _u2(u2),
          _samplingMode(samplingMode), _renderColorSpace(renderColorSpace)
    {
    }

    HdEmbree_LightData const& _lightData;
    GfVec3f const& _positionHitWld;
    GfVec3f const& _normalShdWldOut;
    float _u1;
    float _u2;
    SamplingMode _samplingMode;
    HdEmbreeRenderColorSpace _renderColorSpace;
};

PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_IMAGING_PLUGIN_HD_EMBREE_LIGHT_SAMPLERS_H
