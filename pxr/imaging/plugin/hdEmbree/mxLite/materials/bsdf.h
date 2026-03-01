//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_MXLITE_MATERIALS_BSDF_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_MXLITE_MATERIALS_BSDF_H

#include "pxr/pxr.h"
#include "pxr/imaging/plugin/hdEmbree/mxLite/types.h"

PXR_NAMESPACE_OPEN_SCOPE

/// Standalone BSDF evaluation functions.
///
/// Each function evaluates a single BSDF lobe for a given pair of
/// incident (wi) and outgoing (wo) directions. Directions point away
/// from the surface.
namespace MxLiteBsdf
{
    // ------------------------------------------------------------------
    // Evaluation (Phase 4)
    // ------------------------------------------------------------------

    /// Lambertian diffuse BRDF: f = albedo / pi.
    GfVec3f EvalLambertian(
        const GfVec3f& baseColor,
        const GfVec3f& N,
        const GfVec3f& wi,
        const GfVec3f& wo);

    /// GGX microfacet specular BRDF (Cook-Torrance).
    GfVec3f EvalGGXSpecular(
        float roughness,
        float ior,
        const GfVec3f& specularColor,
        const GfVec3f& N,
        const GfVec3f& wi,
        const GfVec3f& wo);

    /// GGX microfacet transmission BTDF.
    GfVec3f EvalGGXTransmission(
        float roughness,
        float ior,
        const GfVec3f& transmissionColor,
        const GfVec3f& N,
        const GfVec3f& wi,
        const GfVec3f& wo);

    /// Charlie sheen BRDF (Imageworks model).
    GfVec3f EvalSheen(
        const GfVec3f& sheenColor,
        float roughness,
        const GfVec3f& N,
        const GfVec3f& wi,
        const GfVec3f& wo);

    /// Clear-coat GGX specular lobe.
    GfVec3f EvalCoat(
        float coatWeight,
        float coatRoughness,
        float coatIor,
        const GfVec3f& N,
        const GfVec3f& wi,
        const GfVec3f& wo);

    /// Evaluate the full layered surface model from a closure.
    /// Combines all BSDF lobes with proper energy conservation.
    /// Returns the outgoing radiance contribution for one light sample.
    GfVec3f EvalSurface(
        const MxLiteSurfaceClosure& closure,
        const GfVec3f& N,
        const GfVec3f& wi,
        const GfVec3f& wo);

    // ------------------------------------------------------------------
    // Sampling & PDF (Phase 9)
    // ------------------------------------------------------------------

    struct BsdfSample {
        GfVec3f wi;
        GfVec3f f;
        float   pdf;
        bool    isSpecular;
    };

    /// Cosine-weighted hemisphere sampling for Lambertian diffuse.
    BsdfSample SampleLambertian(
        const GfVec3f& baseColor,
        const GfVec3f& N,
        const GfVec3f& wo,
        float u1, float u2);

    float PdfLambertian(
        const GfVec3f& N,
        const GfVec3f& wi);

    /// GGX visible-normal distribution function (VNDF) sampling for
    /// microfacet specular reflection.
    BsdfSample SampleGGXSpecular(
        float roughness,
        float ior,
        const GfVec3f& specularColor,
        const GfVec3f& N,
        const GfVec3f& wo,
        float u1, float u2);

    float PdfGGXSpecular(
        float roughness,
        const GfVec3f& N,
        const GfVec3f& wi,
        const GfVec3f& wo);

    /// Unified surface sampler: selects a lobe proportional to its
    /// approximate energy contribution, then importance-samples that lobe.
    /// The PDF accounts for all lobes (mixed PDF).
    BsdfSample SampleSurface(
        const MxLiteSurfaceClosure& closure,
        const GfVec3f& N,
        const GfVec3f& wo,
        float u1, float u2, float uLobe);

    float PdfSurface(
        const MxLiteSurfaceClosure& closure,
        const GfVec3f& N,
        const GfVec3f& wi,
        const GfVec3f& wo);

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

PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_IMAGING_PLUGIN_HD_EMBREE_MXLITE_MATERIALS_BSDF_H
