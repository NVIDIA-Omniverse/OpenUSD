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

    /// Complete result of sampling or directionally evaluating one light.
    ///
    /// valid reports geometric and PDF usability; it does not independently
    /// validate radianceIn. When valid, omegaInWld and distanceWld describe the
    /// selected light point or direction, and pdfSolidAngleInverse is usable
    /// for the selected technique. Callers may read only valid when it is
    /// false.
    struct LightSample {
        /// Incident linear RGB radiance in the requested render color space.
        GfVec3f radianceIn;
        /// Normalized world-space direction from the shading point to light.
        GfVec3f omegaInWld;
        /// Positive world-space intersection distance for finite lights;
        /// max-float for distant and dome lights.
        float distanceWld;
        /// 1 / p(omega) in steradians for the selected solid-angle technique.
        /// Delta lights use 1 as a finite placeholder and are excluded from
        /// continuous-density MIS by delta.
        float pdfSolidAngleInverse;
        /// Geometric and PDF validity; radiance finiteness follows LightData's
        /// authored-input invariants rather than a result-side check.
        bool valid;
        /// True when the light is a directional delta distribution.
        bool delta = false;
    };

    /// Sample the selected light from a world-space shading point.
    ///
    /// posHitWld must be finite and u1/u2 are expected in [0,1). The concrete
    /// shape parameters and transforms must satisfy LightData's invariants.
    /// normalShdWldOut and samplingMode are used only for dome lights; dome
    /// reflection-hemisphere sampling requires a finite non-zero outward
    /// shading normal. Unsupported lights or unusable geometry return an
    /// invalid sample.
    static LightSample
    GetLightSample(LightData const& lightData,
                   GfVec3f const& posHitWld,
                   GfVec3f const& normalShdWldOut, float u1, float u2,
                   SamplingMode samplingMode = SamplingMode::FullSphere,
                   RenderColorSpace renderColorSpace =
                       RenderColorSpace::LinearRec709);

    /// Evaluate a dome along a non-zero finite world-space direction.
    ///
    /// The direction need not be normalized. lightData is expected to describe
    /// a dome; unusable direction, transform, or PDF state returns an invalid
    /// sample.
    static LightSample
    EvaluateDomeLightDirection(LightData const& lightData,
                               GfVec3f const& omegaInWld,
                               RenderColorSpace renderColorSpace =
                                   RenderColorSpace::LinearRec709);

    /// Evaluate a dome direction with the selected sampling-mode PDF.
    ///
    /// The world-space direction and outward shading normal must be finite and
    /// non-zero; either may be unnormalized. lightData is expected to describe
    /// a dome; unusable state returns an invalid sample.
    static LightSample EvaluateDomeLightDirection(
        LightData const& lightData, GfVec3f const& omegaInWld,
        GfVec3f const& normalShdWldOut, SamplingMode samplingMode,
        RenderColorSpace renderColorSpace =
            RenderColorSpace::LinearRec709);

    /// Evaluate a light along a fixed world-space ray from posHitWld.
    ///
    /// The origin must be finite and omegaInWld must be finite and non-zero;
    /// the direction need not be normalized. Returns an invalid sample when
    /// the ray misses, the light type is unsupported, or light state is
    /// unusable.
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
