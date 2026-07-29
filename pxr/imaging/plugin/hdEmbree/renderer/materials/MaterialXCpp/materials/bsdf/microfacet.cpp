//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "microfacet.h"
#include "energyCompensation.h"

namespace mxcpp {
namespace Bsdf {
namespace detail {

Vec3f
EvalMicrofacetReflectionAnisotropic(const Vec2f& roughness,
                                     const Vec3f& tangent, const Vec3f& fresnel,
                                     float weight, const Vec3f& normalShdWldOut,
                                     const Vec3f& omegaInWld,
                                     const Vec3f& omegaOutWld,
                                     bool compensateMissingEnergy )
{
    const Frame frame = Frame::FromNormalAndTangent(normalShdWldOut, tangent);
    const Vec3f omegaOutLocal = frame.ToLocal(omegaOutWld);
    const Vec3f omegaInLocal = frame.ToLocal(omegaInWld);
    const float cosThetaO = AbsCosTheta(omegaOutLocal);
    const float cosThetaI = AbsCosTheta(omegaInLocal);
    if (cosThetaI <= 0.0f || cosThetaO <= 0.0f || omegaInLocal[2] <= 0.0f ||
        omegaOutLocal[2] <= 0.0f) {
        return Vec3f(0.0f);
    }

    Vec3f wmLocal = omegaInLocal + omegaOutLocal;
    if (wmLocal.length() < kEpsilon) {
        return Vec3f(0.0f);
    }
    wmLocal.normalize();
    if (wmLocal[2] < 0.0f) {
        wmLocal = -wmLocal;
    }

    const Vec2f alpha = ClampAlpha(roughness);
    const float D = GGX_D_Anisotropic(alpha, wmLocal);
    const float G = GGX_G_Anisotropic(alpha, omegaOutLocal, omegaInLocal);
    const Vec3f compensatedFresnel = compensateMissingEnergy
        ? CompMul(
              fresnel,
              TurquinMicrofacetMsScale(
                  AverageAlphaForEnergy(alpha), cosThetaO, fresnel)) * weight
        : fresnel * weight;
    return SafeVec(CompMul(
        compensatedFresnel,
        Vec3f(D * G / std::max(4.0f * cosThetaI * cosThetaO, kEpsilon))));
}

Vec3f
EvalMicrofacetReflectionIsotropic(float alpha, const Vec3f& fresnel,
                                   float weight, const Vec3f& normalShdWldOut,
                                   const Vec3f& omegaInWld,
                                   const Vec3f& omegaOutWld,
                                   bool compensateMissingEnergy )
{
    const float clampedAlpha =
        std::clamp(alpha, kMinMicrofacetAlpha, 1.0f);
    const float NdotL = std::max(Dot(normalShdWldOut, omegaInWld), 0.0f);
    const float NdotV = std::max(Dot(normalShdWldOut, omegaOutWld), kEpsilon);
    if (NdotL <= 0.0f || NdotV <= 0.0f) {
        return Vec3f(0.0f);
    }

    Vec3f H = omegaInWld + omegaOutWld;
    if (H.length() < kEpsilon) {
        return Vec3f(0.0f);
    }
    H.normalize();
    const float NdotH = std::max(Dot(normalShdWldOut, H), 0.0f);
    const float D = GGX_D(clampedAlpha, NdotH);
    const float G = GGX_G(clampedAlpha, NdotV, NdotL);
    const Vec3f compensatedFresnel = compensateMissingEnergy
        ? CompMul(
              fresnel,
              TurquinMicrofacetMsScale(clampedAlpha, NdotV, fresnel)) * weight
        : fresnel * weight;
    return SafeVec(CompMul(
        compensatedFresnel,
        Vec3f(D * G / std::max(4.0f * NdotL * NdotV, kEpsilon))));
}

float
PdfGGXSpecularAnisotropic(const Vec2f& roughness, const Vec3f& tangent,
                           const Vec3f& normalShdWldOut,
                           const Vec3f& omegaInWld, const Vec3f& omegaOutWld)
{
    const Frame frame = Frame::FromNormalAndTangent(normalShdWldOut, tangent);
    const Vec3f omegaOutLocal = frame.ToLocal(omegaOutWld);
    const Vec3f omegaInLocal = frame.ToLocal(omegaInWld);
    if (omegaOutLocal[2] <= 0.0f || omegaInLocal[2] <= 0.0f) {
        return 0.0f;
    }

    Vec3f wmLocal = omegaOutLocal + omegaInLocal;
    if (wmLocal.length() < kEpsilon) {
        return 0.0f;
    }
    wmLocal.normalize();
    if (wmLocal[2] <= 0.0f) {
        return 0.0f;
    }

    const Vec2f alpha = ClampAlpha(roughness);
    const float pdfMicrofacetNormalSolidAngle =
        PdfGGX_VNDF_Anisotropic(omegaOutLocal, wmLocal, alpha);
    const float VdotH =
        std::max(std::abs(Dot(omegaOutLocal, wmLocal)), kEpsilon);
    return pdfMicrofacetNormalSolidAngle / (4.0f * VdotH);
}

Bsdf::BsdfSample
SampleGGXSpecularAnisotropic(const Vec2f& roughness, const Vec3f& tangent,
                              const Vec3f& normalShdWldOut,
                              const Vec3f& omegaOutWld, float u1, float u2)
{
    const Frame frame = Frame::FromNormalAndTangent(normalShdWldOut, tangent);
    const Vec3f omegaOutLocal = frame.ToLocal(omegaOutWld);
    if (omegaOutLocal[2] <= 0.0f) {
        return Bsdf::BsdfSample{Vec3f(0.0f), Vec3f(0.0f), 0.0f, false};
    }

    const Vec2f alpha = ClampAlpha(roughness);
    const Vec3f wmLocal =
        SampleGGX_VNDF_Anisotropic(omegaOutLocal, alpha, u1, u2);
    const Vec3f omegaInLocal =
        2.0f * Dot(omegaOutLocal, wmLocal) * wmLocal - omegaOutLocal;
    if (omegaInLocal[2] <= 0.0f) {
        return Bsdf::BsdfSample{Vec3f(0.0f), Vec3f(0.0f), 0.0f, false};
    }

    const Vec3f omegaInWld = frame.ToWorld(omegaInLocal);
    const float pdfMicrofacetNormalSolidAngle =
        PdfGGX_VNDF_Anisotropic(omegaOutLocal, wmLocal, alpha);
    const float VdotH =
        std::max(std::abs(Dot(omegaOutLocal, wmLocal)), kEpsilon);
    return Bsdf::BsdfSample{omegaInWld, Vec3f(0.0f),
                            pdfMicrofacetNormalSolidAngle / (4.0f * VdotH),
                            false};
}

}  // namespace detail
}  // namespace Bsdf
}  // namespace mxcpp
