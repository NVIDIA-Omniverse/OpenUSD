//
// Copyright 2025 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
// Declares radiometry shared after light geometry is resolved, plus the small
// numeric helpers used by every light sampler.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_LIGHT_SAMPLER_COMMON_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_LIGHT_SAMPLER_COMMON_H

#include "lightSampler.h"

#include <renderer/rendererMath.h>

#include "pxr/base/gf/math.h"
#include "pxr/base/gf/matrix3f.h"
#include "pxr/base/gf/matrix4f.h"
#include "pxr/base/gf/vec2f.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/pxr.h"

#include <cmath>
#include <limits>

PXR_NAMESPACE_OPEN_SCOPE

namespace ty {

inline float Sqr(float x) { return x * x; }

/// Clamps a float into the half-open unit interval [0, 1).
inline float ClampUnitHalfOpen(float u)
{ return GfClamp(u, 0.0f, std::nextafter(1.0f, 0.0f)); }

inline float
WrapUnit(float u)
{
    u -= std::floor(u);
    if (u < 0.0f) {
        u += 1.0f;
    }
    return u;
}

struct ShapeSample {
    GfVec3f pWorld;
    GfVec3f nWorld;
    GfVec2f uv;
    float pdfAreaInverse;
};

inline ShapeSample
MakeAreaShapeSample(GfMatrix4f const& transform,
                    GfMatrix3f const& normalTransform,
                    GfVec3f const& positionLight,
                    GfVec3f const& normalLight,
                    GfVec2f const& uv,
                    float area)
{
    return ShapeSample{
        transform.Transform(positionLight),
        (normalLight * normalTransform).GetNormalized(),
        uv,
        area};
}

inline HdEmbreeLightSampler::LightSample InvalidLightSample()
{
    return HdEmbreeLightSampler::LightSample{
        GfVec3f(0.0f), GfVec3f(0.0f), 0.0f, 0.0f, false};
}

// TODO: This fixed split is a temporary baseline for finite lights. When
// MeshLight/arbitrary-emitter sampling is added, replace this with a shared
// emitter sampler that can build a receiver-dependent product proposal, e.g.
// p_area(x) * shaping(x -> shadingPoint), with consistent sample/evaluate PDFs.
inline constexpr float ShapingAwareFiniteDirectionalProposalWeight = 0.5f;
inline constexpr float ShapingAwareFiniteAreaProposalWeight = 1.0f -
    ShapingAwareFiniteDirectionalProposalWeight;

GfVec3f EvalLightBasic(HdEmbree_LightData const& light,
                       HdEmbreeRenderColorSpace renderColorSpace);

GfVec3f SampleLightTexture(HdEmbree_LightTexture const& texture, float s,
                           float t,
                           HdEmbreeRenderColorSpace renderColorSpace);

HdEmbreeLightSampler::LightSample EvalAreaLight(
    HdEmbree_LightData const& light, ShapeSample const& sample,
    GfVec3f const& position, HdEmbreeRenderColorSpace renderColorSpace);

void ApplyShapingAwareFinitePdf(HdEmbree_LightData const& light,
                                HdEmbreeLightSampler::LightSample* sample,
                                bool foldToFrontHemisphere);

} // namespace ty

PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_IMAGING_PLUGIN_HD_EMBREE_LIGHT_SAMPLER_COMMON_H
