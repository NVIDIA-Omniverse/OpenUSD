//
// hdEmbree random-walk SSS helpers.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_SSS_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_SSS_H

#include <renderer/embreeCompat.h>

#include "pxr/base/gf/matrix4f.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/pxr.h"

#include <cstdint>

PXR_NAMESPACE_OPEN_SCOPE
namespace ty {

// =============================================================================
// New SSS API (Phase 1 of Chiang + Dwivedi rewrite).
// Replaces the reflect-loop-based SSS with a self-contained random walk.
// See agent-docs/designs/2026-04-16-design-sss-chiang-dwivedi.md.
// =============================================================================

struct SampleDomain;

struct SssInput {
    GfVec3f posEntryWld;
    // Material-resolved incident-side guide used by Dwivedi sampling. It is
    // not geometric boundary state.
    GfVec3f normalShdEntryGuideWldOut;
    GfVec3f dirEntryWld; // Into the medium from the entry point.
    GfVec3f albedo;            // Target per-channel diffuse reflectance.
    GfVec3f radius;            // Per-channel world-space scattering radius.
    float anisotropy;          // Clamped to [-0.99, 0.99].
    bool usePrecomputedCoefficients = false;
    GfVec3f precomputedAbsorption = GfVec3f(0.0f);
    GfVec3f precomputedScattering = GfVec3f(0.0f);
    unsigned int ownerInstanceId;
    unsigned int ownerGeomId;
    RTCScene ownerScene = nullptr;  // prototype scene containing ownerGeomId
    GfMatrix4f objectToWorldMatrix = GfMatrix4f(1.0f);
    GfMatrix4f worldToObjectMatrix = GfMatrix4f(1.0f);
};

struct SssOutput {
    bool success = false; // False when no exit was found.
    GfVec3f posExitWld = GfVec3f(0.0f);
    GfVec3f normalGeomExitWldExt = GfVec3f(0.0f);
    GfVec3f dirExitWld = GfVec3f(0.0f); // Interior to exterior.
    GfVec3f normalGeomExitObjExt = GfVec3f(0.0f);
    unsigned int exitInstanceId = RTC_INVALID_GEOMETRY_ID;
    unsigned int exitGeomId = RTC_INVALID_GEOMETRY_ID;
    unsigned int exitPrimId = RTC_INVALID_GEOMETRY_ID;
    float uExit = 0.0f;
    float vExit = 0.0f;
    GfVec3f throughputWeight = GfVec3f(0.0f);    // multiplier applied by caller
    uint32_t walkSteps = 0;                      // random-walk loop iterations
    uint32_t intersectionTests = 0;              // Embree rtcIntersect1 calls
};

SssOutput RandomWalkSSS(SssInput const& input,
                                        SampleDomain const& domain,
                                        RTCScene scene);

/// Chiang 2016 random-walk SSS coefficient remap.
///
/// Converts (albedo, radius, anisotropy) to (extinction, alpha) using the
/// polynomial fit from Chiang et al. 2016 (as implemented in Cycles).
/// The remap ensures that a random walk with these coefficients reproduces
/// the target diffuse reflectance specified by `albedo`.
///
/// If `rawAlphaOutput != nullptr`, also writes the pre-clamp alpha (needed
/// for min-alpha throughput correction in the random walk).
///
/// Alpha is clamped to [0.2, 0.999999] for numerical stability; the pre-clamp
/// value lives in `*rawAlphaOutput`.
///
/// Visible here for unit testing (Phase 2 Task 2.3-2.4).
/// Source: Blender Cycles `subsurface_random_walk_remap` (Apache 2.0).
void ChiangRemap(const GfVec3f& albedo, const GfVec3f& radius,
                         float anisotropy, GfVec3f* extinctionOutput,
                         GfVec3f* alphaOutput,
                         GfVec3f* rawAlphaOutput = nullptr);

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
float DiffusionLengthDwivedi(float alpha);

/// Evaluate the Dwivedi phase function at cosTheta given the precomputed
/// `phaseLog = log((L+1)/(L-1))`, where L is diffusion length.
/// (Eq. 9 from Meng et al 2016.)
float EvalPhaseDwivedi(float diffusionLength, float phaseLog,
                               float cosTheta);

/// Sample cosTheta from the Dwivedi distribution given phaseLog. Inverse CDF.
/// (Eq. 10 from Meng et al 2016.)
float SamplePhaseDwivedi(float diffusionLength, float phaseLog,
                                 float u1);

/// Probability of using the backward Dwivedi guide when an opposite interface
/// is known. `distanceFromEntryPlaneWld` is the clamped distance from the
/// entry tangent plane toward that interface. Visible for unit testing
/// ticket #403.
float BackwardDwivediFraction(float distanceOppositeWld,
                                      float distanceFromEntryPlaneWld,
                                      float diffusionLength);

} // namespace ty
PXR_NAMESPACE_CLOSE_SCOPE

#endif  // PXR_IMAGING_PLUGIN_HD_EMBREE_SSS_H
