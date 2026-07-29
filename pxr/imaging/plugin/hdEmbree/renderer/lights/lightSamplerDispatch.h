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
LightSampler::LightSample SampleRectLight(
    LightData const& light, RectLight const& rect,
    GfVec3f const& posWld, float u1, float u2,
    RenderColorSpace renderColorSpace);

/// Evaluates a positive, finite rect with its sampling PDF. Returns invalid
/// when the ray is parallel to the plane, behind `posWld`, or misses.
LightSampler::LightSample EvaluateRectLightDirection(
    LightData const& light, RectLight const& rect,
    GfVec3f const& posWld, GfVec3f const& dirWld,
    RenderColorSpace renderColorSpace);

/// Samples a positive, finite-radius sphere. Uniform-scale orthogonal outside
/// receivers use solid angle; others use area. Returns invalid if it cannot emit.
LightSampler::LightSample SampleSphereLight(
    LightData const& light, SphereLight const& sphere,
    GfVec3f const& posWld, float u1, float u2,
    RenderColorSpace renderColorSpace);

/// Evaluates a positive, finite-radius sphere with its available solid-angle
/// PDF override. Returns invalid for no emitting hit.
LightSampler::LightSample EvaluateSphereLightDirection(
    LightData const& light, SphereLight const& sphere,
    GfVec3f const& posWld, GfVec3f const& dirWld,
    RenderColorSpace renderColorSpace);

/// Samples a positive, finite-radius disk; shaping splits `u1` between
/// proposals. Returns invalid for zero area or an unusable sample.
LightSampler::LightSample SampleDiskLight(
    LightData const& light, DiskLight const& disk,
    GfVec3f const& posWld, float u1, float u2,
    RenderColorSpace renderColorSpace);

/// Evaluates a positive, finite-radius disk with its sampling PDF. Returns
/// invalid when the ray is parallel to the plane, behind `posWld`, or misses.
LightSampler::LightSample EvaluateDiskLightDirection(
    LightData const& light, DiskLight const& disk,
    GfVec3f const& posWld, GfVec3f const& dirWld,
    RenderColorSpace renderColorSpace);

/// Uniformly samples a positive, finite open cylinder. Shaped cylinders remain
/// area sampled. Returns invalid when the lateral surface cannot emit.
LightSampler::LightSample SampleCylinderLight(
    LightData const& light, CylinderLight const& cylinder,
    GfVec3f const& posWld, float u1, float u2,
    RenderColorSpace renderColorSpace);

/// Evaluates a positive, finite open cylinder. End rays miss; returns invalid
/// for a miss or a non-emitting lateral intersection.
LightSampler::LightSample EvaluateCylinderLightDirection(
    LightData const& light, CylinderLight const& cylinder,
    GfVec3f const& posWld, GfVec3f const& dirWld,
    RenderColorSpace renderColorSpace);

/// Samples a distant cone. Zero angle returns a delta with reciprocal PDF one;
/// nonzero angle is uniform. Returns invalid for a degenerate transformed +Z.
LightSampler::LightSample SampleDistantLight(
    LightData const& light, DistantLight const& distant,
    float u1, float u2, RenderColorSpace renderColorSpace);

/// Evaluates a distant light. Delta directions require cosine >= `1 - 1e-5`;
/// a mismatch or degenerate transformed +Z returns invalid.
LightSampler::LightSample EvaluateDistantLightDirection(
    LightData const& light, DistantLight const& distant,
    GfVec3f const& dirWld, RenderColorSpace renderColorSpace);

/// Samples an infinite dome, uniformly at `1 / 4Pi` without a distribution.
/// Hemisphere folding silently falls back for an invalid normalShdWldOut.
LightSampler::LightSample SampleDomeLight(
    LightData const& light, GfVec3f const& normalShdWldOut,
    float u1, float u2,
    LightSampler::SamplingMode samplingMode,
    RenderColorSpace renderColorSpace);

/// Evaluates an infinite dome at full-sphere PDF (`1 / 4Pi` without a
/// distribution). Returns invalid for a zero or non-finite dirWld.
LightSampler::LightSample EvaluateDomeLightDirection(
    LightData const& light, GfVec3f const& dirWld,
    RenderColorSpace renderColorSpace);

/// Evaluates an infinite dome with the selected PDF. Hemisphere mode rejects
/// an invalid normalShdWldOut or dirWld outside its hemisphere.
LightSampler::LightSample EvaluateDomeLightDirection(
    LightData const& light, GfVec3f const& dirWld,
    GfVec3f const& normalShdWldOut,
    LightSampler::SamplingMode samplingMode,
    RenderColorSpace renderColorSpace);


} // namespace ty
PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_IMAGING_PLUGIN_HD_EMBREE_LIGHT_SAMPLER_DISPATCH_H
