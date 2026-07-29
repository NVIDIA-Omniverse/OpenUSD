//
// Copyright 2025 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
// Declares the per-light-type entry points used only by the light sampler
// dispatcher.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_LIGHT_SAMPLER_DISPATCH_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_LIGHT_SAMPLER_DISPATCH_H

#include "lightSampler.h"

#include "pxr/base/gf/vec3f.h"
#include "pxr/pxr.h"

PXR_NAMESPACE_OPEN_SCOPE

namespace ty {

/// Samples a positive, finite rect; shaping splits `u1` between proposals.
/// Returns invalid for the unlit side or an unusable sample.
HdEmbreeLightSampler::LightSample SampleRectLight(
    HdEmbree_LightData const& light, HdEmbree_Rect const& rect,
    GfVec3f const& position, float u1, float u2, HdEmbreeRenderColorSpace renderColorSpace);

/// Evaluates a positive, finite rect with its sampling PDF. Returns invalid
/// when the ray is parallel to the plane, behind `position`, or misses.
HdEmbreeLightSampler::LightSample EvaluateRectLightDirection(
    HdEmbree_LightData const& light, HdEmbree_Rect const& rect,
    GfVec3f const& position, GfVec3f const& direction, HdEmbreeRenderColorSpace renderColorSpace);

/// Samples a positive, finite-radius sphere. Uniform-scale orthogonal outside
/// receivers use solid angle; others use area. Returns invalid if it cannot emit.
HdEmbreeLightSampler::LightSample SampleSphereLight(
    HdEmbree_LightData const& light, HdEmbree_Sphere const& sphere,
    GfVec3f const& position, float u1, float u2, HdEmbreeRenderColorSpace renderColorSpace);

/// Evaluates a positive, finite-radius sphere with its available solid-angle
/// PDF override. Returns invalid for no emitting hit.
HdEmbreeLightSampler::LightSample EvaluateSphereLightDirection(
    HdEmbree_LightData const& light, HdEmbree_Sphere const& sphere,
    GfVec3f const& position, GfVec3f const& direction, HdEmbreeRenderColorSpace renderColorSpace);

/// Samples a positive, finite-radius disk; shaping splits `u1` between
/// proposals. Returns invalid for zero area or an unusable sample.
HdEmbreeLightSampler::LightSample SampleDiskLight(
    HdEmbree_LightData const& light, HdEmbree_Disk const& disk,
    GfVec3f const& position, float u1, float u2, HdEmbreeRenderColorSpace renderColorSpace);

/// Evaluates a positive, finite-radius disk with its sampling PDF. Returns
/// invalid when the ray is parallel to the plane, behind `position`, or misses.
HdEmbreeLightSampler::LightSample EvaluateDiskLightDirection(
    HdEmbree_LightData const& light, HdEmbree_Disk const& disk,
    GfVec3f const& position, GfVec3f const& direction, HdEmbreeRenderColorSpace renderColorSpace);

/// Uniformly samples a positive, finite open cylinder. Shaped cylinders remain
/// area sampled. Returns invalid when the lateral surface cannot emit.
HdEmbreeLightSampler::LightSample SampleCylinderLight(
    HdEmbree_LightData const& light, HdEmbree_Cylinder const& cylinder,
    GfVec3f const& position, float u1, float u2, HdEmbreeRenderColorSpace renderColorSpace);

/// Evaluates a positive, finite open cylinder. End rays miss; returns invalid
/// for a miss or a non-emitting lateral intersection.
HdEmbreeLightSampler::LightSample EvaluateCylinderLightDirection(
    HdEmbree_LightData const& light, HdEmbree_Cylinder const& cylinder,
    GfVec3f const& position, GfVec3f const& direction, HdEmbreeRenderColorSpace renderColorSpace);

/// Samples a distant cone. Zero angle returns a delta with reciprocal PDF one;
/// nonzero angle is uniform. Returns invalid for a degenerate transformed +Z.
HdEmbreeLightSampler::LightSample SampleDistantLight(
    HdEmbree_LightData const& light, HdEmbree_Distant const& distant,
    float u1, float u2, HdEmbreeRenderColorSpace renderColorSpace);

/// Evaluates a distant light. Delta directions require cosine >= `1 - 1e-5`;
/// a mismatch or degenerate transformed +Z returns invalid.
HdEmbreeLightSampler::LightSample EvaluateDistantLightDirection(
    HdEmbree_LightData const& light, HdEmbree_Distant const& distant,
    GfVec3f const& direction, HdEmbreeRenderColorSpace renderColorSpace);

/// Samples an infinite dome, uniformly at `1 / 4Pi` without a distribution.
/// Hemisphere folding silently falls back for an invalid normal.
HdEmbreeLightSampler::LightSample SampleDomeLight(
    HdEmbree_LightData const& light, GfVec3f const& normal, float u1, float u2,
    HdEmbreeLightSampler::SamplingMode samplingMode,
    HdEmbreeRenderColorSpace renderColorSpace);

/// Evaluates an infinite dome at full-sphere PDF (`1 / 4Pi` without a
/// distribution). Returns invalid for a zero or non-finite direction.
HdEmbreeLightSampler::LightSample EvaluateDomeLightDirection(
    HdEmbree_LightData const& light, GfVec3f const& direction,
    HdEmbreeRenderColorSpace renderColorSpace);

/// Evaluates an infinite dome with the selected PDF. Hemisphere mode rejects
/// an invalid normal or direction outside its hemisphere.
HdEmbreeLightSampler::LightSample EvaluateDomeLightDirection(
    HdEmbree_LightData const& light, GfVec3f const& direction,
    GfVec3f const& normal,
    HdEmbreeLightSampler::SamplingMode samplingMode,
    HdEmbreeRenderColorSpace renderColorSpace);

} // namespace ty

PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_IMAGING_PLUGIN_HD_EMBREE_LIGHT_SAMPLER_DISPATCH_H
