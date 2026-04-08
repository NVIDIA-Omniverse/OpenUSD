//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "bsdf.h"

#include "../nodes/helpers/colorHelpers.h"
#include "../nodes/helpers/mathHelpers.h"

#include <algorithm>
#include <cmath>
#include <variant>

namespace mxcpp {

namespace {

constexpr float _kEpsilon = 1e-7f;

inline float
_Clamp01(float x)
{
    return std::clamp(x, 0.0f, 1.0f);
}

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

inline float
_AverageRoughness(const Vec2f& roughness)
{
    return std::sqrt(_ClampRoughness(roughness[0]) *
                     _ClampRoughness(roughness[1]));
}

inline Vec3f
_LerpVec(const Vec3f& a, const Vec3f& b, float t)
{
    return a * (1.0f - t) + b * t;
}

inline Vec3f
_SchlickFresnel(const Vec3f& F0, float cosTheta)
{
    float t = 1.0f - cosTheta;
    float t2 = t * t;
    float t5 = t2 * t2 * t;
    return F0 + (Vec3f(1.0f) - F0) * t5;
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

inline Vec3f
_GeneralizedSchlickFresnel(
    const Vec3f& color0,
    const Vec3f& color90,
    float exponent,
    float cosTheta)
{
    float x = std::pow(_Clamp01(1.0f - cosTheta), std::max(exponent, 0.0f));
    return _LerpVec(color0, color90, x);
}

inline Vec3f
_ConductorF0(const Vec3f& ior, const Vec3f& extinction)
{
    Vec3f result(0.0f);
    for (int i = 0; i < 3; ++i) {
        float n = std::max(ior[i], 0.0f);
        float k = std::max(extinction[i], 0.0f);
        float nMinusOne2 = (n - 1.0f) * (n - 1.0f);
        float nPlusOne2 = (n + 1.0f) * (n + 1.0f);
        result[i] = (nMinusOne2 + k * k) / (nPlusOne2 + k * k + _kEpsilon);
    }
    return result;
}

inline float
_GGX_D(float alpha, float NdotH)
{
    float a2 = alpha * alpha;
    float denom = NdotH * NdotH * (a2 - 1.0f) + 1.0f;
    return a2 / (kPi * denom * denom + _kEpsilon);
}

inline float
_GGX_V(float alpha, float NdotV, float NdotL)
{
    float a2 = alpha * alpha;
    float ggxV = NdotL * std::sqrt(NdotV * NdotV * (1.0f - a2) + a2);
    float ggxL = NdotV * std::sqrt(NdotL * NdotL * (1.0f - a2) + a2);
    return 0.5f / (ggxV + ggxL + _kEpsilon);
}

inline float
_SmithG1(float alpha, float cosTheta)
{
    float a2 = alpha * alpha;
    float cos2 = cosTheta * cosTheta;
    return 2.0f * cosTheta /
        (cosTheta + std::sqrt(a2 + (1.0f - a2) * cos2) + _kEpsilon);
}

inline Vec3f
_ComputeLegacyF0(const Vec3f& baseColor, float metallic,
                 float specular, float ior)
{
    float dielectricF0 = ((ior - 1.0f) / (ior + 1.0f));
    dielectricF0 *= dielectricF0;
    dielectricF0 *= specular;
    Vec3f F0 = Vec3f(dielectricF0);
    return F0 * (1.0f - metallic) + baseColor * metallic;
}

inline float
_Charlie_D(float alpha, float NdotH)
{
    float sinTheta2 = 1.0f - NdotH * NdotH;
    float sinTheta = std::sqrt(std::max(0.0f, sinTheta2));
    float invAlpha = 1.0f / std::max(alpha, _kEpsilon);
    return (2.0f + invAlpha) * std::pow(sinTheta, invAlpha) * kInvPi * 0.5f;
}

inline float
_Ashikhmin_V(float NdotV, float NdotL)
{
    return 1.0f / (4.0f * (NdotL + NdotV - NdotL * NdotV) + _kEpsilon);
}

inline float
_OrenNayarFactor(float NdotV, float NdotL, float LdotV, float roughness)
{
    float s = LdotV - NdotL * NdotV;
    float stinv = (s > 0.0f) ? s / std::max(NdotL, NdotV) : 0.0f;
    float sigma2 = roughness * roughness;
    float A = 1.0f - 0.5f * (sigma2 / (sigma2 + 0.33f));
    float B = 0.45f * sigma2 / (sigma2 + 0.09f);
    return A + B * stinv;
}

inline float
_BurleyFactor(float NdotV, float NdotL, float LdotH, float roughness)
{
    float F90 = 0.5f + (2.0f * roughness * LdotH * LdotH);
    auto schlick = [](float cosTheta, float F0, float F90Value) {
        float x = std::pow(_Clamp01(1.0f - cosTheta), 5.0f);
        return F0 + (F90Value - F0) * x;
    };
    return schlick(NdotL, 1.0f, F90) * schlick(NdotV, 1.0f, F90);
}

inline float
_ApproxSheenDirAlbedo(float NdotV, float roughness)
{
    float x = NdotV;
    float y = roughness;
    float numerator = 13.67300f +
        (-68.78018f * x) +
        (799.08825f * y) +
        (-905.00061f * x * y) +
        (60.28956f * x * x) +
        (1086.96473f * y * y);
    float denominator = 1.0f +
        (61.57746f * x) +
        (442.78211f * y) +
        (2597.49308f * x * y) +
        (121.81241f * x * x) +
        (3045.55075f * y * y);
    return _Clamp01(numerator / (denominator + _kEpsilon));
}

struct _Frame {
    Vec3f T, B, N;

    Vec3f ToLocal(const Vec3f& v) const {
        return Vec3f(Dot(v, T), Dot(v, B), Dot(v, N));
    }

    Vec3f ToWorld(const Vec3f& v) const {
        return T * v[0] + B * v[1] + N * v[2];
    }

    static _Frame FromNormal(const Vec3f& n) {
        _Frame f;
        f.N = n;
        const Vec3f helper =
            (std::abs(n[0]) < 0.9f) ? Vec3f(1.0f, 0.0f, 0.0f)
                                    : Vec3f(0.0f, 1.0f, 0.0f);
        f.T = n.cross(helper).normalized();
        f.B = n.cross(f.T);
        return f;
    }
};

inline Vec3f
_SampleCosineHemisphere(float u1, float u2)
{
    float cosTheta = std::sqrt(u1);
    float sinTheta = std::sqrt(1.0f - u1);
    float phi = 2.0f * kPi * u2;
    return Vec3f(sinTheta * std::cos(phi),
                 sinTheta * std::sin(phi),
                 cosTheta);
}

inline float
_CosineHemispherePdf(float cosTheta)
{
    return std::max(cosTheta, 0.0f) * kInvPi;
}

Vec3f
_SampleGGX_VNDF(const Vec3f& woLocal, float alpha, float u1, float u2)
{
    Vec3f wh(alpha * woLocal[0], alpha * woLocal[1], woLocal[2]);
    float whLen = wh.length();
    if (whLen < _kEpsilon) {
        return Vec3f(0.0f, 0.0f, 1.0f);
    }
    wh /= whLen;
    if (wh[2] < 0.0f) {
        wh = -wh;
    }

    Vec3f T1 = (wh[2] < 0.99999f)
        ? Cross(Vec3f(0.0f, 0.0f, 1.0f), wh).normalized()
        : Vec3f(1.0f, 0.0f, 0.0f);
    Vec3f T2 = Cross(wh, T1);

    float r = std::sqrt(u1);
    float phi = 2.0f * kPi * u2;
    float t1 = r * std::cos(phi);
    float t2 = r * std::sin(phi);

    float h = std::sqrt(std::max(0.0f, 1.0f - t1 * t1));
    float blend = (1.0f + wh[2]) * 0.5f;
    t2 = (1.0f - blend) * h + blend * t2;

    float pz = std::sqrt(std::max(0.0f, 1.0f - t1 * t1 - t2 * t2));
    Vec3f nh = T1 * t1 + T2 * t2 + wh * pz;

    Vec3f wm(alpha * nh[0], alpha * nh[1], std::max(1e-6f, nh[2]));
    return wm.normalized();
}

float
_PdfGGX_VNDF(const Vec3f& woLocal, const Vec3f& wmLocal, float alpha)
{
    float cosThetaO = std::max(woLocal[2], _kEpsilon);
    float G1 = _SmithG1(alpha, cosThetaO);
    float NdotH = std::max(wmLocal[2], 0.0f);
    float VdotH = std::max(Dot(woLocal, wmLocal), _kEpsilon);
    float D = _GGX_D(alpha, NdotH);
    float pdfWm = G1 * D * VdotH / cosThetaO;
    return pdfWm / (4.0f * VdotH);
}

inline float
_Luminance(const Vec3f& c)
{
    return kRec709LumaR * c[0] + kRec709LumaG * c[1] + kRec709LumaB * c[2];
}

inline Vec3f
_SafeVec(const Vec3f& v)
{
    Vec3f r = v;
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(r[i]) || r[i] < 0.0f) {
            r[i] = 0.0f;
        }
    }
    return r;
}

inline Vec3f
_SaturateVec(const Vec3f& v)
{
    return Vec3f(_Clamp01(v[0]), _Clamp01(v[1]), _Clamp01(v[2]));
}

inline Vec3f
_FaceForwardNormal(const Vec3f& N, const Vec3f& wo)
{
    return (Dot(N, wo) < 0.0f) ? -N : N;
}

inline bool
_IsSameSide(const Vec3f& N, const Vec3f& wi, const Vec3f& wo)
{
    return Dot(N, wi) * Dot(N, wo) > 0.0f;
}

Vec3f
_EvalMicrofacetReflection(
    float roughness,
    const Vec3f& fresnel,
    const Vec3f& N,
    const Vec3f& wi,
    const Vec3f& wo)
{
    float alpha = _RoughnessToAlpha(roughness);
    float NdotL = std::max(Dot(N, wi), 0.0f);
    float NdotV = std::max(Dot(N, wo), _kEpsilon);
    if (NdotL <= 0.0f || NdotV <= 0.0f) {
        return Vec3f(0.0f);
    }
    Vec3f H = (wi + wo).normalized();
    float NdotH = std::max(Dot(N, H), 0.0f);
    float D = _GGX_D(alpha, NdotH);
    float V = _GGX_V(alpha, NdotV, NdotL);
    return _SafeVec(CompMul(fresnel, Vec3f(D * V)));
}

float
_PdfTranslucent(const Vec3f& N, const Vec3f& wi)
{
    if (Dot(N, wi) >= 0.0f) {
        return 0.0f;
    }
    return _CosineHemispherePdf(std::abs(Dot(N, wi)));
}

Vec3f
_EvalTranslucent(const Vec3f& color, float weight, const Vec3f& N,
                 const Vec3f& wi)
{
    if (Dot(N, wi) >= 0.0f || weight <= 0.0f) {
        return Vec3f(0.0f);
    }
    return color * (weight * kInvPi);
}

Bsdf::BsdfSample
_SampleDeltaTransmission(
    float ior,
    const Vec3f& tint,
    float weight,
    const Vec3f& N,
    const Vec3f& wo)
{
    float cosI = Dot(N, wo);
    float eta = 1.0f;
    Vec3f n = N;
    if (cosI > 0.0f) {
        eta = 1.0f / ior;
    } else {
        eta = ior;
        n = -N;
        cosI = -cosI;
    }

    float sin2T = eta * eta * (1.0f - cosI * cosI);
    if (sin2T >= 1.0f) {
        Vec3f wi = 2.0f * Dot(n, wo) * n - wo;
        wi.normalize();
        Bsdf::BsdfSample sample{wi, Vec3f(weight), 1.0f, true};
        sample.eta = 1.0f;
        return sample;
    }

    float cosT = std::sqrt(1.0f - sin2T);
    Vec3f wi = -eta * wo + (eta * cosI - cosT) * n;
    wi.normalize();
    float fresnel = _SchlickFresnelScalar(ior, cosI);
    Bsdf::BsdfSample sample{
        wi,
        tint * ((1.0f - fresnel) * weight),
        1.0f,
        true
    };
    sample.eta = eta;
    return sample;
}

Vec3f
_EvalLegacySurface(
    const SurfaceClosure& c,
    const Vec3f& N,
    const Vec3f& wi,
    const Vec3f& wo)
{
    float NdotL = Dot(N, wi);

    Vec3f reflected(0.0f);
    if (NdotL > 0.0f) {
        Vec3f F0 = _ComputeLegacyF0(
            c.baseColor, c.metallic, c.specular, c.specularIor);
        bool hasSpecularLobe = (_Luminance(F0) > _kEpsilon);

        Vec3f H = (wi + wo).normalized();
        float VdotH = std::max(Dot(wo, H), 0.0f);

        Vec3f diffuse(0.0f);
        Vec3f specular(0.0f);

        if (hasSpecularLobe) {
            Vec3f fresnel = _SchlickFresnel(F0, VdotH);
            Vec3f kD = CompMul(Vec3f(1.0f) - fresnel,
                               Vec3f(1.0f - c.metallic));
            kD = kD * (1.0f - c.transmission);
            diffuse = CompMul(kD, Bsdf::EvalLambertian(c.baseColor, N, wi, wo));

            specular = Bsdf::EvalGGXSpecular(
                c.roughness, c.specularIor,
                CompMul(c.specularColor, F0), N, wi, wo);
        } else {
            Vec3f kD = Vec3f(1.0f - c.metallic) * (1.0f - c.transmission);
            diffuse = CompMul(kD, Bsdf::EvalLambertian(c.baseColor, N, wi, wo));
        }

        Vec3f sheen(0.0f);
        if (c.sheen > 0.0f) {
            sheen = Bsdf::EvalSheen(c.sheenColor, c.sheenRoughness, N, wi, wo) *
                    c.sheen;
        }

        Vec3f coatContrib(0.0f);
        float coatAttenuation = 1.0f;
        if (c.coat > 0.0f) {
            coatContrib = Bsdf::EvalCoat(
                c.coat, c.coatRoughness, c.coatIor, N, wi, wo);
            float coatFresnel = _SchlickFresnelScalar(c.coatIor, VdotH);
            coatAttenuation = 1.0f - c.coat * coatFresnel;
        }

        reflected = (diffuse + specular + sheen) * coatAttenuation +
                    coatContrib;
    }

    Vec3f transmitted(0.0f);
    if (c.transmission > 0.0f && NdotL < 0.0f) {
        float absNdotV = std::max(std::abs(Dot(N, wo)), _kEpsilon);
        float fresnel = _SchlickFresnelScalar(c.specularIor, absNdotV);
        transmitted = c.transmissionColor *
            ((1.0f - fresnel) * c.transmission * kInvPi);
    }

    return _SafeVec((reflected + transmitted) * c.presence);
}

float
_PdfLegacySurface(
    const SurfaceClosure& c,
    const Vec3f& N,
    const Vec3f& wi,
    const Vec3f& wo)
{
    Vec3f F0 = _ComputeLegacyF0(
        c.baseColor, c.metallic, c.specular, c.specularIor);
    bool hasSpecularLobe = (_Luminance(F0) > _kEpsilon);

    float wDiffuse = (1.0f - c.metallic) * (1.0f - c.transmission) *
                     _Luminance(c.baseColor);
    float wSpecular = hasSpecularLobe ? (_Luminance(F0) + 0.05f) : 0.0f;
    float wCoat = (c.coat > 0.0f) ? c.coat * 0.04f : 0.0f;
    float wTransmission = (c.transmission > 0.0f)
        ? c.transmission * std::max(_Luminance(c.transmissionColor), 0.1f)
        : 0.0f;

    float total = wDiffuse + wSpecular + wCoat + wTransmission;
    if (total <= 0.0f) {
        return 0.0f;
    }

    float pDiffuse = wDiffuse / total;
    float pSpecular = wSpecular / total;
    float pCoat = wCoat / total;

    float pdf = 0.0f;
    pdf += pDiffuse * Bsdf::PdfLambertian(N, wi);
    pdf += pSpecular * Bsdf::PdfGGXSpecular(c.roughness, N, wi, wo);
    if (c.coat > 0.0f) {
        pdf += pCoat * Bsdf::PdfGGXSpecular(c.coatRoughness, N, wi, wo);
    }
    return pdf;
}

Bsdf::BsdfSample
_SampleLegacySurface(
    const SurfaceClosure& c,
    const Vec3f& N,
    const Vec3f& wo,
    float u1,
    float u2,
    float uLobe)
{
    Vec3f F0 = _ComputeLegacyF0(
        c.baseColor, c.metallic, c.specular, c.specularIor);
    bool hasSpecularLobe = (_Luminance(F0) > _kEpsilon);

    float wDiffuse = (1.0f - c.metallic) * (1.0f - c.transmission) *
                     _Luminance(c.baseColor);
    float wSpecular = hasSpecularLobe ? (_Luminance(F0) + 0.05f) : 0.0f;
    float wCoat = (c.coat > 0.0f) ? c.coat * 0.04f : 0.0f;
    float wTransmission = (c.transmission > 0.0f)
        ? c.transmission * std::max(_Luminance(c.transmissionColor), 0.1f)
        : 0.0f;

    float total = wDiffuse + wSpecular + wCoat + wTransmission;
    if (total <= 0.0f) {
        return Bsdf::BsdfSample{Vec3f(0.0f), Vec3f(0.0f), 0.0f, false};
    }

    float pDiffuse = wDiffuse / total;
    float pSpecular = wSpecular / total;
    float pCoat = wCoat / total;
    float cumDiffuse = pDiffuse;
    float cumSpecular = cumDiffuse + pSpecular;
    float cumCoat = cumSpecular + pCoat;

    if (uLobe < cumDiffuse) {
        auto sample = Bsdf::SampleLambertian(c.baseColor, N, wo, u1, u2);
        if (sample.pdf <= 0.0f) {
            return Bsdf::BsdfSample{Vec3f(0.0f), Vec3f(0.0f), 0.0f, false};
        }
        sample.f = _EvalLegacySurface(c, N, sample.wi, wo);
        sample.pdf = _PdfLegacySurface(c, N, sample.wi, wo);
        return sample;
    }
    if (uLobe < cumSpecular) {
        Vec3f specCol = CompMul(c.specularColor, F0);
        auto sample = Bsdf::SampleGGXSpecular(
            c.roughness, c.specularIor, specCol, N, wo, u1, u2);
        if (sample.pdf <= 0.0f) {
            return Bsdf::BsdfSample{Vec3f(0.0f), Vec3f(0.0f), 0.0f, false};
        }
        sample.f = _EvalLegacySurface(c, N, sample.wi, wo);
        sample.pdf = _PdfLegacySurface(c, N, sample.wi, wo);
        return sample;
    }
    if (uLobe < cumCoat) {
        float coatF0 = _SchlickFresnelScalar(c.coatIor, 1.0f);
        auto sample = Bsdf::SampleGGXSpecular(
            c.coatRoughness, c.coatIor, Vec3f(coatF0), N, wo, u1, u2);
        if (sample.pdf <= 0.0f) {
            return Bsdf::BsdfSample{Vec3f(0.0f), Vec3f(0.0f), 0.0f, false};
        }
        sample.f = _EvalLegacySurface(c, N, sample.wi, wo);
        sample.pdf = _PdfLegacySurface(c, N, sample.wi, wo);
        return sample;
    }
    return _SampleDeltaTransmission(
        c.specularIor,
        c.transmissionColor * (c.transmission * c.presence),
        1.0f,
        N,
        wo);
}

Vec3f
_EvalNode(const Bsdf::ClosureTree& tree, Bsdf::NodeId nodeId,
          const Vec3f& N, const Vec3f& wi, const Vec3f& wo);

Vec3f
_EvalThroughput(const Bsdf::ClosureTree& tree, Bsdf::NodeId nodeId,
                const Vec3f& N, const Vec3f& wo);

float
_ApproxWeight(const Bsdf::ClosureTree& tree, Bsdf::NodeId nodeId,
              const Vec3f& N, const Vec3f& wo);

float
_PdfNode(const Bsdf::ClosureTree& tree, Bsdf::NodeId nodeId,
         const Vec3f& N, const Vec3f& wi, const Vec3f& wo);

Bsdf::BsdfSample
_SampleNode(const Bsdf::ClosureTree& tree, Bsdf::NodeId nodeId,
            const Vec3f& N, const Vec3f& wo,
            float u1, float u2, float uChoice);

Vec3f
_EvalNode(const Bsdf::ClosureTree& tree, Bsdf::NodeId nodeId,
          const Vec3f& N, const Vec3f& wi, const Vec3f& wo)
{
    const Bsdf::Node* node = tree.Get(nodeId);
    if (!node) {
        return Vec3f(0.0f);
    }

    return std::visit([&](const auto& data) -> Vec3f {
        using T = std::decay_t<decltype(data)>;
        if constexpr (std::is_same_v<T, Bsdf::OrenNayarDiffuseData>) {
            if (Dot(N, wi) <= 0.0f || data.weight <= 0.0f) {
                return Vec3f(0.0f);
            }
            float NdotL = std::max(Dot(N, wi), 0.0f);
            float NdotV = std::max(Dot(N, wo), _kEpsilon);
            float LdotV = std::max(Dot(wi, wo), 0.0f);
            float factor = _OrenNayarFactor(NdotV, NdotL, LdotV, data.roughness);
            return _SafeVec(data.color * (data.weight * factor * kInvPi));
        } else if constexpr (std::is_same_v<T, Bsdf::BurleyDiffuseData>) {
            if (Dot(N, wi) <= 0.0f || data.weight <= 0.0f) {
                return Vec3f(0.0f);
            }
            float NdotL = std::max(Dot(N, wi), 0.0f);
            float NdotV = std::max(Dot(N, wo), _kEpsilon);
            Vec3f H = (wi + wo).normalized();
            float LdotH = std::max(Dot(wi, H), 0.0f);
            float factor = _BurleyFactor(NdotV, NdotL, LdotH, data.roughness);
            return _SafeVec(data.color * (data.weight * factor * kInvPi));
        } else if constexpr (std::is_same_v<T, Bsdf::TranslucentData>) {
            return _SafeVec(_EvalTranslucent(data.color, data.weight, N, wi));
        } else if constexpr (std::is_same_v<T, Bsdf::SubsurfaceData>) {
            return Vec3f(0.0f);
        } else if constexpr (std::is_same_v<T, Bsdf::DielectricData>) {
            Vec3f result(0.0f);
            float roughness = _AverageRoughness(data.roughness);
            bool sameSide = _IsSameSide(N, wi, wo);
            Vec3f shadingN = _FaceForwardNormal(N, wo);
            float F0 = ((data.ior - 1.0f) / (data.ior + 1.0f));
            F0 *= F0;
            Vec3f F0Color = Vec3f(F0) * data.tint;
            if (sameSide &&
                data.scatterMode != Bsdf::ScatterMode::Transmission) {
                Vec3f H = (wi + wo).normalized();
                float VdotH = std::max(Dot(wo, H), 0.0f);
                Vec3f fresnel = _SchlickFresnel(F0Color, VdotH);
                result += _EvalMicrofacetReflection(
                    roughness, fresnel * data.weight, shadingN, wi, wo);
            }
            if (!sameSide &&
                data.scatterMode != Bsdf::ScatterMode::Reflection) {
                result += Bsdf::EvalGGXTransmission(
                    roughness, data.ior, data.tint, N, wi, wo) * data.weight;
            }
            return _SafeVec(result);
        } else if constexpr (std::is_same_v<T, Bsdf::ConductorData>) {
            if (Dot(N, wi) <= 0.0f || data.weight <= 0.0f) {
                return Vec3f(0.0f);
            }
            float roughness = _AverageRoughness(data.roughness);
            Vec3f H = (wi + wo).normalized();
            float VdotH = std::max(Dot(wo, H), 0.0f);
            Vec3f F0 = _ConductorF0(data.ior, data.extinction);
            Vec3f fresnel = _SchlickFresnel(F0, VdotH);
            return _EvalMicrofacetReflection(
                roughness, fresnel * data.weight, N, wi, wo);
        } else if constexpr (std::is_same_v<T, Bsdf::GeneralizedSchlickData>) {
            Vec3f result(0.0f);
            float roughness = _AverageRoughness(data.roughness);
            bool sameSide = _IsSameSide(N, wi, wo);
            Vec3f shadingN = _FaceForwardNormal(N, wo);
            if (sameSide &&
                data.scatterMode != Bsdf::ScatterMode::Transmission) {
                Vec3f H = (wi + wo).normalized();
                float VdotH = std::max(Dot(wo, H), 0.0f);
                Vec3f fresnel = _GeneralizedSchlickFresnel(
                    data.color0, data.color90, data.exponent, VdotH);
                result += _EvalMicrofacetReflection(
                    roughness, fresnel * data.weight, shadingN, wi, wo);
            }
            if (!sameSide &&
                data.scatterMode != Bsdf::ScatterMode::Reflection) {
                float avgF0 = _Clamp01(_Luminance(_SaturateVec(data.color0)));
                float ior = (1.0f + std::sqrt(std::max(avgF0, 0.01f))) /
                            (1.0f - std::sqrt(std::max(avgF0, 0.01f)));
                result += Bsdf::EvalGGXTransmission(
                    roughness, ior, Vec3f(1.0f), N, wi, wo) * data.weight;
            }
            return _SafeVec(result);
        } else if constexpr (std::is_same_v<T, Bsdf::SheenData>) {
            if (Dot(N, wi) <= 0.0f || data.weight <= 0.0f) {
                return Vec3f(0.0f);
            }
            return _SafeVec(Bsdf::EvalSheen(
                data.color, data.roughness, N, wi, wo) * data.weight);
        } else if constexpr (std::is_same_v<T, Bsdf::UnsupportedData>) {
            return Vec3f(0.0f);
        } else if constexpr (std::is_same_v<T, Bsdf::MixData>) {
            return _LerpVec(
                _EvalNode(tree, data.bg, N, wi, wo),
                _EvalNode(tree, data.fg, N, wi, wo),
                _Clamp01(data.mix));
        } else if constexpr (std::is_same_v<T, Bsdf::LayerData>) {
            Vec3f topEval = _EvalNode(tree, data.top, N, wi, wo);
            Vec3f baseEval = _EvalNode(tree, data.base, N, wi, wo);
            Vec3f topThroughput = _EvalThroughput(tree, data.top, N, wo);
            return topEval + CompMul(baseEval, topThroughput);
        } else if constexpr (std::is_same_v<T, Bsdf::AddData>) {
            return _EvalNode(tree, data.in1, N, wi, wo) +
                   _EvalNode(tree, data.in2, N, wi, wo);
        } else if constexpr (std::is_same_v<T, Bsdf::MultiplyData>) {
            return CompMul(data.weight, _EvalNode(tree, data.input, N, wi, wo));
        } else {
            return Vec3f(0.0f);
        }
    }, node->data);
}

Vec3f
_EvalThroughput(const Bsdf::ClosureTree& tree, Bsdf::NodeId nodeId,
                const Vec3f& N, const Vec3f& wo)
{
    const Bsdf::Node* node = tree.Get(nodeId);
    if (!node) {
        return Vec3f(0.0f);
    }

    return std::visit([&](const auto& data) -> Vec3f {
        using T = std::decay_t<decltype(data)>;
        if constexpr (std::is_same_v<T, Bsdf::OrenNayarDiffuseData> ||
                      std::is_same_v<T, Bsdf::BurleyDiffuseData> ||
                      std::is_same_v<T, Bsdf::TranslucentData> ||
                      std::is_same_v<T, Bsdf::SubsurfaceData> ||
                      std::is_same_v<T, Bsdf::UnsupportedData>) {
            return Vec3f(0.0f);
        } else if constexpr (std::is_same_v<T, Bsdf::DielectricData>) {
            float NdotV = std::max(std::abs(Dot(N, wo)), _kEpsilon);
            Vec3f throughput(1.0f);
            if (data.scatterMode != Bsdf::ScatterMode::Transmission) {
                float F = _SchlickFresnelScalar(data.ior, NdotV);
                throughput -= Vec3f(F * data.weight);
            }
            return _SaturateVec(throughput);
        } else if constexpr (std::is_same_v<T, Bsdf::ConductorData>) {
            float NdotV = std::max(std::abs(Dot(N, wo)), _kEpsilon);
            Vec3f F = _SchlickFresnel(_ConductorF0(data.ior, data.extinction), NdotV);
            return _SaturateVec(Vec3f(1.0f) - F * data.weight);
        } else if constexpr (std::is_same_v<T, Bsdf::GeneralizedSchlickData>) {
            float NdotV = std::max(std::abs(Dot(N, wo)), _kEpsilon);
            Vec3f F = _GeneralizedSchlickFresnel(
                data.color0, data.color90, data.exponent, NdotV);
            return _SaturateVec(Vec3f(1.0f) - F * data.weight);
        } else if constexpr (std::is_same_v<T, Bsdf::SheenData>) {
            float NdotV = std::max(std::abs(Dot(N, wo)), _kEpsilon);
            float dirAlbedo = _ApproxSheenDirAlbedo(NdotV, data.roughness);
            return _SaturateVec(Vec3f(1.0f - dirAlbedo * data.weight));
        } else if constexpr (std::is_same_v<T, Bsdf::MixData>) {
            return _LerpVec(
                _EvalThroughput(tree, data.bg, N, wo),
                _EvalThroughput(tree, data.fg, N, wo),
                _Clamp01(data.mix));
        } else if constexpr (std::is_same_v<T, Bsdf::LayerData>) {
            return CompMul(
                _EvalThroughput(tree, data.top, N, wo),
                _EvalThroughput(tree, data.base, N, wo));
        } else if constexpr (std::is_same_v<T, Bsdf::AddData>) {
            Vec3f t = _EvalThroughput(tree, data.in1, N, wo) +
                      _EvalThroughput(tree, data.in2, N, wo) -
                      Vec3f(1.0f);
            return Vec3f(std::max(t[0], 0.0f),
                         std::max(t[1], 0.0f),
                         std::max(t[2], 0.0f));
        } else if constexpr (std::is_same_v<T, Bsdf::MultiplyData>) {
            return _EvalThroughput(tree, data.input, N, wo);
        } else {
            return Vec3f(0.0f);
        }
    }, node->data);
}

float
_ApproxWeight(const Bsdf::ClosureTree& tree, Bsdf::NodeId nodeId,
              const Vec3f& N, const Vec3f& wo)
{
    const Bsdf::Node* node = tree.Get(nodeId);
    if (!node) {
        return 0.0f;
    }

    return std::visit([&](const auto& data) -> float {
        using T = std::decay_t<decltype(data)>;
        if constexpr (std::is_same_v<T, Bsdf::OrenNayarDiffuseData> ||
                      std::is_same_v<T, Bsdf::BurleyDiffuseData>) {
            return data.weight * std::max(_Luminance(data.color), 0.0f);
        } else if constexpr (std::is_same_v<T, Bsdf::TranslucentData>) {
            return data.weight * std::max(_Luminance(data.color), 0.0f);
        } else if constexpr (std::is_same_v<T, Bsdf::SubsurfaceData>) {
            return 0.0f;
        } else if constexpr (std::is_same_v<T, Bsdf::DielectricData>) {
            float NdotV = std::max(std::abs(Dot(N, wo)), _kEpsilon);
            float F = _SchlickFresnelScalar(data.ior, NdotV);
            float t = data.weight * std::max(_Luminance(data.tint), 0.05f);
            if (data.scatterMode == Bsdf::ScatterMode::Reflection) {
                return std::max(F * data.weight, 0.05f);
            }
            if (data.scatterMode == Bsdf::ScatterMode::Transmission) {
                return std::max((1.0f - F) * t, 0.05f);
            }
            return std::max(t, 0.05f);
        } else if constexpr (std::is_same_v<T, Bsdf::ConductorData>) {
            return data.weight *
                std::max(_Luminance(_ConductorF0(data.ior, data.extinction)), 0.05f);
        } else if constexpr (std::is_same_v<T, Bsdf::GeneralizedSchlickData>) {
            return data.weight *
                std::max(_Luminance(_SaturateVec(data.color0)), 0.05f);
        } else if constexpr (std::is_same_v<T, Bsdf::SheenData>) {
            return data.weight * std::max(_Luminance(data.color), 0.0f) * 0.25f;
        } else if constexpr (std::is_same_v<T, Bsdf::UnsupportedData>) {
            return 0.0f;
        } else if constexpr (std::is_same_v<T, Bsdf::MixData>) {
            return _LerpVec(
                Vec3f(_ApproxWeight(tree, data.bg, N, wo)),
                Vec3f(_ApproxWeight(tree, data.fg, N, wo)),
                _Clamp01(data.mix))[0];
        } else if constexpr (std::is_same_v<T, Bsdf::LayerData>) {
            return _ApproxWeight(tree, data.top, N, wo) +
                _ApproxWeight(tree, data.base, N, wo) *
                _Luminance(_EvalThroughput(tree, data.top, N, wo));
        } else if constexpr (std::is_same_v<T, Bsdf::AddData>) {
            return _ApproxWeight(tree, data.in1, N, wo) +
                   _ApproxWeight(tree, data.in2, N, wo);
        } else if constexpr (std::is_same_v<T, Bsdf::MultiplyData>) {
            return _ApproxWeight(tree, data.input, N, wo) *
                   std::max(_Luminance(data.weight), 0.0f);
        } else {
            return 0.0f;
        }
    }, node->data);
}

float
_PdfNode(const Bsdf::ClosureTree& tree, Bsdf::NodeId nodeId,
         const Vec3f& N, const Vec3f& wi, const Vec3f& wo)
{
    const Bsdf::Node* node = tree.Get(nodeId);
    if (!node) {
        return 0.0f;
    }

    return std::visit([&](const auto& data) -> float {
        using T = std::decay_t<decltype(data)>;
        if constexpr (std::is_same_v<T, Bsdf::OrenNayarDiffuseData> ||
                      std::is_same_v<T, Bsdf::BurleyDiffuseData>) {
            return (Dot(N, wi) > 0.0f) ? Bsdf::PdfLambertian(N, wi) : 0.0f;
        } else if constexpr (std::is_same_v<T, Bsdf::TranslucentData>) {
            return _PdfTranslucent(N, wi);
        } else if constexpr (std::is_same_v<T, Bsdf::SubsurfaceData>) {
            return 0.0f;
        } else if constexpr (std::is_same_v<T, Bsdf::DielectricData>) {
            float roughness = _AverageRoughness(data.roughness);
            bool sameSide = _IsSameSide(N, wi, wo);
            Vec3f shadingN = _FaceForwardNormal(N, wo);
            if (sameSide &&
                data.scatterMode != Bsdf::ScatterMode::Transmission) {
                return Bsdf::PdfGGXSpecular(roughness, shadingN, wi, wo);
            }
            if (!sameSide &&
                data.scatterMode != Bsdf::ScatterMode::Reflection) {
                return Bsdf::PdfGGXTransmission(roughness, data.ior, N, wi, wo);
            }
            return 0.0f;
        } else if constexpr (std::is_same_v<T, Bsdf::ConductorData>) {
            return (Dot(N, wi) > 0.0f)
                ? Bsdf::PdfGGXSpecular(_AverageRoughness(data.roughness), N, wi, wo)
                : 0.0f;
        } else if constexpr (std::is_same_v<T, Bsdf::GeneralizedSchlickData>) {
            float roughness = _AverageRoughness(data.roughness);
            bool sameSide = _IsSameSide(N, wi, wo);
            Vec3f shadingN = _FaceForwardNormal(N, wo);
            if (sameSide &&
                data.scatterMode != Bsdf::ScatterMode::Transmission) {
                return Bsdf::PdfGGXSpecular(roughness, shadingN, wi, wo);
            }
            if (!sameSide &&
                data.scatterMode != Bsdf::ScatterMode::Reflection) {
                float avgF0 = _Clamp01(_Luminance(_SaturateVec(data.color0)));
                float sqrtF0 = std::sqrt(std::max(avgF0, 0.01f));
                float ior = (1.0f + sqrtF0) / (1.0f - sqrtF0);
                return Bsdf::PdfGGXTransmission(roughness, ior, N, wi, wo);
            }
            return 0.0f;
        } else if constexpr (std::is_same_v<T, Bsdf::SheenData>) {
            return (Dot(N, wi) > 0.0f) ? Bsdf::PdfLambertian(N, wi) : 0.0f;
        } else if constexpr (std::is_same_v<T, Bsdf::UnsupportedData>) {
            return 0.0f;
        } else if constexpr (std::is_same_v<T, Bsdf::MixData>) {
            float mix = _Clamp01(data.mix);
            return (1.0f - mix) * _PdfNode(tree, data.bg, N, wi, wo) +
                   mix * _PdfNode(tree, data.fg, N, wi, wo);
        } else if constexpr (std::is_same_v<T, Bsdf::LayerData>) {
            float topWeight = _ApproxWeight(tree, data.top, N, wo);
            float baseWeight = _ApproxWeight(tree, data.base, N, wo) *
                _Luminance(_EvalThroughput(tree, data.top, N, wo));
            float total = topWeight + baseWeight;
            if (total <= 0.0f) {
                return 0.0f;
            }
            return (topWeight / total) * _PdfNode(tree, data.top, N, wi, wo) +
                   (baseWeight / total) * _PdfNode(tree, data.base, N, wi, wo);
        } else if constexpr (std::is_same_v<T, Bsdf::AddData>) {
            float w1 = _ApproxWeight(tree, data.in1, N, wo);
            float w2 = _ApproxWeight(tree, data.in2, N, wo);
            float total = w1 + w2;
            if (total <= 0.0f) {
                return 0.0f;
            }
            return (w1 / total) * _PdfNode(tree, data.in1, N, wi, wo) +
                   (w2 / total) * _PdfNode(tree, data.in2, N, wi, wo);
        } else if constexpr (std::is_same_v<T, Bsdf::MultiplyData>) {
            return _PdfNode(tree, data.input, N, wi, wo);
        } else {
            return 0.0f;
        }
    }, node->data);
}

Bsdf::BsdfSample
_FinalizeSubtreeSample(const Bsdf::ClosureTree& tree,
                       Bsdf::NodeId nodeId,
                       const Vec3f& N,
                       const Vec3f& wo,
                       Bsdf::BsdfSample sample)
{
    if (sample.pdf <= 0.0f) {
        return sample;
    }
    if (sample.isSpecular) {
        sample.pdf = std::max(sample.pdf, 1.0f);
        return sample;
    }
    sample.f = _EvalNode(tree, nodeId, N, sample.wi, wo);
    sample.pdf = _PdfNode(tree, nodeId, N, sample.wi, wo);
    return sample;
}

Bsdf::BsdfSample
_SampleNode(const Bsdf::ClosureTree& tree, Bsdf::NodeId nodeId,
            const Vec3f& N, const Vec3f& wo,
            float u1, float u2, float uChoice)
{
    const Bsdf::Node* node = tree.Get(nodeId);
    if (!node) {
        return Bsdf::BsdfSample{Vec3f(0.0f), Vec3f(0.0f), 0.0f, false};
    }

    return std::visit([&](const auto& data) -> Bsdf::BsdfSample {
        using T = std::decay_t<decltype(data)>;
        if constexpr (std::is_same_v<T, Bsdf::OrenNayarDiffuseData>) {
            auto sample = Bsdf::SampleLambertian(data.color * data.weight, N, wo, u1, u2);
            return _FinalizeSubtreeSample(tree, nodeId, N, wo, sample);
        } else if constexpr (std::is_same_v<T, Bsdf::BurleyDiffuseData>) {
            auto sample = Bsdf::SampleLambertian(data.color * data.weight, N, wo, u1, u2);
            return _FinalizeSubtreeSample(tree, nodeId, N, wo, sample);
        } else if constexpr (std::is_same_v<T, Bsdf::TranslucentData>) {
            _Frame frame = _Frame::FromNormal(-N);
            Vec3f wiLocal = _SampleCosineHemisphere(u1, u2);
            Vec3f wi = frame.ToWorld(wiLocal);
            Bsdf::BsdfSample sample{
                wi,
                _EvalTranslucent(data.color, data.weight, N, wi),
                _CosineHemispherePdf(wiLocal[2]),
                false
            };
            return _FinalizeSubtreeSample(tree, nodeId, N, wo, sample);
        } else if constexpr (std::is_same_v<T, Bsdf::SubsurfaceData>) {
            return Bsdf::BsdfSample{Vec3f(0.0f), Vec3f(0.0f), 0.0f, false};
        } else if constexpr (std::is_same_v<T, Bsdf::DielectricData>) {
            float NdotV = std::max(std::abs(Dot(N, wo)), _kEpsilon);
            float fresnelProb = _Clamp01(_SchlickFresnelScalar(data.ior, NdotV));
            Vec3f shadingN = _FaceForwardNormal(N, wo);
            if (data.scatterMode == Bsdf::ScatterMode::Reflection) {
                Vec3f F0 = Vec3f(_SchlickFresnelScalar(data.ior, 1.0f)) * data.tint;
                auto sample = Bsdf::SampleGGXSpecular(
                    _AverageRoughness(data.roughness), data.ior, F0,
                    shadingN, wo, u1, u2);
                return _FinalizeSubtreeSample(tree, nodeId, N, wo, sample);
            }
            if (data.scatterMode == Bsdf::ScatterMode::Transmission) {
                auto sample = Bsdf::SampleGGXTransmission(
                    _AverageRoughness(data.roughness), data.ior,
                    data.tint, N, wo, u1, u2);
                if (sample.pdf <= 0.0f) {
                    return _SampleDeltaTransmission(
                        data.ior, data.tint, data.weight, N, wo);
                }
                sample.f *= data.weight;
                return _FinalizeSubtreeSample(tree, nodeId, N, wo, sample);
            }
            if (uChoice < fresnelProb) {
                Vec3f F0 = Vec3f(_SchlickFresnelScalar(data.ior, 1.0f)) * data.tint;
                auto sample = Bsdf::SampleGGXSpecular(
                    _AverageRoughness(data.roughness), data.ior, F0,
                    shadingN, wo, u1, u2);
                return _FinalizeSubtreeSample(tree, nodeId, N, wo, sample);
            }
            {
                auto sample = Bsdf::SampleGGXTransmission(
                    _AverageRoughness(data.roughness), data.ior,
                    data.tint, N, wo, u1, u2);
                if (sample.pdf <= 0.0f) {
                    return _SampleDeltaTransmission(
                        data.ior, data.tint, data.weight, N, wo);
                }
                sample.f *= data.weight;
                return _FinalizeSubtreeSample(tree, nodeId, N, wo, sample);
            }
        } else if constexpr (std::is_same_v<T, Bsdf::ConductorData>) {
            auto sample = Bsdf::SampleGGXSpecular(
                _AverageRoughness(data.roughness), 1.5f,
                _ConductorF0(data.ior, data.extinction), N, wo, u1, u2);
            return _FinalizeSubtreeSample(tree, nodeId, N, wo, sample);
        } else if constexpr (std::is_same_v<T, Bsdf::GeneralizedSchlickData>) {
            float fresnelProb = _Clamp01(_Luminance(_GeneralizedSchlickFresnel(
                data.color0, data.color90, data.exponent,
                std::max(std::abs(Dot(N, wo)), _kEpsilon))));
            Vec3f shadingN = _FaceForwardNormal(N, wo);
            if (data.scatterMode == Bsdf::ScatterMode::Transmission) {
                float avgF0 = _Clamp01(_Luminance(_SaturateVec(data.color0)));
                float sqrtF0 = std::sqrt(std::max(avgF0, 0.01f));
                float ior = (1.0f + sqrtF0) / (1.0f - sqrtF0);
                auto sample = Bsdf::SampleGGXTransmission(
                    _AverageRoughness(data.roughness), ior,
                    Vec3f(1.0f), N, wo, u1, u2);
                if (sample.pdf <= 0.0f) {
                    return _SampleDeltaTransmission(
                        ior, Vec3f(1.0f), data.weight, N, wo);
                }
                sample.f *= data.weight;
                return _FinalizeSubtreeSample(tree, nodeId, N, wo, sample);
            }
            if (data.scatterMode == Bsdf::ScatterMode::ReflectionTransmission &&
                uChoice >= fresnelProb) {
                float avgF0 = _Clamp01(_Luminance(_SaturateVec(data.color0)));
                float sqrtF0 = std::sqrt(std::max(avgF0, 0.01f));
                float ior = (1.0f + sqrtF0) / (1.0f - sqrtF0);
                auto sample = Bsdf::SampleGGXTransmission(
                    _AverageRoughness(data.roughness), ior,
                    Vec3f(1.0f), N, wo, u1, u2);
                if (sample.pdf <= 0.0f) {
                    return _SampleDeltaTransmission(
                        ior, Vec3f(1.0f), data.weight, N, wo);
                }
                sample.f *= data.weight;
                return _FinalizeSubtreeSample(tree, nodeId, N, wo, sample);
            }
            auto sample = Bsdf::SampleGGXSpecular(
                _AverageRoughness(data.roughness), 1.5f, data.color0,
                shadingN, wo, u1, u2);
            return _FinalizeSubtreeSample(tree, nodeId, N, wo, sample);
        } else if constexpr (std::is_same_v<T, Bsdf::SheenData>) {
            auto sample = Bsdf::SampleLambertian(data.color * data.weight, N, wo, u1, u2);
            return _FinalizeSubtreeSample(tree, nodeId, N, wo, sample);
        } else if constexpr (std::is_same_v<T, Bsdf::UnsupportedData>) {
            return Bsdf::BsdfSample{Vec3f(0.0f), Vec3f(0.0f), 0.0f, false};
        } else if constexpr (std::is_same_v<T, Bsdf::MixData>) {
            float mix = _Clamp01(data.mix);
            bool chooseFg = (uChoice < mix);
            Bsdf::NodeId chosen = chooseFg ? data.fg : data.bg;
            float chooseProb = chooseFg ? mix : (1.0f - mix);
            float remapped = (uChoice < mix)
                ? (mix > 0.0f ? uChoice / mix : 0.0f)
                : ((1.0f - mix) > 0.0f ? (uChoice - mix) / (1.0f - mix) : 0.0f);
            auto sample = _SampleNode(tree, chosen, N, wo, u1, u2, remapped);
            if (sample.isSpecular && sample.pdf > 0.0f && chooseProb > 0.0f) {
                sample.f /= chooseProb;
            }
            return _FinalizeSubtreeSample(tree, nodeId, N, wo, sample);
        } else if constexpr (std::is_same_v<T, Bsdf::LayerData>) {
            float topWeight = _ApproxWeight(tree, data.top, N, wo);
            float baseWeight = _ApproxWeight(tree, data.base, N, wo) *
                _Luminance(_EvalThroughput(tree, data.top, N, wo));
            float total = topWeight + baseWeight;
            if (total <= 0.0f) {
                return Bsdf::BsdfSample{Vec3f(0.0f), Vec3f(0.0f), 0.0f, false};
            }
            float pTop = topWeight / total;
            bool chooseTop = (uChoice < pTop);
            Bsdf::NodeId chosen = chooseTop ? data.top : data.base;
            float chooseProb = chooseTop ? pTop : (1.0f - pTop);
            float remapped = (uChoice < pTop)
                ? (pTop > 0.0f ? uChoice / pTop : 0.0f)
                : ((1.0f - pTop) > 0.0f ? (uChoice - pTop) / (1.0f - pTop) : 0.0f);
            auto sample = _SampleNode(tree, chosen, N, wo, u1, u2, remapped);
            if (sample.isSpecular && sample.pdf > 0.0f) {
                if (!chooseTop) {
                    sample.f = CompMul(
                        sample.f, _EvalThroughput(tree, data.top, N, wo));
                }
                if (chooseProb > 0.0f) {
                    sample.f /= chooseProb;
                }
            }
            return _FinalizeSubtreeSample(tree, nodeId, N, wo, sample);
        } else if constexpr (std::is_same_v<T, Bsdf::AddData>) {
            float w1 = _ApproxWeight(tree, data.in1, N, wo);
            float w2 = _ApproxWeight(tree, data.in2, N, wo);
            float total = w1 + w2;
            if (total <= 0.0f) {
                return Bsdf::BsdfSample{Vec3f(0.0f), Vec3f(0.0f), 0.0f, false};
            }
            float p1 = w1 / total;
            bool chooseFirst = (uChoice < p1);
            Bsdf::NodeId chosen = chooseFirst ? data.in1 : data.in2;
            float chooseProb = chooseFirst ? p1 : (1.0f - p1);
            float remapped = (uChoice < p1)
                ? (p1 > 0.0f ? uChoice / p1 : 0.0f)
                : ((1.0f - p1) > 0.0f ? (uChoice - p1) / (1.0f - p1) : 0.0f);
            auto sample = _SampleNode(tree, chosen, N, wo, u1, u2, remapped);
            if (sample.isSpecular && sample.pdf > 0.0f && chooseProb > 0.0f) {
                sample.f /= chooseProb;
            }
            return _FinalizeSubtreeSample(tree, nodeId, N, wo, sample);
        } else if constexpr (std::is_same_v<T, Bsdf::MultiplyData>) {
            auto sample = _SampleNode(tree, data.input, N, wo, u1, u2, uChoice);
            if (sample.isSpecular && sample.pdf > 0.0f) {
                sample.f = CompMul(data.weight, sample.f);
            }
            return _FinalizeSubtreeSample(tree, nodeId, N, wo, sample);
        } else {
            return Bsdf::BsdfSample{Vec3f(0.0f), Vec3f(0.0f), 0.0f, false};
        }
    }, node->data);
}

}  // namespace

Vec3f
Bsdf::EvalLambertian(
    const Vec3f& baseColor,
    const Vec3f& /*N*/,
    const Vec3f& /*wi*/,
    const Vec3f& /*wo*/)
{
    return baseColor * kInvPi;
}

Vec3f
Bsdf::EvalGGXSpecular(
    float roughness,
    float /*ior*/,
    const Vec3f& specularColor,
    const Vec3f& N,
    const Vec3f& wi,
    const Vec3f& wo)
{
    float alpha = _RoughnessToAlpha(roughness);
    float NdotL = std::max(Dot(N, wi), 0.0f);
    float NdotV = std::max(Dot(N, wo), _kEpsilon);
    if (NdotL <= 0.0f || NdotV <= 0.0f) {
        return Vec3f(0.0f);
    }
    Vec3f H = (wi + wo).normalized();
    float NdotH = std::max(Dot(N, H), 0.0f);
    float VdotH = std::max(Dot(wo, H), 0.0f);
    float D = _GGX_D(alpha, NdotH);
    float V = _GGX_V(alpha, NdotV, NdotL);
    Vec3f F = _SchlickFresnel(specularColor, VdotH);
    return _SafeVec(CompMul(F, Vec3f(D * V)));
}

Vec3f
Bsdf::EvalGGXTransmission(
    float roughness,
    float ior,
    const Vec3f& transmissionColor,
    const Vec3f& N,
    const Vec3f& wi,
    const Vec3f& wo)
{
    float alpha = _RoughnessToAlpha(roughness);

    _Frame frame = _Frame::FromNormal(N);
    Vec3f woLocal = frame.ToLocal(wo);
    Vec3f wiLocal = frame.ToLocal(wi);

    float cosThetaO = std::abs(woLocal[2]);
    float cosThetaI = std::abs(wiLocal[2]);
    if (cosThetaO < _kEpsilon || cosThetaI < _kEpsilon) {
        return Vec3f(0.0f);
    }

    // etap = IOR of wi-side medium / IOR of wo-side medium
    // (pbrt convention: eta_t / eta_i)
    float etap = (woLocal[2] > 0.0f) ? ior : (1.0f / ior);

    // Generalized half-vector for refraction (Walter et al. 2007)
    Vec3f wm = (wiLocal * etap + woLocal);
    if (wm.length() < _kEpsilon) {
        return Vec3f(0.0f);
    }
    wm.normalize();
    if (wm[2] < 0.0f) {
        wm = -wm;
    }

    // Reject if wi and wo are on the same side of the microfacet
    if (Dot(wiLocal, wm) * Dot(woLocal, wm) > 0.0f) {
        return Vec3f(0.0f);
    }

    float NdotH = std::abs(wm[2]);
    float VdotH = std::abs(Dot(woLocal, wm));
    float LdotH = std::abs(Dot(wiLocal, wm));

    float fresnel = _SchlickFresnelScalar(ior, VdotH);
    float T = 1.0f - fresnel;

    float D = _GGX_D(alpha, NdotH);
    float G = _SmithG1(alpha, cosThetaO) * _SmithG1(alpha, cosThetaI);

    // Jacobian denominator uses signed dot products (Walter et al.)
    float denom = Dot(wiLocal, wm) + Dot(woLocal, wm) / etap;
    denom *= denom;
    if (denom < _kEpsilon) {
        return Vec3f(0.0f);
    }

    float btdf = T * D * G * VdotH * LdotH /
        (cosThetaO * cosThetaI * denom);

    return _SafeVec(transmissionColor * std::abs(btdf));
}

Vec3f
Bsdf::EvalSheen(
    const Vec3f& sheenColor,
    float roughness,
    const Vec3f& N,
    const Vec3f& wi,
    const Vec3f& wo)
{
    float alpha = _ClampRoughness(roughness);
    float NdotL = std::max(Dot(N, wi), 0.0f);
    float NdotV = std::max(Dot(N, wo), _kEpsilon);
    if (NdotL <= 0.0f || NdotV <= 0.0f) {
        return Vec3f(0.0f);
    }
    Vec3f H = (wi + wo).normalized();
    float NdotH = std::max(Dot(N, H), 0.0f);
    float D = _Charlie_D(alpha, NdotH);
    float V = _Ashikhmin_V(NdotV, NdotL);
    return _SafeVec(sheenColor * (D * V));
}

Vec3f
Bsdf::EvalCoat(
    float coatWeight,
    float coatRoughness,
    float coatIor,
    const Vec3f& N,
    const Vec3f& wi,
    const Vec3f& wo)
{
    if (coatWeight <= 0.0f) {
        return Vec3f(0.0f);
    }

    float alpha = _RoughnessToAlpha(coatRoughness);
    float NdotL = std::max(Dot(N, wi), 0.0f);
    float NdotV = std::max(Dot(N, wo), _kEpsilon);
    if (NdotL <= 0.0f || NdotV <= 0.0f) {
        return Vec3f(0.0f);
    }
    Vec3f H = (wi + wo).normalized();
    float NdotH = std::max(Dot(N, H), 0.0f);
    float VdotH = std::max(Dot(wo, H), 0.0f);
    float D = _GGX_D(alpha, NdotH);
    float V = _GGX_V(alpha, NdotV, NdotL);
    float F = _SchlickFresnelScalar(coatIor, VdotH);
    return Vec3f(coatWeight * D * V * F);
}

Vec3f
Bsdf::EvalSurface(
    const SurfaceClosure& closure,
    const Vec3f& N,
    const Vec3f& wi,
    const Vec3f& wo)
{
    Vec3f f = closure.HasBsdfTree()
        ? _EvalNode(closure.bsdfTree, closure.bsdfTree.root, N, wi, wo)
        : _EvalLegacySurface(closure, N, wi, wo);
    return _SafeVec(f * closure.presence);
}

Bsdf::BsdfSample
Bsdf::SampleLambertian(
    const Vec3f& baseColor,
    const Vec3f& N,
    const Vec3f& /*wo*/,
    float u1, float u2)
{
    _Frame frame = _Frame::FromNormal(N);
    Vec3f wiLocal = _SampleCosineHemisphere(u1, u2);
    Vec3f wi = frame.ToWorld(wiLocal);
    float pdf = _CosineHemispherePdf(wiLocal[2]);
    return BsdfSample{wi, baseColor * kInvPi, pdf, false};
}

float
Bsdf::PdfLambertian(const Vec3f& N, const Vec3f& wi)
{
    return _CosineHemispherePdf(Dot(N, wi));
}

Bsdf::BsdfSample
Bsdf::SampleGGXSpecular(
    float roughness,
    float /*ior*/,
    const Vec3f& specularColor,
    const Vec3f& N,
    const Vec3f& wo,
    float u1, float u2)
{
    float alpha = _RoughnessToAlpha(roughness);

    _Frame frame = _Frame::FromNormal(N);
    Vec3f woLocal = frame.ToLocal(wo);
    if (woLocal[2] <= 0.0f) {
        return BsdfSample{Vec3f(0.0f), Vec3f(0.0f), 0.0f, false};
    }

    Vec3f wmLocal = _SampleGGX_VNDF(woLocal, alpha, u1, u2);
    Vec3f wiLocal = 2.0f * Dot(woLocal, wmLocal) * wmLocal - woLocal;
    if (wiLocal[2] <= 0.0f) {
        return BsdfSample{Vec3f(0.0f), Vec3f(0.0f), 0.0f, false};
    }

    Vec3f wi = frame.ToWorld(wiLocal);
    float NdotL = wiLocal[2];
    float NdotV = woLocal[2];
    float NdotH = wmLocal[2];
    float VdotH = std::max(Dot(woLocal, wmLocal), 0.0f);

    float D = _GGX_D(alpha, NdotH);
    float V = _GGX_V(alpha, NdotV, NdotL);
    Vec3f F = _SchlickFresnel(specularColor, VdotH);
    Vec3f f = CompMul(F, Vec3f(D * V));
    float pdf = _PdfGGX_VNDF(woLocal, wmLocal, alpha);

    return BsdfSample{wi, _SafeVec(f), std::max(pdf, 0.0f), false};
}

float
Bsdf::PdfGGXSpecular(
    float roughness,
    const Vec3f& N,
    const Vec3f& wi,
    const Vec3f& wo)
{
    float alpha = _RoughnessToAlpha(roughness);
    _Frame frame = _Frame::FromNormal(N);
    Vec3f woLocal = frame.ToLocal(wo);
    Vec3f wiLocal = frame.ToLocal(wi);

    if (woLocal[2] <= 0.0f || wiLocal[2] <= 0.0f) {
        return 0.0f;
    }

    Vec3f wmLocal = (woLocal + wiLocal).normalized();
    if (wmLocal[2] <= 0.0f) {
        return 0.0f;
    }

    return _PdfGGX_VNDF(woLocal, wmLocal, alpha);
}

Bsdf::BsdfSample
Bsdf::SampleGGXTransmission(
    float roughness,
    float ior,
    const Vec3f& transmissionColor,
    const Vec3f& N,
    const Vec3f& wo,
    float u1, float u2)
{
    float alpha = _RoughnessToAlpha(roughness);

    _Frame frame = _Frame::FromNormal(N);
    Vec3f woLocal = frame.ToLocal(wo);
    if (std::abs(woLocal[2]) < _kEpsilon) {
        return BsdfSample{Vec3f(0.0f), Vec3f(0.0f), 0.0f, false};
    }

    // Flip to upper hemisphere for VNDF sampling
    bool flipN = woLocal[2] < 0.0f;
    Vec3f woFlipped = flipN ? -woLocal : woLocal;

    Vec3f wmLocal = _SampleGGX_VNDF(woFlipped, alpha, u1, u2);
    if (flipN) {
        wmLocal = -wmLocal;
    }

    // Determine eta (incident / transmitted)
    float eta = (woLocal[2] > 0.0f) ? (1.0f / ior) : ior;

    // Refract through the microfacet
    float cosI = Dot(woLocal, wmLocal);
    float sin2T = eta * eta * (1.0f - cosI * cosI);
    if (sin2T >= 1.0f) {
        // Total internal reflection
        return BsdfSample{Vec3f(0.0f), Vec3f(0.0f), 0.0f, false};
    }

    float cosT = std::sqrt(1.0f - sin2T);
    // Ensure cosT has opposite sign from cosI for transmission
    if (cosI > 0.0f) {
        cosT = -cosT;
    }
    Vec3f wiLocal = -eta * woLocal + (eta * cosI + cosT) * wmLocal;
    wiLocal.normalize();

    // Reject if both directions are on the same side
    if (wiLocal[2] * woLocal[2] > 0.0f) {
        return BsdfSample{Vec3f(0.0f), Vec3f(0.0f), 0.0f, false};
    }

    Vec3f wi = frame.ToWorld(wiLocal);

    // Evaluate BTDF and PDF
    Vec3f f = EvalGGXTransmission(roughness, ior, transmissionColor, N, wi, wo);
    float pdf = PdfGGXTransmission(roughness, ior, N, wi, wo);

    if (pdf < _kEpsilon) {
        return BsdfSample{Vec3f(0.0f), Vec3f(0.0f), 0.0f, false};
    }

    BsdfSample sample{wi, _SafeVec(f), pdf, false};
    sample.eta = eta;
    return sample;
}

float
Bsdf::PdfGGXTransmission(
    float roughness,
    float ior,
    const Vec3f& N,
    const Vec3f& wi,
    const Vec3f& wo)
{
    float alpha = _RoughnessToAlpha(roughness);

    _Frame frame = _Frame::FromNormal(N);
    Vec3f woLocal = frame.ToLocal(wo);
    Vec3f wiLocal = frame.ToLocal(wi);

    // Transmission requires wi and wo on opposite sides
    if (wiLocal[2] * woLocal[2] > 0.0f) {
        return 0.0f;
    }

    float cosThetaO = std::abs(woLocal[2]);
    if (cosThetaO < _kEpsilon) {
        return 0.0f;
    }

    float etap = (woLocal[2] > 0.0f) ? ior : (1.0f / ior);

    // Generalized half-vector
    Vec3f wm = (wiLocal * etap + woLocal);
    if (wm.length() < _kEpsilon) {
        return 0.0f;
    }
    wm.normalize();
    if (wm[2] < 0.0f) {
        wm = -wm;
    }

    float VdotH = std::abs(Dot(woLocal, wm));
    float LdotH = std::abs(Dot(wiLocal, wm));

    // VNDF PDF for the half-vector
    float G1o = _SmithG1(alpha, cosThetaO);
    float NdotH = std::abs(wm[2]);
    float D = _GGX_D(alpha, NdotH);
    float pdfWm = G1o * D * VdotH / cosThetaO;

    // Jacobian for transmission half-vector change of variables
    // Uses signed dot products (Walter et al. 2007)
    float denom = Dot(wiLocal, wm) + Dot(woLocal, wm) / etap;
    denom *= denom;
    if (denom < _kEpsilon) {
        return 0.0f;
    }
    float dwm_dwi = LdotH / denom;

    return std::max(pdfWm * dwm_dwi, 0.0f);
}

Bsdf::BsdfSample
Bsdf::SampleSurface(
    const SurfaceClosure& closure,
    const Vec3f& N,
    const Vec3f& wo,
    float u1, float u2, float uLobe)
{
    if (closure.HasBsdfTree()) {
        auto sample = _SampleNode(closure.bsdfTree, closure.bsdfTree.root,
                                  N, wo, u1, u2, uLobe);
        if (!sample.isSpecular) {
            sample.f *= closure.presence;
        }
        return sample;
    }
    return _SampleLegacySurface(closure, N, wo, u1, u2, uLobe);
}

float
Bsdf::PdfSurface(
    const SurfaceClosure& closure,
    const Vec3f& N,
    const Vec3f& wi,
    const Vec3f& wo)
{
    if (closure.HasBsdfTree()) {
        return _PdfNode(closure.bsdfTree, closure.bsdfTree.root, N, wi, wo);
    }
    return _PdfLegacySurface(closure, N, wi, wo);
}

}  // namespace mxcpp
