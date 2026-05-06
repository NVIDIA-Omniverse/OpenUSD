//
// hdEmbree random-walk SSS helpers.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_SSS_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_SSS_H

#include "pxr/pxr.h"

#include "pxr/base/gf/matrix4f.h"
#include "pxr/base/gf/vec3f.h"

#include <embree4/rtcore.h>
#include <cstdint>

PXR_NAMESPACE_OPEN_SCOPE

// =============================================================================
// New SSS API (Phase 1 of Chiang + Dwivedi rewrite).
// Replaces the reflect-loop-based SSS with a self-contained random walk.
// See agent-docs/designs/2026-04-16-design-sss-chiang-dwivedi.md.
// =============================================================================

struct HdEmbreeSampleDomain;

struct HdEmbreeSssInput {
    GfVec3f entryPos;
    GfVec3f entryGeomNormal;     // outward
    GfVec3f entryDir;            // into medium (from Bsdf::SampleSubsurfaceEntry)
    GfVec3f albedo;              // = subsurface_color (target reflectance)
    GfVec3f radius;              // = subsurface_radius * radius_scale (world units, per channel)
    float anisotropy;            // clamp to [-0.99, 0.99]
    float ior;                   // clamp to >= 1.0
    unsigned int ownerInstanceId;
    unsigned int ownerGeomId;
    RTCScene ownerScene = nullptr;  // prototype scene containing ownerGeomId
    GfMatrix4f objectToWorldMatrix = GfMatrix4f(1.0f);
    GfMatrix4f worldToObjectMatrix = GfMatrix4f(1.0f);
};

struct HdEmbreeSssOutput {
    bool success = false;                        // false: no exit found
    GfVec3f exitPos = GfVec3f(0.0f);
    GfVec3f exitGeomNormal = GfVec3f(0.0f);      // outward
    GfVec3f exitDir = GfVec3f(0.0f);             // internal -> outside
    GfVec3f exitObjectGeomNormal = GfVec3f(0.0f);
    unsigned int exitInstanceId = RTC_INVALID_GEOMETRY_ID;
    unsigned int exitGeomId = RTC_INVALID_GEOMETRY_ID;
    unsigned int exitPrimId = RTC_INVALID_GEOMETRY_ID;
    float exitU = 0.0f;
    float exitV = 0.0f;
    GfVec3f throughputWeight = GfVec3f(0.0f);    // multiplier applied by caller
    uint32_t walkSteps = 0;                      // random-walk loop iterations
    uint32_t intersectionTests = 0;              // Embree rtcIntersect1 calls
};

HdEmbreeSssOutput
HdEmbreeRandomWalkSSS(
    HdEmbreeSssInput const& in,
    HdEmbreeSampleDomain const& domain,
    RTCScene scene);

/// Chiang 2016 random-walk SSS coefficient remap.
///
/// Converts (albedo, radius, anisotropy) to (sigma_t, alpha) using the
/// polynomial fit from Chiang et al. 2016 (as implemented in Cycles).
/// The remap ensures that a random walk with these coefficients reproduces
/// the target diffuse reflectance specified by `albedo`.
///
/// If `rawAlphaOut != nullptr`, also writes the pre-clamp alpha (needed
/// for min-alpha throughput correction in the random walk).
///
/// Alpha is clamped to [0.2, 0.999999] for numerical stability; the pre-clamp
/// value lives in `*rawAlphaOut`.
///
/// Visible here for unit testing (Phase 2 Task 2.3-2.4).
/// Source: Blender Cycles `subsurface_random_walk_remap` (Apache 2.0).
void
HdEmbreeChiangRemap(
    const GfVec3f& albedo,
    const GfVec3f& radius,
    float anisotropy,
    GfVec3f* sigma_t,
    GfVec3f* alpha,
    GfVec3f* rawAlphaOut = nullptr);

/// Dwivedi sampling helpers (Phase 3; visible for testing).
///
/// Source: Cycles src/kernel/integrator/subsurface_random_walk.h (Apache 2.0).
/// References:
/// - [Křivánek, d'Eon 2014] "A Zero-variance-based Sampling Scheme for
///   Monte Carlo Subsurface Scattering"
/// - [Meng, Hanika, Dachsbacher 2016] "Improving the Dwivedi Sampling Scheme"
/// - [d'Eon, Křivánek 2020] "Zero-Variance Theory for Efficient Subsurface
///   Scattering"

/// Compute the diffusion length v = 1/sqrt(1 - alpha^k) where the exponent k
/// is a polynomial approximation of the zero-variance form. (Eq. 67 from
/// d'Eon-Křivánek 2020, via Cycles.)
float HdEmbreeDiffusionLengthDwivedi(float alpha);

/// Evaluate the Dwivedi phase function at cos_theta given the precomputed
/// `phase_log = log((L+1)/(L-1))`. (Eq. 9 from Meng et al 2016.)
float HdEmbreeEvalPhaseDwivedi(float L, float phase_log, float cos_theta);

/// Sample cos_theta from the Dwivedi distribution given phase_log. Inverse CDF.
/// (Eq. 10 from Meng et al 2016.)
float HdEmbreeSamplePhaseDwivedi(float L, float phase_log, float u);

/// Probability of using the backward Dwivedi guide when an opposite interface
/// is known. `x` is the clamped distance from the entry tangent plane toward
/// that interface. Visible for unit testing ticket #403.
float HdEmbreeBackwardDwivediFraction(
    float oppositeDistance,
    float x,
    float diffusionLength);

PXR_NAMESPACE_CLOSE_SCOPE

#endif  // PXR_IMAGING_PLUGIN_HD_EMBREE_SSS_H
