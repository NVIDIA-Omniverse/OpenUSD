//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
// Shared path-contribution cutoff, clamping, and MIS policy.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_INTEGRATOR_TRANSPORT_POLICY_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_INTEGRATOR_TRANSPORT_POLICY_H

#include "pxr/base/gf/vec3f.h"

#include <algorithm>

PXR_NAMESPACE_OPEN_SCOPE
namespace ty {

inline constexpr float MinLuminanceCutoff = 1e-9f;

inline bool
IsNearlyBlack(const GfVec3f& value, float threshold = 1.0e-4f)
{
    return value[0] <= threshold &&
           value[1] <= threshold &&
           value[2] <= threshold;
}

inline GfVec3f
ClampFireflyContribution(
    GfVec3f contribution,
    float threshold,
    GfVec3f const& luminanceCoefficients)
{
    if (threshold <= 0.0f) {
        return contribution;
    }

    const float luminance = GfDot(contribution, luminanceCoefficients);
    if (luminance > threshold) {
        contribution *= threshold / luminance;
    }
    return contribution;
}

inline float
GetMultiSampleMisLightPdf(float lightPdf, int sampleCount)
{
    if (lightPdf <= 0.0f) {
        return 0.0f;
    }
    return lightPdf * static_cast<float>(std::max(1, sampleCount));
}

} // namespace ty
PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_IMAGING_PLUGIN_HD_EMBREE_INTEGRATOR_TRANSPORT_POLICY_H
