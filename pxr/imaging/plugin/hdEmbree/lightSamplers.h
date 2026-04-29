//
// Copyright 2025 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_LIGHT_SAMPLERS_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_LIGHT_SAMPLERS_H

#include "pxr/pxr.h"
#include "pxr/base/gf/vec3f.h"

#include "pxr/imaging/plugin/hdEmbree/light.h"

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
        GfVec3f Li;
        GfVec3f wI;
        float dist;
        float invPdfW;
        bool valid;
    };

    static LightSample GetLightSample(
            HdEmbree_LightData const& lightData,
            GfVec3f const& hitPosition,
            GfVec3f const& normal,
            float u1,
            float u2,
            SamplingMode samplingMode = SamplingMode::FullSphere);

    /// Evaluates a dome light along a fixed direction and returns the
    /// corresponding radiance and directional PDF.
    static LightSample EvaluateDomeLightDirection(
            HdEmbree_LightData const& lightData,
            GfVec3f const& direction);

    /// Evaluates a dome light along a fixed direction with the PDF used by
    /// the selected dome-light sampling mode.
    static LightSample EvaluateDomeLightDirection(
            HdEmbree_LightData const& lightData,
            GfVec3f const& direction,
            GfVec3f const& normal,
            SamplingMode samplingMode);

    /// Evaluates a light along a fixed direction from a point and returns the
    /// corresponding radiance, distance, and directional PDF when the ray
    /// intersects the light shape.
    static LightSample EvaluateLightDirection(
            HdEmbree_LightData const& lightData,
            GfVec3f const& hitPosition,
            GfVec3f const& direction);

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
                         GfVec3f const& hitPosition,
                         GfVec3f const& normal,
                         float u1,
                         float u2,
                         SamplingMode samplingMode) :
        _lightData(lightData),
        _hitPosition(hitPosition),
        _normal(normal),
        _u1(u1),
        _u2(u2),
        _samplingMode(samplingMode)
    {}

    HdEmbree_LightData const& _lightData;
    GfVec3f const& _hitPosition;
    GfVec3f const& _normal;
    float _u1;
    float _u2;
    SamplingMode _samplingMode;
};

PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_IMAGING_PLUGIN_HD_EMBREE_LIGHT_SAMPLERS_H
