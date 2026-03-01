//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/imaging/plugin/hdEmbree/mxLite/materials/bsdf.h"

#include "pxr/base/gf/math.h"
#include "pxr/base/gf/vec2f.h"

#include <cmath>
#include <algorithm>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

constexpr float _kPi      = 3.14159265358979323846f;
constexpr float _kInvPi   = 0.31830988618379067154f;
constexpr float _kEpsilon = 1e-7f;

// -----------------------------------------------------------------------
// Roughness helpers
// -----------------------------------------------------------------------

inline float
_ClampRoughness(float r)
{
    return std::clamp(r, 0.001f, 1.0f);
}

inline float
_RoughnessToAlpha(float roughness)
{
    float r = _ClampRoughness(roughness);
    return r * r;
}

// -----------------------------------------------------------------------
// Fresnel
// -----------------------------------------------------------------------

inline GfVec3f
_SchlickFresnel(const GfVec3f& F0, float cosTheta)
{
    float t = 1.0f - cosTheta;
    float t2 = t * t;
    float t5 = t2 * t2 * t;
    return F0 + (GfVec3f(1.0f) - F0) * t5;
}

inline float
_SchlickFresnelScalar(float ior, float cosTheta)
{
    float f0 = (ior - 1.0f) / (ior + 1.0f);
    f0 *= f0;
    float t = 1.0f - cosTheta;
    float t2 = t * t;
    float t5 = t2 * t2 * t;
    return f0 + (1.0f - f0) * t5;
}

// -----------------------------------------------------------------------
// GGX helpers
// -----------------------------------------------------------------------

inline float
_GGX_D(float alpha, float NdotH)
{
    float a2 = alpha * alpha;
    float denom = NdotH * NdotH * (a2 - 1.0f) + 1.0f;
    return a2 / (_kPi * denom * denom + _kEpsilon);
}

inline float
_GGX_V(float alpha, float NdotV, float NdotL)
{
    float a2 = alpha * alpha;
    float ggxV = NdotL * std::sqrt(NdotV * NdotV * (1.0f - a2) + a2);
    float ggxL = NdotV * std::sqrt(NdotL * NdotL * (1.0f - a2) + a2);
    return 0.5f / (ggxV + ggxL + _kEpsilon);
}

// Smith G1 masking function for GGX.
inline float
_SmithG1(float alpha, float cosTheta)
{
    float a2 = alpha * alpha;
    float cos2 = cosTheta * cosTheta;
    return 2.0f * cosTheta /
        (cosTheta + std::sqrt(a2 + (1.0f - a2) * cos2) + _kEpsilon);
}

inline GfVec3f
_ComputeF0(const GfVec3f& baseColor, float metallic,
           float specular, float ior)
{
    float dielectricF0 = ((ior - 1.0f) / (ior + 1.0f));
    dielectricF0 *= dielectricF0;
    dielectricF0 *= specular;
    GfVec3f F0 = GfVec3f(dielectricF0);
    return F0 * (1.0f - metallic) + baseColor * metallic;
}

// -----------------------------------------------------------------------
// Sheen helpers
// -----------------------------------------------------------------------

inline float
_Charlie_D(float alpha, float NdotH)
{
    float sinTheta2 = 1.0f - NdotH * NdotH;
    float sinTheta = std::sqrt(std::max(0.0f, sinTheta2));
    float invAlpha = 1.0f / std::max(alpha, _kEpsilon);
    return (2.0f + invAlpha) * std::pow(sinTheta, invAlpha) * _kInvPi * 0.5f;
}

inline float
_Ashikhmin_V(float NdotV, float NdotL)
{
    return 1.0f / (4.0f * (NdotL + NdotV - NdotL * NdotV) + _kEpsilon);
}

// -----------------------------------------------------------------------
// Tangent frame (world <-> shading-local)
// -----------------------------------------------------------------------

struct _Frame {
    GfVec3f T, B, N;

    GfVec3f ToLocal(const GfVec3f& v) const {
        return GfVec3f(GfDot(v, T), GfDot(v, B), GfDot(v, N));
    }

    GfVec3f ToWorld(const GfVec3f& v) const {
        return T * v[0] + B * v[1] + N * v[2];
    }

    static _Frame FromNormal(const GfVec3f& n) {
        _Frame f;
        f.N = n;
        GfBuildOrthonormalFrame(n, &f.T, &f.B);
        return f;
    }
};

// -----------------------------------------------------------------------
// Hemisphere sampling
// -----------------------------------------------------------------------

// Cosine-weighted hemisphere sample in local frame (N = +Z).
inline GfVec3f
_SampleCosineHemisphere(float u1, float u2)
{
    float cosTheta = std::sqrt(u1);
    float sinTheta = std::sqrt(1.0f - u1);
    float phi = 2.0f * _kPi * u2;
    return GfVec3f(sinTheta * std::cos(phi),
                   sinTheta * std::sin(phi),
                   cosTheta);
}

inline float
_CosineHemispherePdf(float cosTheta)
{
    return std::max(cosTheta, 0.0f) * _kInvPi;
}

// -----------------------------------------------------------------------
// GGX VNDF sampling (following pbrt-v4 / Heitz 2018)
// -----------------------------------------------------------------------

// Sample a microfacet normal visible from `woLocal` (in local frame).
GfVec3f
_SampleGGX_VNDF(const GfVec3f& woLocal, float alpha, float u1, float u2)
{
    // Transform wo to hemispherical configuration
    GfVec3f wh = GfVec3f(alpha * woLocal[0],
                          alpha * woLocal[1],
                          woLocal[2]);
    float whLen = wh.GetLength();
    if (whLen < _kEpsilon) return GfVec3f(0.0f, 0.0f, 1.0f);
    wh /= whLen;
    if (wh[2] < 0.0f) wh = -wh;

    // Build ONB from wh
    GfVec3f T1 = (wh[2] < 0.99999f)
        ? GfCross(GfVec3f(0.0f, 0.0f, 1.0f), wh).GetNormalized()
        : GfVec3f(1.0f, 0.0f, 0.0f);
    GfVec3f T2 = GfCross(wh, T1);

    // Sample uniform disk (polar)
    float r = std::sqrt(u1);
    float phi = 2.0f * _kPi * u2;
    float t1 = r * std::cos(phi);
    float t2 = r * std::sin(phi);

    // Warp hemispherical projection for visible normal sampling
    float h = std::sqrt(std::max(0.0f, 1.0f - t1 * t1));
    float blend = (1.0f + wh[2]) * 0.5f;
    t2 = (1.0f - blend) * h + blend * t2;

    // Reproject to hemisphere
    float pz = std::sqrt(std::max(0.0f, 1.0f - t1 * t1 - t2 * t2));
    GfVec3f nh = T1 * t1 + T2 * t2 + wh * pz;

    // Transform back to ellipsoid configuration
    GfVec3f wm(alpha * nh[0], alpha * nh[1], std::max(1e-6f, nh[2]));
    return wm.GetNormalized();
}

// PDF for VNDF-sampled microfacet normal wm given view direction wo.
// Returns the PDF in reflected direction measure (divided by Jacobian).
float
_PdfGGX_VNDF(const GfVec3f& woLocal, const GfVec3f& wmLocal, float alpha)
{
    float cosTheta_o = std::max(woLocal[2], _kEpsilon);
    float G1 = _SmithG1(alpha, cosTheta_o);
    float NdotH = std::max(wmLocal[2], 0.0f);
    float VdotH = std::max(GfDot(woLocal, wmLocal), _kEpsilon);

    float D = _GGX_D(alpha, NdotH);

    // PDF of the microfacet normal: G1(wo) * D(wm) * max(0, dot(wo, wm)) / cos(theta_o)
    float pdfWm = G1 * D * VdotH / cosTheta_o;

    // Jacobian of reflection: 1 / (4 * |dot(wo, wm)|)
    return pdfWm / (4.0f * VdotH);
}

// -----------------------------------------------------------------------
// Luminance helper for lobe selection
// -----------------------------------------------------------------------

inline float
_Luminance(const GfVec3f& c)
{
    return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
}

// Guard against NaN/Inf/negative.
inline GfVec3f
_SafeVec(const GfVec3f& v)
{
    GfVec3f r = v;
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(r[i]) || r[i] < 0.0f) r[i] = 0.0f;
    }
    return r;
}

} // anonymous namespace

// ===========================================================================
// BSDF Evaluation (Phase 4)
// ===========================================================================

GfVec3f
MxLiteBsdf::EvalLambertian(
    const GfVec3f& baseColor,
    const GfVec3f& N,
    const GfVec3f& /*wi*/,
    const GfVec3f& /*wo*/)
{
    return baseColor * _kInvPi;
}

GfVec3f
MxLiteBsdf::EvalGGXSpecular(
    float roughness,
    float ior,
    const GfVec3f& specularColor,
    const GfVec3f& N,
    const GfVec3f& wi,
    const GfVec3f& wo)
{
    float alpha = _RoughnessToAlpha(roughness);

    float NdotL = std::max(GfDot(N, wi), 0.0f);
    float NdotV = std::max(GfDot(N, wo), _kEpsilon);
    GfVec3f H = (wi + wo).GetNormalized();
    float NdotH = std::max(GfDot(N, H), 0.0f);
    float VdotH = std::max(GfDot(wo, H), 0.0f);

    float D = _GGX_D(alpha, NdotH);
    float V = _GGX_V(alpha, NdotV, NdotL);
    GfVec3f F = _SchlickFresnel(specularColor, VdotH);

    return GfCompMult(F, GfVec3f(D * V));
}

GfVec3f
MxLiteBsdf::EvalGGXTransmission(
    float roughness,
    float ior,
    const GfVec3f& transmissionColor,
    const GfVec3f& N,
    const GfVec3f& wi,
    const GfVec3f& wo)
{
    float alpha = _RoughnessToAlpha(roughness);

    float NdotL = std::fabs(GfDot(N, wi));
    float NdotV = std::fabs(GfDot(N, wo));

    float fresnel = _SchlickFresnelScalar(ior, NdotV);
    float transmission = (1.0f - fresnel);

    GfVec3f H = (wi + wo).GetNormalized();
    float NdotH = std::fabs(GfDot(N, H));
    float D = _GGX_D(alpha, NdotH);
    float V = _GGX_V(alpha, std::max(NdotV, _kEpsilon),
                      std::max(NdotL, _kEpsilon));

    return transmissionColor * (transmission * D * V);
}

GfVec3f
MxLiteBsdf::EvalSheen(
    const GfVec3f& sheenColor,
    float roughness,
    const GfVec3f& N,
    const GfVec3f& wi,
    const GfVec3f& wo)
{
    float alpha = _ClampRoughness(roughness);

    float NdotL = std::max(GfDot(N, wi), 0.0f);
    float NdotV = std::max(GfDot(N, wo), _kEpsilon);
    GfVec3f H = (wi + wo).GetNormalized();
    float NdotH = std::max(GfDot(N, H), 0.0f);

    float D = _Charlie_D(alpha, NdotH);
    float V = _Ashikhmin_V(NdotV, NdotL);

    return sheenColor * (D * V);
}

GfVec3f
MxLiteBsdf::EvalCoat(
    float coatWeight,
    float coatRoughness,
    float coatIor,
    const GfVec3f& N,
    const GfVec3f& wi,
    const GfVec3f& wo)
{
    if (coatWeight <= 0.0f) return GfVec3f(0.0f);

    float alpha = _RoughnessToAlpha(coatRoughness);

    float NdotL = std::max(GfDot(N, wi), 0.0f);
    float NdotV = std::max(GfDot(N, wo), _kEpsilon);
    GfVec3f H = (wi + wo).GetNormalized();
    float NdotH = std::max(GfDot(N, H), 0.0f);
    float VdotH = std::max(GfDot(wo, H), 0.0f);

    float D = _GGX_D(alpha, NdotH);
    float V = _GGX_V(alpha, NdotV, NdotL);
    float F = _SchlickFresnelScalar(coatIor, VdotH);

    return GfVec3f(coatWeight * D * V * F);
}

GfVec3f
MxLiteBsdf::EvalSurface(
    const MxLiteSurfaceClosure& c,
    const GfVec3f& N,
    const GfVec3f& wi,
    const GfVec3f& wo)
{
    float NdotL = GfDot(N, wi);

    // Reflection lobes require wi in the same hemisphere as N.
    GfVec3f reflected(0.0f);
    if (NdotL > 0.0f) {
        GfVec3f F0 = _ComputeF0(
            c.baseColor, c.metallic, c.specular, c.specularIor);
        bool hasSpecularLobe = (_Luminance(F0) > _kEpsilon);

        GfVec3f H = (wi + wo).GetNormalized();
        float VdotH = std::max(GfDot(wo, H), 0.0f);

        GfVec3f diffuse(0.0f);
        GfVec3f specular(0.0f);

        if (hasSpecularLobe) {
            GfVec3f fresnel = _SchlickFresnel(F0, VdotH);
            GfVec3f kD = GfCompMult(GfVec3f(1.0f) - fresnel,
                                     GfVec3f(1.0f - c.metallic));
            kD = kD * (1.0f - c.transmission);
            diffuse = GfCompMult(kD,
                EvalLambertian(c.baseColor, N, wi, wo));

            specular = EvalGGXSpecular(
                c.roughness, c.specularIor,
                GfCompMult(c.specularColor, F0),
                N, wi, wo);
        } else {
            GfVec3f kD = GfVec3f(1.0f - c.metallic)
                       * (1.0f - c.transmission);
            diffuse = GfCompMult(kD,
                EvalLambertian(c.baseColor, N, wi, wo));
        }

        GfVec3f sheen(0.0f);
        if (c.sheen > 0.0f) {
            sheen = EvalSheen(c.sheenColor, c.sheenRoughness, N, wi, wo)
                    * c.sheen;
        }

        GfVec3f coatContrib(0.0f);
        float coatAttenuation = 1.0f;
        if (c.coat > 0.0f) {
            coatContrib = EvalCoat(
                c.coat, c.coatRoughness, c.coatIor, N, wi, wo);
            float coatFresnel = _SchlickFresnelScalar(c.coatIor, VdotH);
            coatAttenuation = 1.0f - c.coat * coatFresnel;
        }

        reflected = (diffuse + specular + sheen) * coatAttenuation
                    + coatContrib;
    }

    // Thin-surface transmission: wi is in the opposite hemisphere.
    // This is a continuous approximation for NEE; the delta path is
    // handled separately by SampleSurface.
    GfVec3f transmitted(0.0f);
    if (c.transmission > 0.0f && NdotL < 0.0f) {
        float absNdotV = std::max(std::abs(GfDot(N, wo)), _kEpsilon);
        float fresnel = _SchlickFresnelScalar(c.specularIor, absNdotV);
        transmitted = c.transmissionColor
            * ((1.0f - fresnel) * c.transmission * _kInvPi);
    }

    return _SafeVec((reflected + transmitted) * c.opacity);
}

// ===========================================================================
// BSDF Sampling & PDF (Phase 9)
// ===========================================================================

MxLiteBsdf::BsdfSample
MxLiteBsdf::SampleLambertian(
    const GfVec3f& baseColor,
    const GfVec3f& N,
    const GfVec3f& wo,
    float u1, float u2)
{
    _Frame frame = _Frame::FromNormal(N);
    GfVec3f wiLocal = _SampleCosineHemisphere(u1, u2);
    GfVec3f wi = frame.ToWorld(wiLocal);

    float cosTheta = wiLocal[2];
    float pdf = _CosineHemispherePdf(cosTheta);

    return BsdfSample{wi, baseColor * _kInvPi, pdf, false};
}

float
MxLiteBsdf::PdfLambertian(const GfVec3f& N, const GfVec3f& wi)
{
    float cosTheta = GfDot(N, wi);
    return _CosineHemispherePdf(cosTheta);
}

MxLiteBsdf::BsdfSample
MxLiteBsdf::SampleGGXSpecular(
    float roughness,
    float ior,
    const GfVec3f& specularColor,
    const GfVec3f& N,
    const GfVec3f& wo,
    float u1, float u2)
{
    float alpha = _RoughnessToAlpha(roughness);

    _Frame frame = _Frame::FromNormal(N);
    GfVec3f woLocal = frame.ToLocal(wo);

    if (woLocal[2] <= 0.0f) {
        return BsdfSample{GfVec3f(0.0f), GfVec3f(0.0f), 0.0f, false};
    }

    // Sample microfacet normal via VNDF
    GfVec3f wmLocal = _SampleGGX_VNDF(woLocal, alpha, u1, u2);

    // Reflect wo about wm
    GfVec3f wiLocal = 2.0f * GfDot(woLocal, wmLocal) * wmLocal - woLocal;

    if (wiLocal[2] <= 0.0f) {
        return BsdfSample{GfVec3f(0.0f), GfVec3f(0.0f), 0.0f, false};
    }

    GfVec3f wi = frame.ToWorld(wiLocal);

    // Evaluate
    float NdotL = wiLocal[2];
    float NdotV = woLocal[2];
    float NdotH = wmLocal[2];
    float VdotH = std::max(GfDot(woLocal, wmLocal), 0.0f);

    float D = _GGX_D(alpha, NdotH);
    float V = _GGX_V(alpha, NdotV, NdotL);
    GfVec3f F = _SchlickFresnel(specularColor, VdotH);
    GfVec3f f = GfCompMult(F, GfVec3f(D * V));

    float pdf = _PdfGGX_VNDF(woLocal, wmLocal, alpha);

    return BsdfSample{wi, _SafeVec(f), std::max(pdf, 0.0f), false};
}

float
MxLiteBsdf::PdfGGXSpecular(
    float roughness,
    const GfVec3f& N,
    const GfVec3f& wi,
    const GfVec3f& wo)
{
    float alpha = _RoughnessToAlpha(roughness);

    _Frame frame = _Frame::FromNormal(N);
    GfVec3f woLocal = frame.ToLocal(wo);
    GfVec3f wiLocal = frame.ToLocal(wi);

    if (woLocal[2] <= 0.0f || wiLocal[2] <= 0.0f) return 0.0f;

    GfVec3f wmLocal = (woLocal + wiLocal).GetNormalized();
    if (wmLocal[2] <= 0.0f) return 0.0f;

    return _PdfGGX_VNDF(woLocal, wmLocal, alpha);
}

MxLiteBsdf::BsdfSample
MxLiteBsdf::SampleSurface(
    const MxLiteSurfaceClosure& c,
    const GfVec3f& N,
    const GfVec3f& wo,
    float u1, float u2, float uLobe)
{
    GfVec3f F0 = _ComputeF0(
        c.baseColor, c.metallic, c.specular, c.specularIor);
    bool hasSpecularLobe = (_Luminance(F0) > _kEpsilon);

    // Approximate lobe weights for selection.
    float wDiffuse = (1.0f - c.metallic) * (1.0f - c.transmission)
                     * _Luminance(c.baseColor);
    float wSpecular = hasSpecularLobe ? (_Luminance(F0) + 0.05f) : 0.0f;
    float wCoat = (c.coat > 0.0f) ? c.coat * 0.04f : 0.0f;
    float wTransmission = (c.transmission > 0.0f)
        ? c.transmission * std::max(_Luminance(c.transmissionColor), 0.1f)
        : 0.0f;

    float total = wDiffuse + wSpecular + wCoat + wTransmission;
    if (total <= 0.0f) {
        return BsdfSample{GfVec3f(0.0f), GfVec3f(0.0f), 0.0f, false};
    }

    float pDiffuse      = wDiffuse / total;
    float pSpecular     = wSpecular / total;
    float pCoat         = wCoat / total;
    // pTransmission = 1 - pDiffuse - pSpecular - pCoat (implicit)

    float cumDiffuse  = pDiffuse;
    float cumSpecular = cumDiffuse + pSpecular;
    float cumCoat     = cumSpecular + pCoat;

    if (uLobe < cumDiffuse) {
        // Diffuse lobe
        BsdfSample sample = SampleLambertian(c.baseColor, N, wo, u1, u2);
        if (sample.pdf <= 0.0f) {
            return BsdfSample{GfVec3f(0.0f), GfVec3f(0.0f), 0.0f, false};
        }
        GfVec3f f = EvalSurface(c, N, sample.wi, wo);
        float pdf = PdfSurface(c, N, sample.wi, wo);
        return BsdfSample{sample.wi, f, pdf, false};

    } else if (uLobe < cumSpecular) {
        // Specular lobe
        GfVec3f specCol = GfCompMult(c.specularColor, F0);
        BsdfSample sample = SampleGGXSpecular(
            c.roughness, c.specularIor, specCol, N, wo, u1, u2);
        if (sample.pdf <= 0.0f) {
            return BsdfSample{GfVec3f(0.0f), GfVec3f(0.0f), 0.0f, false};
        }
        GfVec3f f = EvalSurface(c, N, sample.wi, wo);
        float pdf = PdfSurface(c, N, sample.wi, wo);
        return BsdfSample{sample.wi, f, pdf, false};

    } else if (uLobe < cumCoat) {
        // Coat lobe
        float coatF0 = _SchlickFresnelScalar(c.coatIor, 1.0f);
        BsdfSample sample = SampleGGXSpecular(
            c.coatRoughness, c.coatIor, GfVec3f(coatF0), N, wo, u1, u2);
        if (sample.pdf <= 0.0f) {
            return BsdfSample{GfVec3f(0.0f), GfVec3f(0.0f), 0.0f, false};
        }
        GfVec3f f = EvalSurface(c, N, sample.wi, wo);
        float pdf = PdfSurface(c, N, sample.wi, wo);
        return BsdfSample{sample.wi, f, pdf, false};

    } else {
        // Specular refraction via Snell's law.
        float cosI = GfDot(N, wo);
        float eta;
        GfVec3f n;
        if (cosI > 0.0f) {
            // Entering: air -> medium
            eta = 1.0f / c.specularIor;
            n = N;
        } else {
            // Exiting: medium -> air
            eta = c.specularIor;
            n = -N;
            cosI = -cosI;
        }

        float sin2T = eta * eta * (1.0f - cosI * cosI);
        if (sin2T >= 1.0f) {
            // Total internal reflection.
            GfVec3f wi = 2.0f * GfDot(n, wo) * n - wo;
            wi.Normalize();
            return BsdfSample{wi, GfVec3f(c.opacity), 1.0f, true};
        }

        float cosT = std::sqrt(1.0f - sin2T);
        GfVec3f wi = -eta * wo + (eta * cosI - cosT) * n;
        wi.Normalize();

        float fresnel = _SchlickFresnelScalar(c.specularIor, cosI);
        GfVec3f T = c.transmissionColor
            * ((1.0f - fresnel) * c.transmission * c.opacity);
        return BsdfSample{wi, T, 1.0f, /*isSpecular=*/true};
    }
}

float
MxLiteBsdf::PdfSurface(
    const MxLiteSurfaceClosure& c,
    const GfVec3f& N,
    const GfVec3f& wi,
    const GfVec3f& wo)
{
    GfVec3f F0 = _ComputeF0(
        c.baseColor, c.metallic, c.specular, c.specularIor);
    bool hasSpecularLobe = (_Luminance(F0) > _kEpsilon);

    float wDiffuse = (1.0f - c.metallic) * (1.0f - c.transmission)
                     * _Luminance(c.baseColor);
    float wSpecular = hasSpecularLobe ? (_Luminance(F0) + 0.05f) : 0.0f;
    float wCoat = (c.coat > 0.0f) ? c.coat * 0.04f : 0.0f;
    float wTransmission = (c.transmission > 0.0f)
        ? c.transmission * std::max(_Luminance(c.transmissionColor), 0.1f)
        : 0.0f;

    float total = wDiffuse + wSpecular + wCoat + wTransmission;
    if (total <= 0.0f) return 0.0f;

    float pDiffuse  = wDiffuse / total;
    float pSpecular = wSpecular / total;
    float pCoat     = wCoat / total;
    // Transmission is a delta distribution: its continuous PDF is 0.
    // Including wTransmission in total correctly reduces the other
    // lobes' selection probabilities.

    float pdf = 0.0f;
    pdf += pDiffuse  * PdfLambertian(N, wi);
    pdf += pSpecular * PdfGGXSpecular(c.roughness, N, wi, wo);
    if (c.coat > 0.0f) {
        pdf += pCoat * PdfGGXSpecular(c.coatRoughness, N, wi, wo);
    }

    return pdf;
}

PXR_NAMESPACE_CLOSE_SCOPE
