//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef MXCPP_MATERIALS_BSDF_H
#define MXCPP_MATERIALS_BSDF_H

#include "../surfaceClosure.h"

namespace mxcpp {

/// Standalone BSDF evaluation functions.
///
/// Each function evaluates a single BSDF lobe for a given pair of
/// incident (wi) and outgoing (wo) directions. Directions point away
/// from the surface.
namespace Bsdf
{
    /// Enables Turquin-style multiple-scattering compensation for GGX
    /// reflection lobes and their layered throughput estimates.
    void SetGgxMicrofacetMultipleScatteringEnabled(bool enabled);
    bool IsGgxMicrofacetMultipleScatteringEnabled();

    enum class DielectricLayerThroughputMode
    {
        Bsdl,
        MaterialXGlsl
    };

    /// Selects the rough dielectric top-layer throughput estimate.
    void SetDielectricLayerThroughputMode(
        DielectricLayerThroughputMode mode);
    DielectricLayerThroughputMode GetDielectricLayerThroughputMode();

    // ------------------------------------------------------------------
    // Evaluation (Phase 4)
    // ------------------------------------------------------------------

    /// Lambertian diffuse BRDF: f = albedo / pi.
    Vec3f EvalLambertian(
        const Vec3f& baseColor,
        const Vec3f& N,
        const Vec3f& wi,
        const Vec3f& wo);

    /// GGX microfacet specular BRDF (Cook-Torrance).
    Vec3f EvalGGXSpecular(
        float roughness,
        float ior,
        const Vec3f& specularColor,
        const Vec3f& N,
        const Vec3f& wi,
        const Vec3f& wo);

    /// GGX microfacet transmission BTDF.
    Vec3f EvalGGXTransmission(
        float roughness,
        float ior,
        const Vec3f& transmissionColor,
        const Vec3f& N,
        const Vec3f& wi,
        const Vec3f& wo);

    /// Charlie sheen BRDF (Imageworks model).
    Vec3f EvalSheen(
        const Vec3f& sheenColor,
        float roughness,
        const Vec3f& N,
        const Vec3f& wi,
        const Vec3f& wo);

    /// Clear-coat GGX specular lobe.
    Vec3f EvalCoat(
        float coatWeight,
        float coatRoughness,
        float coatIor,
        const Vec3f& N,
        const Vec3f& wi,
        const Vec3f& wo);

    /// Evaluate the full layered surface model from a closure.
    /// Combines all BSDF lobes with proper energy conservation.
    /// Returns the outgoing radiance contribution for one light sample.
    Vec3f EvalSurface(
        const SurfaceClosure& closure,
        const Vec3f& N,
        const Vec3f& wi,
        const Vec3f& wo,
        float heroWavelengthNm = 0.0f);

    // ------------------------------------------------------------------
    // Sampling & PDF (Phase 9)
    // ------------------------------------------------------------------

    struct BsdfSample {
        Vec3f wi;
        Vec3f f;
        float   pdf;
        bool    isSpecular;
        bool    isSubsurface = false;
        bool    hasSubsurfaceEntryDirection = false;
        float   eta = 1.0f;     // IOR ratio (incident/transmitted), for refraction differential propagation
    };

    /// Cosine-weighted hemisphere sampling for Lambertian diffuse.
    BsdfSample SampleLambertian(
        const Vec3f& baseColor,
        const Vec3f& N,
        const Vec3f& wo,
        float u1, float u2);

    float PdfLambertian(
        const Vec3f& N,
        const Vec3f& wi);

    /// GGX visible-normal distribution function (VNDF) sampling for
    /// microfacet specular reflection.
    BsdfSample SampleGGXSpecular(
        float roughness,
        float ior,
        const Vec3f& specularColor,
        const Vec3f& N,
        const Vec3f& wo,
        float u1, float u2);

    float PdfGGXSpecular(
        float roughness,
        const Vec3f& N,
        const Vec3f& wi,
        const Vec3f& wo);

    /// Directional energy helpers for the isotropic GGX reflection lobe.
    /// alphaRoughness is the GGX alpha parameter, not perceptual roughness.
    float GgxDirectionalMissingEnergy(
        float cosTheta,
        float alphaRoughness);

    float GgxDirectionalSingleScatterEnergy(
        float cosTheta,
        float alphaRoughness);

    /// GGX VNDF-based transmission sampling.
    BsdfSample SampleGGXTransmission(
        float roughness,
        float ior,
        const Vec3f& transmissionColor,
        const Vec3f& N,
        const Vec3f& wo,
        float u1, float u2);

    float PdfGGXTransmission(
        float roughness,
        float ior,
        const Vec3f& N,
        const Vec3f& wi,
        const Vec3f& wo);

    /// Unified surface sampler: selects a lobe proportional to its
    /// approximate energy contribution, then importance-samples that lobe.
    /// The PDF accounts for all lobes (mixed PDF).
    BsdfSample SampleSurface(
        const SurfaceClosure& closure,
        const Vec3f& N,
        const Vec3f& wo,
        float u1, float u2, float uLobe,
        float heroWavelengthNm = 0.0f);

    float PdfSurface(
        const SurfaceClosure& closure,
        const Vec3f& N,
        const Vec3f& wi,
        const Vec3f& wo,
        float heroWavelengthNm = 0.0f);

    /// Sample a direction entering a subsurface medium using the surface's
    /// specular dielectric parameters (roughness + IOR).
    ///
    /// - Smooth surface (roughness < threshold): deterministic Snell refraction.
    /// - Rough surface: GGX VNDF samples microfacet normal H, then Snell about H.
    /// - IOR is clamped to >= 1.0 so there is no TIR at entry (matches Cycles).
    /// - Returns false only on degenerate input (e.g. Dot(N, wo) <= 0).
    ///
    /// Source: influenced by Cycles `subsurface_entry_bounce` in
    /// intern/cycles/kernel/integrator/subsurface.h (Apache 2.0).
    bool SampleSubsurfaceEntry(
        const SurfaceClosure& closure,
        const Vec3f& N,
        const Vec3f& wo,
        float u1, float u2,
        Vec3f& wi_into_medium);

    // ------------------------------------------------------------------
    // MIS utilities
    // ------------------------------------------------------------------

    /// Power heuristic with exponent 2 (following Veach).
    inline float PowerHeuristic(float pdfA, float pdfB) {
        float a2 = pdfA * pdfA;
        float b2 = pdfB * pdfB;
        return a2 / (a2 + b2 + 1e-10f);
    }
}

}  // namespace mxcpp

#endif  // MXCPP_MATERIALS_BSDF_H
