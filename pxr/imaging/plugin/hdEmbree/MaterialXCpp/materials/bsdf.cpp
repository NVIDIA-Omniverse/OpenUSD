//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "bsdf.h"

#include "../spectral.h"
#include "../nodes/helpers/colorHelpers.h"
#include "../nodes/helpers/mathHelpers.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <variant>

namespace mxcpp {

namespace {

constexpr float _kEpsilon = 1e-7f;
constexpr int _kThinFilmAiryIterations = 2;

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
_ResolveDielectricIor(
    const Bsdf::DielectricData& data,
    float heroWavelengthNm)
{
    if (data.dispersionAbbe > 0.0f && heroWavelengthNm > 0.0f) {
        return std::max(
            Spectral::CauchyDispersionIOR(
                data.dispersionAbbe, data.ior, heroWavelengthNm),
            1.0f);
    }
    return std::max(data.ior, 1.0f);
}

inline Vec2f
_ClampAlpha(const Vec2f& alpha)
{
    return Vec2f(
        std::clamp(alpha[0], 1.0e-5f, 1.0f),
        std::clamp(alpha[1], 1.0e-5f, 1.0f));
}

inline float
_AverageAlphaAsRoughness(const Vec2f& alpha)
{
    const float clampedX = std::clamp(alpha[0], 1.0e-5f, 1.0f);
    const float clampedY = std::clamp(alpha[1], 1.0e-5f, 1.0f);
    const float avgAlpha = std::sqrt(clampedX * clampedY);
    return std::sqrt(avgAlpha);
}

inline bool
_IsEffectivelyIsotropic(const Vec2f& roughness)
{
    return std::abs(roughness[0] - roughness[1]) < 1.0e-6f;
}

inline bool
_IsEffectivelyDeltaAlpha(const Vec2f& alpha)
{
    constexpr float kDeltaAlphaThreshold = 1.1e-5f;
    return std::max(alpha[0], alpha[1]) <= kDeltaAlphaThreshold;
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
    const Vec3f& color82,
    const Vec3f& color90,
    float exponent,
    float cosTheta)
{
    constexpr float kCosThetaMax = 1.0f / 7.0f;
    const float clampedExponent = std::max(exponent, 0.0f);
    const float x = _Clamp01(cosTheta);
    const float oneMinusX = _Clamp01(1.0f - x);
    const float baseMix =
        std::pow(1.0f - kCosThetaMax, clampedExponent);
    const float factor = 1.0f /
        (kCosThetaMax * std::pow(1.0f - kCosThetaMax, 6.0f));
    const Vec3f a = CompMul(
        _LerpVec(color0, color90, baseMix),
        (Vec3f(1.0f) - color82) * factor);
    const Vec3f result =
        _LerpVec(color0, color90, std::pow(oneMinusX, clampedExponent)) -
        a * x * std::pow(oneMinusX, 6.0f);
    return Vec3f(
        std::max(result[0], 0.0f),
        std::max(result[1], 0.0f),
        std::max(result[2], 0.0f));
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

inline float
_GGX_G(float alpha, float NdotV, float NdotL)
{
    return _SmithG1(alpha, NdotV) * _SmithG1(alpha, NdotL);
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

    static _Frame FromNormalAndTangent(const Vec3f& n, const Vec3f& tangent) {
        const Vec3f projectedTangent = tangent - n * Dot(tangent, n);
        if (projectedTangent.length() < _kEpsilon) {
            return FromNormal(n);
        }

        _Frame f;
        f.N = n;
        f.T = projectedTangent.normalized();
        f.B = Cross(f.N, f.T);
        if (f.B.length() < _kEpsilon) {
            return FromNormal(n);
        }
        f.B.normalize();
        f.T = Cross(f.B, f.N);
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
_AbsCosTheta(const Vec3f& w)
{
    return std::abs(w[2]);
}

inline float
_Tan2Theta(const Vec3f& w)
{
    const float cosTheta2 = w[2] * w[2];
    if (cosTheta2 <= _kEpsilon) {
        return std::numeric_limits<float>::infinity();
    }
    return std::max(0.0f, 1.0f - cosTheta2) / cosTheta2;
}

inline float
_GGX_D_Anisotropic(const Vec2f& alpha, const Vec3f& wmLocal)
{
    const float tan2Theta = _Tan2Theta(wmLocal);
    if (!std::isfinite(tan2Theta)) {
        return 0.0f;
    }

    const float cosTheta2 = wmLocal[2] * wmLocal[2];
    const float cosTheta4 = cosTheta2 * cosTheta2;
    if (cosTheta4 <= _kEpsilon) {
        return 0.0f;
    }

    const float sinTheta2 = std::max(0.0f, 1.0f - cosTheta2);
    float cosPhi2 = 1.0f;
    float sinPhi2 = 0.0f;
    if (sinTheta2 > _kEpsilon) {
        cosPhi2 = wmLocal[0] * wmLocal[0] / sinTheta2;
        sinPhi2 = wmLocal[1] * wmLocal[1] / sinTheta2;
    }

    const float e = tan2Theta *
        (cosPhi2 / (alpha[0] * alpha[0]) +
         sinPhi2 / (alpha[1] * alpha[1]));
    const float denom = kPi * alpha[0] * alpha[1] * cosTheta4 *
        (1.0f + e) * (1.0f + e);
    return (denom > _kEpsilon) ? 1.0f / denom : 0.0f;
}

inline float
_GGX_Lambda_Anisotropic(const Vec2f& alpha, const Vec3f& wLocal)
{
    const float tan2Theta = _Tan2Theta(wLocal);
    if (!std::isfinite(tan2Theta)) {
        return 0.0f;
    }

    const float sinTheta2 = std::max(0.0f, 1.0f - wLocal[2] * wLocal[2]);
    float cosPhi2 = 1.0f;
    float sinPhi2 = 0.0f;
    if (sinTheta2 > _kEpsilon) {
        cosPhi2 = wLocal[0] * wLocal[0] / sinTheta2;
        sinPhi2 = wLocal[1] * wLocal[1] / sinTheta2;
    }

    const float alpha2 =
        cosPhi2 * alpha[0] * alpha[0] +
        sinPhi2 * alpha[1] * alpha[1];
    return (std::sqrt(1.0f + alpha2 * tan2Theta) - 1.0f) * 0.5f;
}

inline float
_GGX_G1_Anisotropic(const Vec2f& alpha, const Vec3f& wLocal)
{
    return 1.0f / (1.0f + _GGX_Lambda_Anisotropic(alpha, wLocal));
}

inline float
_GGX_G_Anisotropic(const Vec2f& alpha,
                   const Vec3f& woLocal,
                   const Vec3f& wiLocal)
{
    return 1.0f / (1.0f +
        _GGX_Lambda_Anisotropic(alpha, woLocal) +
        _GGX_Lambda_Anisotropic(alpha, wiLocal));
}

Vec3f
_SampleGGX_VNDF_Anisotropic(const Vec3f& woLocal,
                            const Vec2f& alpha,
                            float u1,
                            float u2)
{
    Vec3f wh(alpha[0] * woLocal[0], alpha[1] * woLocal[1], woLocal[2]);
    const float whLen = wh.length();
    if (whLen < _kEpsilon) {
        return Vec3f(0.0f, 0.0f, 1.0f);
    }
    wh /= whLen;
    if (wh[2] < 0.0f) {
        wh = -wh;
    }

    const Vec3f T1 = (wh[2] < 0.99999f)
        ? Cross(Vec3f(0.0f, 0.0f, 1.0f), wh).normalized()
        : Vec3f(1.0f, 0.0f, 0.0f);
    const Vec3f T2 = Cross(wh, T1);

    const float r = std::sqrt(u1);
    const float phi = 2.0f * kPi * u2;
    float p1 = r * std::cos(phi);
    float p2 = r * std::sin(phi);

    const float h = std::sqrt(std::max(0.0f, 1.0f - p1 * p1));
    const float blend = (1.0f + wh[2]) * 0.5f;
    p2 = (1.0f - blend) * h + blend * p2;

    const float pz = std::sqrt(std::max(0.0f, 1.0f - p1 * p1 - p2 * p2));
    const Vec3f nh = T1 * p1 + T2 * p2 + wh * pz;

    Vec3f wm(alpha[0] * nh[0], alpha[1] * nh[1], std::max(1.0e-6f, nh[2]));
    wm.normalize();
    return wm;
}

float
_PdfGGX_VNDF_Anisotropic(const Vec3f& woLocal,
                         const Vec3f& wmLocal,
                         const Vec2f& alpha)
{
    const float cosThetaO = _AbsCosTheta(woLocal);
    if (cosThetaO <= _kEpsilon) {
        return 0.0f;
    }

    const float G1 = _GGX_G1_Anisotropic(alpha, woLocal);
    const float D = _GGX_D_Anisotropic(alpha, wmLocal);
    const float VdotH = std::max(std::abs(Dot(woLocal, wmLocal)), _kEpsilon);
    return G1 * D * VdotH / cosThetaO;
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

inline bool
_HasThinFilm(float weight, float thickness, float ior)
{
    return weight > _kEpsilon &&
           thickness > _kEpsilon &&
           ior > (1.0f + _kEpsilon);
}

enum class _ThinFilmModel
{
    Dielectric,
    Conductor,
    Schlick
};

struct _ThinFilmParams
{
    _ThinFilmModel model = _ThinFilmModel::Dielectric;
    Vec3f ior = Vec3f(1.0f);
    Vec3f extinction = Vec3f(0.0f);
    Vec3f tint = Vec3f(1.0f);
    Vec3f F0 = Vec3f(0.0f);
    Vec3f F82 = Vec3f(0.0f);
    Vec3f F90 = Vec3f(1.0f);
    float exponent = 5.0f;
};

inline Vec3f
_SqrtVec(const Vec3f& v)
{
    return Vec3f(
        std::sqrt(std::max(v[0], 0.0f)),
        std::sqrt(std::max(v[1], 0.0f)),
        std::sqrt(std::max(v[2], 0.0f)));
}

inline Vec3f
_CosVec(const Vec3f& v)
{
    return Vec3f(std::cos(v[0]), std::cos(v[1]), std::cos(v[2]));
}

inline Vec3f
_ExpVec(const Vec3f& v)
{
    return Vec3f(std::exp(v[0]), std::exp(v[1]), std::exp(v[2]));
}

inline Vec3f
_SquareVec(const Vec3f& v)
{
    return CompMul(v, v);
}

inline Vec3f
_MaxVec(const Vec3f& v, float minimum)
{
    return Vec3f(
        std::max(v[0], minimum),
        std::max(v[1], minimum),
        std::max(v[2], minimum));
}

inline Vec3f
_F0ToIor(const Vec3f& F0)
{
    const Vec3f sqrtF0 = _SqrtVec(_SaturateVec(Vec3f(
        std::clamp(F0[0], 0.01f, 0.99f),
        std::clamp(F0[1], 0.01f, 0.99f),
        std::clamp(F0[2], 0.01f, 0.99f))));
    return CompDiv(Vec3f(1.0f) + sqrtF0, Vec3f(1.0f) - sqrtF0);
}

inline Vec2f
_FresnelDielectricPolarized(float cosTheta, float ior)
{
    const float cosTheta2 = _Clamp01(cosTheta) * _Clamp01(cosTheta);
    const float sinTheta2 = 1.0f - cosTheta2;

    const float t0 = std::max(ior * ior - sinTheta2, 0.0f);
    const float t1 = t0 + cosTheta2;
    const float t2 = 2.0f * std::sqrt(t0) * _Clamp01(cosTheta);
    const float Rs = (t1 - t2) / std::max(t1 + t2, _kEpsilon);

    const float t3 = cosTheta2 * t0 + sinTheta2 * sinTheta2;
    const float t4 = t2 * sinTheta2;
    const float Rp = Rs * (t3 - t4) / std::max(t3 + t4, _kEpsilon);

    return Vec2f(Rp, Rs);
}

inline void
_FresnelConductorPolarized(
    float cosTheta,
    const Vec3f& n,
    const Vec3f& k,
    Vec3f* Rp,
    Vec3f* Rs)
{
    const float clampedCos = _Clamp01(cosTheta);
    const float cosTheta2 = clampedCos * clampedCos;
    const float sinTheta2 = 1.0f - cosTheta2;
    const Vec3f n2 = _SquareVec(n);
    const Vec3f k2 = _SquareVec(k);

    const Vec3f t0 = n2 - k2 - Vec3f(sinTheta2);
    const Vec3f a2plusb2 = _SqrtVec(t0 * t0 + 4.0f * CompMul(n2, k2));
    const Vec3f t1 = a2plusb2 + Vec3f(cosTheta2);
    const Vec3f a = _SqrtVec(_MaxVec(0.5f * (a2plusb2 + t0), 0.0f));
    const Vec3f t2 = 2.0f * a * clampedCos;
    *Rs = CompDiv(t1 - t2, t1 + t2);

    const Vec3f t3 = a2plusb2 * cosTheta2 + Vec3f(sinTheta2 * sinTheta2);
    const Vec3f t4 = t2 * sinTheta2;
    *Rp = CompMul(*Rs, CompDiv(t3 - t4, t3 + t4));
}

inline void
_FresnelConductorPhasePolarized(
    float cosTheta,
    float eta1,
    const Vec3f& eta2,
    const Vec3f& kappa2,
    Vec3f* phiP,
    Vec3f* phiS)
{
    const Vec3f k2 = CompDiv(kappa2, eta2);
    const Vec3f sinThetaSqr(1.0f - cosTheta * cosTheta);
    const Vec3f A =
        CompMul(eta2, eta2) * (Vec3f(1.0f) - CompMul(k2, k2)) -
        eta1 * eta1 * sinThetaSqr;
    const Vec3f B = _SqrtVec(CompMul(A, A) +
                             4.0f * CompMul(CompMul(eta2, eta2), CompMul(k2, k2)));
    const Vec3f U = _SqrtVec((A + B) * 0.5f);
    const Vec3f V = _MaxVec(_SqrtVec((B - A) * 0.5f), 0.0f);

    *phiS = Vec3f(
        std::atan2(2.0f * eta1 * V[0] * cosTheta,
                   U[0] * U[0] + V[0] * V[0] - eta1 * eta1 * cosTheta * cosTheta),
        std::atan2(2.0f * eta1 * V[1] * cosTheta,
                   U[1] * U[1] + V[1] * V[1] - eta1 * eta1 * cosTheta * cosTheta),
        std::atan2(2.0f * eta1 * V[2] * cosTheta,
                   U[2] * U[2] + V[2] * V[2] - eta1 * eta1 * cosTheta * cosTheta));

    const Vec3f eta2Squared = CompMul(eta2, eta2);
    const Vec3f oneMinusK2 = Vec3f(1.0f) - CompMul(k2, k2);
    const Vec3f onePlusK2 = Vec3f(1.0f) + CompMul(k2, k2);
    *phiP = Vec3f(
        std::atan2(
            2.0f * eta1 * eta2Squared[0] * cosTheta *
                (2.0f * k2[0] * U[0] - oneMinusK2[0] * V[0]),
            eta2Squared[0] * eta2Squared[0] * onePlusK2[0] * onePlusK2[0] *
                    cosTheta * cosTheta -
                eta1 * eta1 * (U[0] * U[0] + V[0] * V[0])),
        std::atan2(
            2.0f * eta1 * eta2Squared[1] * cosTheta *
                (2.0f * k2[1] * U[1] - oneMinusK2[1] * V[1]),
            eta2Squared[1] * eta2Squared[1] * onePlusK2[1] * onePlusK2[1] *
                    cosTheta * cosTheta -
                eta1 * eta1 * (U[1] * U[1] + V[1] * V[1])),
        std::atan2(
            2.0f * eta1 * eta2Squared[2] * cosTheta *
                (2.0f * k2[2] * U[2] - oneMinusK2[2] * V[2]),
            eta2Squared[2] * eta2Squared[2] * onePlusK2[2] * onePlusK2[2] *
                    cosTheta * cosTheta -
                eta1 * eta1 * (U[2] * U[2] + V[2] * V[2])));
}

inline Vec3f
_EvalSensitivity(float opd, const Vec3f& shift)
{
    const float phase = 2.0f * kPi * opd;
    const Vec3f val(5.4856e-13f, 4.4201e-13f, 5.2481e-13f);
    const Vec3f pos(1.6810e+06f, 1.7953e+06f, 2.2084e+06f);
    const Vec3f var(4.3278e+09f, 9.3046e+09f, 6.6121e+09f);
    const Vec3f phaseVec = pos * phase + shift;
    const Vec3f gaussian = _ExpVec(-var * phase * phase);
    Vec3f xyz = CompMul(
        CompMul(val, _SqrtVec(2.0f * kPi * var)),
        CompMul(_CosVec(phaseVec), gaussian));
    xyz[0] += 9.7470e-14f * std::sqrt(2.0f * kPi * 4.5282e+09f) *
        std::cos(2.2399e+06f * phase + shift[0]) *
        std::exp(-4.5282e+09f * phase * phase);
    return xyz * (1.0f / 1.0685e-7f);
}

inline Vec3f
_XYZToRGB(const Vec3f& xyz)
{
    const Vec3f redVec(2.3706743f, -0.9000405f, -0.4706338f);
    const Vec3f grnVec(-0.5138850f, 1.4253036f, 0.0885814f);
    const Vec3f bluVec(0.0052982f, -0.0146949f, 1.0093968f);
    return Vec3f(
        std::max(Dot(redVec, xyz), 0.0f),
        std::max(Dot(grnVec, xyz), 0.0f),
        std::max(Dot(bluVec, xyz), 0.0f));
}

inline Vec3f
_ThinFilmAiryReflectance(
    float cosTheta,
    float thinFilmThickness,
    float thinFilmIor,
    const _ThinFilmParams& params)
{
    const float clampedCos = _Clamp01(cosTheta);
    const float eta1 = 1.0f;
    const float eta2 = std::max(thinFilmIor, eta1);
    const Vec3f eta3 = (params.model == _ThinFilmModel::Schlick)
        ? _F0ToIor(params.F0)
        : params.ior;
    const Vec3f kappa3 = (params.model == _ThinFilmModel::Schlick)
        ? Vec3f(0.0f)
        : params.extinction;

    const float sinTheta2 = std::max(1.0f - clampedCos * clampedCos, 0.0f);
    const float eta = eta1 / eta2;
    const float cosThetaTSqr = 1.0f - sinTheta2 * eta * eta;
    const float cosThetaT = (cosThetaTSqr > 0.0f) ? std::sqrt(cosThetaTSqr) : 0.0f;

    Vec2f R12 = _FresnelDielectricPolarized(clampedCos, eta2 / eta1);
    if (cosThetaT <= 0.0f) {
        R12 = Vec2f(1.0f, 1.0f);
    }
    const Vec2f T121 = Vec2f(1.0f, 1.0f) - R12;

    Vec3f R23p(0.0f), R23s(0.0f);
    Vec3f phi23p(0.0f), phi23s(0.0f);
    if (params.model == _ThinFilmModel::Schlick) {
        const Vec3f f = _GeneralizedSchlickFresnel(
            params.F0, params.F82, params.F90, params.exponent, cosThetaT);
        R23p = f * 0.5f;
        R23s = f * 0.5f;
        phi23p = Vec3f(
            eta3[0] < eta2 ? kPi : 0.0f,
            eta3[1] < eta2 ? kPi : 0.0f,
            eta3[2] < eta2 ? kPi : 0.0f);
        phi23s = phi23p;
    } else {
        _FresnelConductorPolarized(
            cosThetaT, CompDiv(eta3, Vec3f(eta2)), CompDiv(kappa3, Vec3f(eta2)),
            &R23p, &R23s);
        _FresnelConductorPhasePolarized(
            cosThetaT, eta2, eta3, kappa3, &phi23p, &phi23s);
    }

    const float cosB = std::cos(std::atan(eta2 / eta1));
    const Vec2f phi21(clampedCos < cosB ? 0.0f : kPi, kPi);
    const Vec3f r123p = _SqrtVec(_MaxVec(R23p * R12[0], 0.0f));
    const Vec3f r123s = _SqrtVec(_MaxVec(R23s * R12[1], 0.0f));

    const float distMeters = thinFilmThickness * 1.0e-9f;
    const float opd = 2.0f * eta2 * cosThetaT * distMeters;

    Vec3f I(0.0f);
    Vec3f Rs = (T121[0] * T121[0]) *
        CompDiv(R23p, Vec3f(1.0f) - R12[0] * R23p);
    I += Vec3f(R12[0]) + Rs;

    Vec3f Cm = Rs - Vec3f(T121[0]);
    for (int m = 1; m <= _kThinFilmAiryIterations; ++m) {
        Cm = CompMul(Cm, r123p);
        I += CompMul(
            Cm,
            2.0f * _EvalSensitivity(
                static_cast<float>(m) * opd,
                static_cast<float>(m) * (phi23p + Vec3f(phi21[0]))));
    }

    Vec3f Rp = (T121[1] * T121[1]) *
        CompDiv(R23s, Vec3f(1.0f) - R12[1] * R23s);
    I += Vec3f(R12[1]) + Rp;

    Cm = Rp - Vec3f(T121[1]);
    for (int m = 1; m <= _kThinFilmAiryIterations; ++m) {
        Cm = CompMul(Cm, r123s);
        I += CompMul(
            Cm,
            2.0f * _EvalSensitivity(
                static_cast<float>(m) * opd,
                static_cast<float>(m) * (phi23s + Vec3f(phi21[1]))));
    }

    return _SaturateVec(CompMul(_XYZToRGB(I * 0.5f), params.tint));
}

inline Vec3f
_ApplyThinFilm(
    const Vec3f& baseReflectance,
    float cosTheta,
    float thinFilmWeight,
    float thinFilmThickness,
    float thinFilmIor,
    const _ThinFilmParams& params)
{
    if (!_HasThinFilm(thinFilmWeight, thinFilmThickness, thinFilmIor)) {
        return baseReflectance;
    }

    const Vec3f thinFilmReflectance = _ThinFilmAiryReflectance(
        cosTheta, thinFilmThickness, thinFilmIor, params);
    return _SaturateVec(_LerpVec(
        baseReflectance,
        thinFilmReflectance,
        _Clamp01(thinFilmWeight)));
}

inline float
_ReflectionFresnelCosTheta(const Vec3f& wi, const Vec3f& wo)
{
    const Vec3f halfVector = wi + wo;
    if (halfVector.length() < _kEpsilon) {
        return 1.0f;
    }
    return std::max(Dot(wo, halfVector.normalized()), 0.0f);
}

inline float
_TransmissionFresnelCosTheta(
    float ior,
    const Vec3f& N,
    const Vec3f& wi,
    const Vec3f& wo)
{
    _Frame frame = _Frame::FromNormal(N);
    const Vec3f woLocal = frame.ToLocal(wo);
    const Vec3f wiLocal = frame.ToLocal(wi);
    const float etap = (woLocal[2] > 0.0f) ? ior : (1.0f / ior);

    Vec3f wm = wiLocal * etap + woLocal;
    if (wm.length() < _kEpsilon) {
        return std::max(std::abs(Dot(N, wo)), 0.0f);
    }
    wm.normalize();
    return std::abs(Dot(woLocal, wm));
}

inline Vec3f
_TransmissionScale(float baseReflectance, const Vec3f& finalReflectance)
{
    const float baseTransmission = std::max(1.0f - baseReflectance, 1.0e-4f);
    return _SafeVec((Vec3f(1.0f) - finalReflectance) *
                    (1.0f / baseTransmission));
}

inline Vec3f
_DielectricReflectionFresnelUntinted(
    const Bsdf::DielectricData& data,
    float cosTheta,
    float effectiveIor)
{
    float F0 = (effectiveIor - 1.0f) / (effectiveIor + 1.0f);
    F0 *= F0;
    const Vec3f baseReflectance = _SchlickFresnel(
        Vec3f(F0),
        cosTheta);
    _ThinFilmParams thinFilm;
    thinFilm.model = _ThinFilmModel::Dielectric;
    thinFilm.ior = Vec3f(std::max(effectiveIor, 1.0f));
    thinFilm.tint = Vec3f(1.0f);
    return _ApplyThinFilm(
        baseReflectance,
        cosTheta,
        data.thinFilmWeight,
        data.thinFilmThickness,
        data.thinFilmIor,
        thinFilm);
}

inline Vec3f
_DielectricReflectionFresnel(
    const Bsdf::DielectricData& data,
    float cosTheta,
    float effectiveIor)
{
    return CompMul(
        _DielectricReflectionFresnelUntinted(data, cosTheta, effectiveIor),
        _SafeVec(data.tint));
}

inline Vec3f
_ConductorReflectionFresnel(
    const Bsdf::ConductorData& data,
    float cosTheta)
{
    const Vec3f baseReflectance = _SchlickFresnel(
        _ConductorF0(data.ior, data.extinction),
        cosTheta);
    _ThinFilmParams thinFilm;
    thinFilm.model = _ThinFilmModel::Conductor;
    thinFilm.ior = _MaxVec(data.ior, 0.0f);
    thinFilm.extinction = _MaxVec(data.extinction, 0.0f);
    return _ApplyThinFilm(
        baseReflectance,
        cosTheta,
        data.thinFilmWeight,
        data.thinFilmThickness,
        data.thinFilmIor,
        thinFilm);
}

inline Vec3f
_GeneralizedSchlickReflectionFresnel(
    const Bsdf::GeneralizedSchlickData& data,
    float cosTheta)
{
    const Vec3f baseReflectance = _GeneralizedSchlickFresnel(
        data.color0,
        data.color82,
        data.color90,
        data.exponent,
        cosTheta);
    _ThinFilmParams thinFilm;
    thinFilm.model = _ThinFilmModel::Schlick;
    thinFilm.F0 = _SaturateVec(data.color0);
    thinFilm.F82 = _SaturateVec(data.color82);
    thinFilm.F90 = _SaturateVec(data.color90);
    thinFilm.exponent = data.exponent;
    return _ApplyThinFilm(
        baseReflectance,
        cosTheta,
        data.thinFilmWeight,
        data.thinFilmThickness,
        data.thinFilmIor,
        thinFilm);
}

inline Vec3f
_FaceForwardNormal(const Vec3f& N, const Vec3f& wo)
{
    return (Dot(N, wo) < 0.0f) ? -N : N;
}

inline Vec3f
_NormalizeOrFallback(const Vec3f& v, const Vec3f& fallback)
{
    const float length = v.length();
    if (length < _kEpsilon) {
        return fallback;
    }
    return v / length;
}

inline Vec3f
_ResolveReflectionNormal(const Bsdf::DielectricData& data,
                         const Vec3f& N,
                         const Vec3f& wo)
{
    if (!data.hasShadingNormal) {
        return _FaceForwardNormal(N, wo);
    }

    return _FaceForwardNormal(
        _NormalizeOrFallback(data.normal, _FaceForwardNormal(N, wo)),
        wo);
}

inline bool
_IsSameSide(const Vec3f& N, const Vec3f& wi, const Vec3f& wo)
{
    return Dot(N, wi) * Dot(N, wo) > 0.0f;
}

Vec3f
_EvalMicrofacetReflectionAnisotropic(
    const Vec2f& roughness,
    const Vec3f& tangent,
    const Vec3f& fresnel,
    const Vec3f& N,
    const Vec3f& wi,
    const Vec3f& wo)
{
    const _Frame frame = _Frame::FromNormalAndTangent(N, tangent);
    const Vec3f woLocal = frame.ToLocal(wo);
    const Vec3f wiLocal = frame.ToLocal(wi);
    const float cosThetaO = _AbsCosTheta(woLocal);
    const float cosThetaI = _AbsCosTheta(wiLocal);
    if (cosThetaI <= 0.0f || cosThetaO <= 0.0f ||
        wiLocal[2] <= 0.0f || woLocal[2] <= 0.0f) {
        return Vec3f(0.0f);
    }

    Vec3f wmLocal = wiLocal + woLocal;
    if (wmLocal.length() < _kEpsilon) {
        return Vec3f(0.0f);
    }
    wmLocal.normalize();
    if (wmLocal[2] < 0.0f) {
        wmLocal = -wmLocal;
    }

    const Vec2f alpha = _ClampAlpha(roughness);
    const float D = _GGX_D_Anisotropic(alpha, wmLocal);
    const float G = _GGX_G_Anisotropic(alpha, woLocal, wiLocal);
    return _SafeVec(CompMul(
        fresnel,
        Vec3f(D * G / std::max(4.0f * cosThetaI * cosThetaO, _kEpsilon))));
}

Vec3f
_EvalMicrofacetReflectionIsotropic(
    float alpha,
    const Vec3f& fresnel,
    const Vec3f& N,
    const Vec3f& wi,
    const Vec3f& wo)
{
    const float clampedAlpha = std::clamp(alpha, 1.0e-5f, 1.0f);
    const float NdotL = std::max(Dot(N, wi), 0.0f);
    const float NdotV = std::max(Dot(N, wo), _kEpsilon);
    if (NdotL <= 0.0f || NdotV <= 0.0f) {
        return Vec3f(0.0f);
    }

    Vec3f H = wi + wo;
    if (H.length() < _kEpsilon) {
        return Vec3f(0.0f);
    }
    H.normalize();
    const float NdotH = std::max(Dot(N, H), 0.0f);
    const float D = _GGX_D(clampedAlpha, NdotH);
    const float G = _GGX_G(clampedAlpha, NdotV, NdotL);
    return _SafeVec(CompMul(
        fresnel,
        Vec3f(D * G / std::max(4.0f * NdotL * NdotV, _kEpsilon))));
}

float
_PdfGGXSpecularAnisotropic(const Vec2f& roughness,
                           const Vec3f& tangent,
                           const Vec3f& N,
                           const Vec3f& wi,
                           const Vec3f& wo)
{
    const _Frame frame = _Frame::FromNormalAndTangent(N, tangent);
    const Vec3f woLocal = frame.ToLocal(wo);
    const Vec3f wiLocal = frame.ToLocal(wi);
    if (woLocal[2] <= 0.0f || wiLocal[2] <= 0.0f) {
        return 0.0f;
    }

    Vec3f wmLocal = woLocal + wiLocal;
    if (wmLocal.length() < _kEpsilon) {
        return 0.0f;
    }
    wmLocal.normalize();
    if (wmLocal[2] <= 0.0f) {
        return 0.0f;
    }

    const Vec2f alpha = _ClampAlpha(roughness);
    const float pdfWm =
        _PdfGGX_VNDF_Anisotropic(woLocal, wmLocal, alpha);
    const float VdotH = std::max(std::abs(Dot(woLocal, wmLocal)), _kEpsilon);
    return pdfWm / (4.0f * VdotH);
}

Bsdf::BsdfSample
_SampleGGXSpecularAnisotropic(const Vec2f& roughness,
                              const Vec3f& tangent,
                              const Vec3f& N,
                              const Vec3f& wo,
                              float u1,
                              float u2)
{
    const _Frame frame = _Frame::FromNormalAndTangent(N, tangent);
    const Vec3f woLocal = frame.ToLocal(wo);
    if (woLocal[2] <= 0.0f) {
        return Bsdf::BsdfSample{Vec3f(0.0f), Vec3f(0.0f), 0.0f, false};
    }

    const Vec2f alpha = _ClampAlpha(roughness);
    const Vec3f wmLocal = _SampleGGX_VNDF_Anisotropic(woLocal, alpha, u1, u2);
    const Vec3f wiLocal = 2.0f * Dot(woLocal, wmLocal) * wmLocal - woLocal;
    if (wiLocal[2] <= 0.0f) {
        return Bsdf::BsdfSample{Vec3f(0.0f), Vec3f(0.0f), 0.0f, false};
    }

    const Vec3f wi = frame.ToWorld(wiLocal);
    const float pdfWm =
        _PdfGGX_VNDF_Anisotropic(woLocal, wmLocal, alpha);
    const float VdotH = std::max(std::abs(Dot(woLocal, wmLocal)), _kEpsilon);
    return Bsdf::BsdfSample{
        wi,
        Vec3f(0.0f),
        pdfWm / (4.0f * VdotH),
        false};
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

inline Bsdf::BsdfSample
_ScaleDiscreteSpecularSample(
    Bsdf::BsdfSample sample,
    float selectionProb)
{
    if (sample.isSpecular && sample.pdf > 0.0f && selectionProb > 0.0f) {
        sample.f /= selectionProb;
    }
    return sample;
}

inline Bsdf::BsdfSample
_SampleDeltaDielectricReflection(
    const Bsdf::DielectricData& data,
    float effectiveIor,
    const Vec3f& shadingN,
    const Vec3f& wo)
{
    Vec3f wi = 2.0f * Dot(shadingN, wo) * shadingN - wo;
    wi.normalize();

    const float cosTheta =
        std::max(std::abs(Dot(shadingN, wo)), _kEpsilon);
    Bsdf::BsdfSample sample{
        wi,
        _DielectricReflectionFresnel(data, cosTheta, effectiveIor) *
            data.weight,
        1.0f,
        true
    };
    sample.eta = 1.0f;
    return sample;
}

inline Bsdf::BsdfSample
_SampleDeltaDielectricTransmission(
    const Bsdf::DielectricData& data,
    float effectiveIor,
    float fresnelCos,
    const Vec3f& N,
    const Vec3f& wo)
{
    auto sample = _SampleDeltaTransmission(
        effectiveIor, data.tint, data.weight, N, wo);
    const float baseReflectance =
        _SchlickFresnelScalar(effectiveIor, fresnelCos);
    sample.f = CompMul(
        sample.f,
        _TransmissionScale(
            baseReflectance,
            _DielectricReflectionFresnelUntinted(
                data, fresnelCos, effectiveIor)));
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
          const Vec3f& N, const Vec3f& wi, const Vec3f& wo,
          float heroWavelengthNm);

Vec3f
_EvalThroughput(const Bsdf::ClosureTree& tree, Bsdf::NodeId nodeId,
                const Vec3f& N, const Vec3f& wo,
                float heroWavelengthNm);

float
_ApproxWeight(const Bsdf::ClosureTree& tree, Bsdf::NodeId nodeId,
              const Vec3f& N, const Vec3f& wo,
              float heroWavelengthNm);

float
_PdfNode(const Bsdf::ClosureTree& tree, Bsdf::NodeId nodeId,
         const Vec3f& N, const Vec3f& wi, const Vec3f& wo,
         float heroWavelengthNm);

Bsdf::BsdfSample
_SampleNode(const Bsdf::ClosureTree& tree, Bsdf::NodeId nodeId,
            const Vec3f& N, const Vec3f& wo,
            float u1, float u2, float uChoice,
            float heroWavelengthNm);

Vec3f
_EvalNode(const Bsdf::ClosureTree& tree, Bsdf::NodeId nodeId,
          const Vec3f& N, const Vec3f& wi, const Vec3f& wo,
          float heroWavelengthNm)
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
            bool sameSide = _IsSameSide(N, wi, wo);
            const Vec3f shadingN = _ResolveReflectionNormal(data, N, wo);
            const float effectiveIor =
                _ResolveDielectricIor(data, heroWavelengthNm);
            if (sameSide &&
                data.scatterMode != Bsdf::ScatterMode::Transmission) {
                const Vec3f fresnel = _DielectricReflectionFresnel(
                    data,
                    _ReflectionFresnelCosTheta(wi, wo),
                    effectiveIor);
                if (_IsEffectivelyIsotropic(data.roughness)) {
                    result += _EvalMicrofacetReflectionIsotropic(
                        std::clamp(data.roughness[0], 1.0e-5f, 1.0f),
                        fresnel * data.weight,
                        shadingN,
                        wi,
                        wo);
                } else {
                    result += _EvalMicrofacetReflectionAnisotropic(
                        data.roughness,
                        data.tangent,
                        fresnel * data.weight,
                        shadingN,
                        wi,
                        wo);
                }
            }
            if (!sameSide &&
                data.scatterMode != Bsdf::ScatterMode::Reflection) {
                const float fresnelCos =
                    _TransmissionFresnelCosTheta(effectiveIor, N, wi, wo);
                const float baseReflectance =
                    _SchlickFresnelScalar(effectiveIor, fresnelCos);
                const Vec3f transmissionScale = _TransmissionScale(
                    baseReflectance,
                    _DielectricReflectionFresnelUntinted(
                        data, fresnelCos, effectiveIor));
                result += CompMul(
                    Bsdf::EvalGGXTransmission(
                        _AverageAlphaAsRoughness(data.roughness),
                        effectiveIor,
                        data.tint,
                        N,
                        wi,
                        wo) * data.weight,
                    transmissionScale);
            }
            return _SafeVec(result);
        } else if constexpr (std::is_same_v<T, Bsdf::ConductorData>) {
            if (Dot(N, wi) <= 0.0f || data.weight <= 0.0f) {
                return Vec3f(0.0f);
            }
            const Vec3f fresnel = _ConductorReflectionFresnel(
                data,
                _ReflectionFresnelCosTheta(wi, wo));
            if (_IsEffectivelyIsotropic(data.roughness)) {
                return _EvalMicrofacetReflectionIsotropic(
                    std::clamp(data.roughness[0], 1.0e-5f, 1.0f),
                    fresnel * data.weight,
                    N,
                    wi,
                    wo);
            }
            return _EvalMicrofacetReflectionAnisotropic(
                data.roughness,
                data.tangent,
                fresnel * data.weight,
                N,
                wi,
                wo);
        } else if constexpr (std::is_same_v<T, Bsdf::GeneralizedSchlickData>) {
            Vec3f result(0.0f);
            bool sameSide = _IsSameSide(N, wi, wo);
            Vec3f shadingN = _FaceForwardNormal(N, wo);
            if (sameSide &&
                data.scatterMode != Bsdf::ScatterMode::Transmission) {
                const Vec3f fresnel = _GeneralizedSchlickReflectionFresnel(
                    data,
                    _ReflectionFresnelCosTheta(wi, wo));
                if (_IsEffectivelyIsotropic(data.roughness)) {
                    result += _EvalMicrofacetReflectionIsotropic(
                        std::clamp(data.roughness[0], 1.0e-5f, 1.0f),
                        fresnel * data.weight,
                        shadingN,
                        wi,
                        wo);
                } else {
                    result += _EvalMicrofacetReflectionAnisotropic(
                        data.roughness,
                        data.tangent,
                        fresnel * data.weight,
                        shadingN,
                        wi,
                        wo);
                }
            }
            if (!sameSide &&
                data.scatterMode != Bsdf::ScatterMode::Reflection) {
                float avgF0 = _Clamp01(_Luminance(_SaturateVec(data.color0)));
                float ior = (1.0f + std::sqrt(std::max(avgF0, 0.01f))) /
                            (1.0f - std::sqrt(std::max(avgF0, 0.01f)));
                const float fresnelCos =
                    _TransmissionFresnelCosTheta(ior, N, wi, wo);
                const float baseReflectance =
                    _SchlickFresnelScalar(ior, fresnelCos);
                const Vec3f transmissionScale = _TransmissionScale(
                    baseReflectance,
                    _GeneralizedSchlickReflectionFresnel(data, fresnelCos));
                result += CompMul(
                    Bsdf::EvalGGXTransmission(
                        _AverageAlphaAsRoughness(data.roughness),
                        ior,
                        Vec3f(1.0f),
                        N,
                        wi,
                        wo) * data.weight,
                    transmissionScale);
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
                _EvalNode(tree, data.bg, N, wi, wo, heroWavelengthNm),
                _EvalNode(tree, data.fg, N, wi, wo, heroWavelengthNm),
                _Clamp01(data.mix));
        } else if constexpr (std::is_same_v<T, Bsdf::LayerData>) {
            Vec3f topEval = _EvalNode(
                tree, data.top, N, wi, wo, heroWavelengthNm);
            Vec3f baseEval = _EvalNode(
                tree, data.base, N, wi, wo, heroWavelengthNm);
            const Vec3f topThroughputOut =
                _EvalThroughput(tree, data.top, N, wo, heroWavelengthNm);
            const Vec3f topThroughputIn =
                _EvalThroughput(tree, data.top, N, wi, heroWavelengthNm);
            return topEval + CompMul(
                baseEval,
                CompMul(topThroughputOut, topThroughputIn));
        } else if constexpr (std::is_same_v<T, Bsdf::AddData>) {
            return _EvalNode(tree, data.in1, N, wi, wo, heroWavelengthNm) +
                   _EvalNode(tree, data.in2, N, wi, wo, heroWavelengthNm);
        } else if constexpr (std::is_same_v<T, Bsdf::MultiplyData>) {
            return CompMul(
                data.weight,
                _EvalNode(tree, data.input, N, wi, wo, heroWavelengthNm));
        } else {
            return Vec3f(0.0f);
        }
    }, node->data);
}

Vec3f
_EvalThroughput(const Bsdf::ClosureTree& tree, Bsdf::NodeId nodeId,
                const Vec3f& N, const Vec3f& wo,
                float heroWavelengthNm)
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
            const Vec3f shadingN = _ResolveReflectionNormal(data, N, wo);
            const float NdotV =
                std::max(std::abs(Dot(shadingN, wo)), _kEpsilon);
            const float effectiveIor =
                _ResolveDielectricIor(data, heroWavelengthNm);
            Vec3f throughput(1.0f);
            if (data.scatterMode != Bsdf::ScatterMode::Transmission) {
                throughput -= _DielectricReflectionFresnelUntinted(
                    data, NdotV, effectiveIor) * data.weight;
            }
            return _SaturateVec(throughput);
        } else if constexpr (std::is_same_v<T, Bsdf::ConductorData>) {
            const float NdotV = std::max(std::abs(Dot(N, wo)), _kEpsilon);
            return _SaturateVec(
                Vec3f(1.0f) -
                _ConductorReflectionFresnel(data, NdotV) * data.weight);
        } else if constexpr (std::is_same_v<T, Bsdf::GeneralizedSchlickData>) {
            const float NdotV = std::max(std::abs(Dot(N, wo)), _kEpsilon);
            return _SaturateVec(
                Vec3f(1.0f) -
                _GeneralizedSchlickReflectionFresnel(data, NdotV) *
                    data.weight);
        } else if constexpr (std::is_same_v<T, Bsdf::SheenData>) {
            float NdotV = std::max(std::abs(Dot(N, wo)), _kEpsilon);
            float dirAlbedo = _ApproxSheenDirAlbedo(NdotV, data.roughness);
            return _SaturateVec(Vec3f(1.0f - dirAlbedo * data.weight));
        } else if constexpr (std::is_same_v<T, Bsdf::MixData>) {
            return _LerpVec(
                _EvalThroughput(tree, data.bg, N, wo, heroWavelengthNm),
                _EvalThroughput(tree, data.fg, N, wo, heroWavelengthNm),
                _Clamp01(data.mix));
        } else if constexpr (std::is_same_v<T, Bsdf::LayerData>) {
            return CompMul(
                _EvalThroughput(tree, data.top, N, wo, heroWavelengthNm),
                _EvalThroughput(tree, data.base, N, wo, heroWavelengthNm));
        } else if constexpr (std::is_same_v<T, Bsdf::AddData>) {
            Vec3f t = _EvalThroughput(
                          tree, data.in1, N, wo, heroWavelengthNm) +
                      _EvalThroughput(
                          tree, data.in2, N, wo, heroWavelengthNm) -
                      Vec3f(1.0f);
            return Vec3f(std::max(t[0], 0.0f),
                         std::max(t[1], 0.0f),
                         std::max(t[2], 0.0f));
        } else if constexpr (std::is_same_v<T, Bsdf::MultiplyData>) {
            return _EvalThroughput(tree, data.input, N, wo, heroWavelengthNm);
        } else {
            return Vec3f(0.0f);
        }
    }, node->data);
}

float
_ApproxWeight(const Bsdf::ClosureTree& tree, Bsdf::NodeId nodeId,
              const Vec3f& N, const Vec3f& wo,
              float heroWavelengthNm)
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
            return data.weight * std::max(_Luminance(data.color), 0.0f);
        } else if constexpr (std::is_same_v<T, Bsdf::DielectricData>) {
            const Vec3f shadingN = _ResolveReflectionNormal(data, N, wo);
            const float NdotV =
                std::max(std::abs(Dot(shadingN, wo)), _kEpsilon);
            const float effectiveIor =
                _ResolveDielectricIor(data, heroWavelengthNm);
            const Vec3f reflectance =
                _DielectricReflectionFresnelUntinted(
                    data, NdotV, effectiveIor);
            const float baseReflectance =
                _SchlickFresnelScalar(effectiveIor, NdotV);
            const Vec3f transmissionScale = _TransmissionScale(
                baseReflectance,
                reflectance);
            if (data.scatterMode == Bsdf::ScatterMode::Reflection) {
                return std::max(_Luminance(reflectance) * data.weight, 0.05f);
            }
            if (data.scatterMode == Bsdf::ScatterMode::Transmission) {
                return std::max(
                    data.weight *
                        _Luminance(CompMul(data.tint, transmissionScale)),
                    0.05f);
            }
            return std::max(
                data.weight *
                    (_Luminance(reflectance) +
                     _Luminance(CompMul(data.tint, transmissionScale))),
                0.05f);
        } else if constexpr (std::is_same_v<T, Bsdf::ConductorData>) {
            return data.weight *
                std::max(
                    _Luminance(_ConductorReflectionFresnel(
                        data,
                        std::max(std::abs(Dot(N, wo)), _kEpsilon))),
                    0.05f);
        } else if constexpr (std::is_same_v<T, Bsdf::GeneralizedSchlickData>) {
            const float NdotV = std::max(std::abs(Dot(N, wo)), _kEpsilon);
            const Vec3f reflectance = _GeneralizedSchlickReflectionFresnel(
                data,
                NdotV);
            if (data.scatterMode == Bsdf::ScatterMode::Transmission) {
                const float avgF0 =
                    _Clamp01(_Luminance(_SaturateVec(data.color0)));
                const float sqrtF0 = std::sqrt(std::max(avgF0, 0.01f));
                const float ior = (1.0f + sqrtF0) / (1.0f - sqrtF0);
                return std::max(
                    data.weight *
                        _Luminance(_TransmissionScale(
                            _SchlickFresnelScalar(ior, NdotV),
                            reflectance)),
                    0.05f);
            }
            if (data.scatterMode == Bsdf::ScatterMode::ReflectionTransmission) {
                const float avgF0 =
                    _Clamp01(_Luminance(_SaturateVec(data.color0)));
                const float sqrtF0 = std::sqrt(std::max(avgF0, 0.01f));
                const float ior = (1.0f + sqrtF0) / (1.0f - sqrtF0);
                return std::max(
                    data.weight *
                        (_Luminance(reflectance) +
                         _Luminance(_TransmissionScale(
                             _SchlickFresnelScalar(ior, NdotV),
                             reflectance))),
                    0.05f);
            }
            return data.weight *
                std::max(_Luminance(reflectance), 0.05f);
        } else if constexpr (std::is_same_v<T, Bsdf::SheenData>) {
            return data.weight * std::max(_Luminance(data.color), 0.0f) * 0.25f;
        } else if constexpr (std::is_same_v<T, Bsdf::UnsupportedData>) {
            return 0.0f;
        } else if constexpr (std::is_same_v<T, Bsdf::MixData>) {
            return _LerpVec(
                Vec3f(_ApproxWeight(tree, data.bg, N, wo, heroWavelengthNm)),
                Vec3f(_ApproxWeight(tree, data.fg, N, wo, heroWavelengthNm)),
                _Clamp01(data.mix))[0];
        } else if constexpr (std::is_same_v<T, Bsdf::LayerData>) {
            return _ApproxWeight(tree, data.top, N, wo, heroWavelengthNm) +
                _ApproxWeight(tree, data.base, N, wo, heroWavelengthNm) *
                _Luminance(
                    _EvalThroughput(tree, data.top, N, wo, heroWavelengthNm));
        } else if constexpr (std::is_same_v<T, Bsdf::AddData>) {
            return _ApproxWeight(tree, data.in1, N, wo, heroWavelengthNm) +
                   _ApproxWeight(tree, data.in2, N, wo, heroWavelengthNm);
        } else if constexpr (std::is_same_v<T, Bsdf::MultiplyData>) {
            return _ApproxWeight(tree, data.input, N, wo, heroWavelengthNm) *
                   std::max(_Luminance(data.weight), 0.0f);
        } else {
            return 0.0f;
        }
    }, node->data);
}

float
_PdfNode(const Bsdf::ClosureTree& tree, Bsdf::NodeId nodeId,
         const Vec3f& N, const Vec3f& wi, const Vec3f& wo,
         float heroWavelengthNm)
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
            bool sameSide = _IsSameSide(N, wi, wo);
            const Vec3f shadingN = _ResolveReflectionNormal(data, N, wo);
            const float effectiveIor =
                _ResolveDielectricIor(data, heroWavelengthNm);
            if (sameSide &&
                data.scatterMode != Bsdf::ScatterMode::Transmission) {
                if (_IsEffectivelyIsotropic(data.roughness)) {
                    return Bsdf::PdfGGXSpecular(
                        _AverageAlphaAsRoughness(data.roughness),
                        shadingN,
                        wi,
                        wo);
                }
                return _PdfGGXSpecularAnisotropic(
                    data.roughness,
                    data.tangent,
                    shadingN,
                    wi,
                    wo);
            }
            if (!sameSide &&
                data.scatterMode != Bsdf::ScatterMode::Reflection) {
                return Bsdf::PdfGGXTransmission(
                    _AverageAlphaAsRoughness(data.roughness),
                    effectiveIor,
                    N,
                    wi,
                    wo);
            }
            return 0.0f;
        } else if constexpr (std::is_same_v<T, Bsdf::ConductorData>) {
            return (Dot(N, wi) > 0.0f)
                ? (_IsEffectivelyIsotropic(data.roughness)
                    ? Bsdf::PdfGGXSpecular(
                        _AverageAlphaAsRoughness(data.roughness),
                        N,
                        wi,
                        wo)
                    : _PdfGGXSpecularAnisotropic(
                        data.roughness,
                        data.tangent,
                        N,
                        wi,
                        wo))
                : 0.0f;
        } else if constexpr (std::is_same_v<T, Bsdf::GeneralizedSchlickData>) {
            bool sameSide = _IsSameSide(N, wi, wo);
            Vec3f shadingN = _FaceForwardNormal(N, wo);
            if (sameSide &&
                data.scatterMode != Bsdf::ScatterMode::Transmission) {
                if (_IsEffectivelyIsotropic(data.roughness)) {
                    return Bsdf::PdfGGXSpecular(
                        _AverageAlphaAsRoughness(data.roughness),
                        shadingN,
                        wi,
                        wo);
                }
                return _PdfGGXSpecularAnisotropic(
                    data.roughness,
                    data.tangent,
                    shadingN,
                    wi,
                    wo);
            }
            if (!sameSide &&
                data.scatterMode != Bsdf::ScatterMode::Reflection) {
                float avgF0 = _Clamp01(_Luminance(_SaturateVec(data.color0)));
                float sqrtF0 = std::sqrt(std::max(avgF0, 0.01f));
                float ior = (1.0f + sqrtF0) / (1.0f - sqrtF0);
                return Bsdf::PdfGGXTransmission(
                    _AverageAlphaAsRoughness(data.roughness),
                    ior,
                    N,
                    wi,
                    wo);
            }
            return 0.0f;
        } else if constexpr (std::is_same_v<T, Bsdf::SheenData>) {
            return (Dot(N, wi) > 0.0f) ? Bsdf::PdfLambertian(N, wi) : 0.0f;
        } else if constexpr (std::is_same_v<T, Bsdf::UnsupportedData>) {
            return 0.0f;
        } else if constexpr (std::is_same_v<T, Bsdf::MixData>) {
            float mix = _Clamp01(data.mix);
            return (1.0f - mix) *
                       _PdfNode(tree, data.bg, N, wi, wo, heroWavelengthNm) +
                   mix *
                       _PdfNode(tree, data.fg, N, wi, wo, heroWavelengthNm);
        } else if constexpr (std::is_same_v<T, Bsdf::LayerData>) {
            float topWeight = _ApproxWeight(
                tree, data.top, N, wo, heroWavelengthNm);
            float baseWeight = _ApproxWeight(
                tree, data.base, N, wo, heroWavelengthNm) *
                _Luminance(
                    _EvalThroughput(tree, data.top, N, wo, heroWavelengthNm));
            float total = topWeight + baseWeight;
            if (total <= 0.0f) {
                return 0.0f;
            }
            return (topWeight / total) *
                       _PdfNode(tree, data.top, N, wi, wo, heroWavelengthNm) +
                   (baseWeight / total) *
                       _PdfNode(
                           tree, data.base, N, wi, wo, heroWavelengthNm);
        } else if constexpr (std::is_same_v<T, Bsdf::AddData>) {
            float w1 = _ApproxWeight(tree, data.in1, N, wo, heroWavelengthNm);
            float w2 = _ApproxWeight(tree, data.in2, N, wo, heroWavelengthNm);
            float total = w1 + w2;
            if (total <= 0.0f) {
                return 0.0f;
            }
            return (w1 / total) *
                       _PdfNode(tree, data.in1, N, wi, wo, heroWavelengthNm) +
                   (w2 / total) *
                       _PdfNode(tree, data.in2, N, wi, wo, heroWavelengthNm);
        } else if constexpr (std::is_same_v<T, Bsdf::MultiplyData>) {
            return _PdfNode(tree, data.input, N, wi, wo, heroWavelengthNm);
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
                       Bsdf::BsdfSample sample,
                       float heroWavelengthNm)
{
    if (sample.isSubsurface) {
        sample.pdf = std::max(sample.pdf, 1.0f);
        return sample;
    }
    if (sample.pdf <= 0.0f) {
        return sample;
    }
    if (sample.isSpecular) {
        sample.pdf = std::max(sample.pdf, 1.0f);
        return sample;
    }
    sample.f = _EvalNode(
        tree, nodeId, N, sample.wi, wo, heroWavelengthNm);
    sample.pdf = _PdfNode(
        tree, nodeId, N, sample.wi, wo, heroWavelengthNm);
    return sample;
}

Bsdf::BsdfSample
_SampleNode(const Bsdf::ClosureTree& tree, Bsdf::NodeId nodeId,
            const Vec3f& N, const Vec3f& wo,
            float u1, float u2, float uChoice,
            float heroWavelengthNm)
{
    const Bsdf::Node* node = tree.Get(nodeId);
    if (!node) {
        return Bsdf::BsdfSample{Vec3f(0.0f), Vec3f(0.0f), 0.0f, false};
    }

    return std::visit([&](const auto& data) -> Bsdf::BsdfSample {
        using T = std::decay_t<decltype(data)>;
        if constexpr (std::is_same_v<T, Bsdf::OrenNayarDiffuseData>) {
            auto sample = Bsdf::SampleLambertian(data.color * data.weight, N, wo, u1, u2);
            return _FinalizeSubtreeSample(
                tree, nodeId, N, wo, sample, heroWavelengthNm);
        } else if constexpr (std::is_same_v<T, Bsdf::BurleyDiffuseData>) {
            auto sample = Bsdf::SampleLambertian(data.color * data.weight, N, wo, u1, u2);
            return _FinalizeSubtreeSample(
                tree, nodeId, N, wo, sample, heroWavelengthNm);
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
            return _FinalizeSubtreeSample(
                tree, nodeId, N, wo, sample, heroWavelengthNm);
        } else if constexpr (std::is_same_v<T, Bsdf::SubsurfaceData>) {
            Bsdf::BsdfSample sample{
                Vec3f(0.0f),
                data.color * data.weight,
                1.0f,
                false
            };
            sample.isSubsurface = true;
            return sample;
        } else if constexpr (std::is_same_v<T, Bsdf::DielectricData>) {
            const Vec3f shadingN = _ResolveReflectionNormal(data, N, wo);
            const float NdotV =
                std::max(std::abs(Dot(shadingN, wo)), _kEpsilon);
            const float effectiveIor =
                _ResolveDielectricIor(data, heroWavelengthNm);
            const bool hasDeltaRoughness =
                _IsEffectivelyDeltaAlpha(data.roughness);
            float fresnelProb = _Clamp01(_Luminance(
                _DielectricReflectionFresnelUntinted(
                    data, NdotV, effectiveIor)));
            if (data.scatterMode == Bsdf::ScatterMode::Reflection) {
                if (hasDeltaRoughness) {
                    return _SampleDeltaDielectricReflection(
                        data, effectiveIor, shadingN, wo);
                }
                if (_IsEffectivelyIsotropic(data.roughness)) {
                    auto sample = Bsdf::SampleGGXSpecular(
                        _AverageAlphaAsRoughness(data.roughness),
                        effectiveIor,
                        data.tint,
                        shadingN,
                        wo,
                        u1,
                        u2);
                    return _FinalizeSubtreeSample(
                        tree, nodeId, N, wo, sample, heroWavelengthNm);
                }
                auto sample = _SampleGGXSpecularAnisotropic(
                    data.roughness,
                    data.tangent,
                    shadingN,
                    wo,
                    u1,
                    u2);
                return _FinalizeSubtreeSample(
                    tree, nodeId, N, wo, sample, heroWavelengthNm);
            }
            if (data.scatterMode == Bsdf::ScatterMode::Transmission) {
                if (hasDeltaRoughness) {
                    return _SampleDeltaDielectricTransmission(
                        data, effectiveIor, NdotV, N, wo);
                }
                auto sample = Bsdf::SampleGGXTransmission(
                    _AverageAlphaAsRoughness(data.roughness),
                    effectiveIor,
                    data.tint,
                    N,
                    wo,
                    u1,
                    u2);
                if (sample.pdf <= 0.0f) {
                    return _SampleDeltaDielectricTransmission(
                        data, effectiveIor, NdotV, N, wo);
                }
                sample.f *= data.weight;
                return _FinalizeSubtreeSample(
                    tree, nodeId, N, wo, sample, heroWavelengthNm);
            }
            if (uChoice < fresnelProb) {
                if (hasDeltaRoughness) {
                    return _ScaleDiscreteSpecularSample(
                        _SampleDeltaDielectricReflection(
                            data, effectiveIor, shadingN, wo),
                        fresnelProb);
                }
                if (_IsEffectivelyIsotropic(data.roughness)) {
                    auto sample = Bsdf::SampleGGXSpecular(
                        _AverageAlphaAsRoughness(data.roughness),
                        effectiveIor,
                        data.tint,
                        shadingN,
                        wo,
                        u1,
                        u2);
                    return _FinalizeSubtreeSample(
                        tree, nodeId, N, wo, sample, heroWavelengthNm);
                }
                auto sample = _SampleGGXSpecularAnisotropic(
                    data.roughness,
                    data.tangent,
                    shadingN,
                    wo,
                    u1,
                    u2);
                return _FinalizeSubtreeSample(
                    tree, nodeId, N, wo, sample, heroWavelengthNm);
            }
            {
                if (hasDeltaRoughness) {
                    return _ScaleDiscreteSpecularSample(
                        _SampleDeltaDielectricTransmission(
                            data, effectiveIor, NdotV, N, wo),
                        1.0f - fresnelProb);
                }
                auto sample = Bsdf::SampleGGXTransmission(
                    _AverageAlphaAsRoughness(data.roughness),
                    effectiveIor,
                    data.tint,
                    N,
                    wo,
                    u1,
                    u2);
                if (sample.pdf <= 0.0f) {
                    return _ScaleDiscreteSpecularSample(
                        _SampleDeltaDielectricTransmission(
                            data, effectiveIor, NdotV, N, wo),
                        1.0f - fresnelProb);
                }
                sample.f *= data.weight;
                return _FinalizeSubtreeSample(
                    tree, nodeId, N, wo, sample, heroWavelengthNm);
            }
        } else if constexpr (std::is_same_v<T, Bsdf::ConductorData>) {
            if (_IsEffectivelyIsotropic(data.roughness)) {
                auto sample = Bsdf::SampleGGXSpecular(
                    _AverageAlphaAsRoughness(data.roughness),
                    1.5f,
                    _ConductorF0(data.ior, data.extinction),
                    N,
                    wo,
                    u1,
                    u2);
                return _FinalizeSubtreeSample(
                    tree, nodeId, N, wo, sample, heroWavelengthNm);
            }
            auto sample = _SampleGGXSpecularAnisotropic(
                data.roughness,
                data.tangent,
                N,
                wo,
                u1,
                u2);
            return _FinalizeSubtreeSample(
                tree, nodeId, N, wo, sample, heroWavelengthNm);
        } else if constexpr (std::is_same_v<T, Bsdf::GeneralizedSchlickData>) {
            const float NdotV = std::max(std::abs(Dot(N, wo)), _kEpsilon);
            float fresnelProb = _Clamp01(_Luminance(
                _GeneralizedSchlickReflectionFresnel(data, NdotV)));
            Vec3f shadingN = _FaceForwardNormal(N, wo);
            if (data.scatterMode == Bsdf::ScatterMode::Transmission) {
                float avgF0 = _Clamp01(_Luminance(_SaturateVec(data.color0)));
                float sqrtF0 = std::sqrt(std::max(avgF0, 0.01f));
                float ior = (1.0f + sqrtF0) / (1.0f - sqrtF0);
                auto sample = Bsdf::SampleGGXTransmission(
                    _AverageAlphaAsRoughness(data.roughness),
                    ior,
                    Vec3f(1.0f),
                    N,
                    wo,
                    u1,
                    u2);
                if (sample.pdf <= 0.0f) {
                    auto deltaSample = _SampleDeltaTransmission(
                        ior, Vec3f(1.0f), data.weight, N, wo);
                    deltaSample.f = CompMul(
                        deltaSample.f,
                        _TransmissionScale(
                            _SchlickFresnelScalar(ior, NdotV),
                            _GeneralizedSchlickReflectionFresnel(
                                data, NdotV)));
                    return deltaSample;
                }
                sample.f *= data.weight;
                return _FinalizeSubtreeSample(
                    tree, nodeId, N, wo, sample, heroWavelengthNm);
            }
            if (data.scatterMode == Bsdf::ScatterMode::ReflectionTransmission &&
                uChoice >= fresnelProb) {
                float avgF0 = _Clamp01(_Luminance(_SaturateVec(data.color0)));
                float sqrtF0 = std::sqrt(std::max(avgF0, 0.01f));
                float ior = (1.0f + sqrtF0) / (1.0f - sqrtF0);
                auto sample = Bsdf::SampleGGXTransmission(
                    _AverageAlphaAsRoughness(data.roughness),
                    ior,
                    Vec3f(1.0f),
                    N,
                    wo,
                    u1,
                    u2);
                if (sample.pdf <= 0.0f) {
                    auto deltaSample = _SampleDeltaTransmission(
                        ior, Vec3f(1.0f), data.weight, N, wo);
                    deltaSample.f = CompMul(
                        deltaSample.f,
                        _TransmissionScale(
                            _SchlickFresnelScalar(ior, NdotV),
                            _GeneralizedSchlickReflectionFresnel(
                                data, NdotV)));
                    return deltaSample;
                }
                sample.f *= data.weight;
                return _FinalizeSubtreeSample(
                    tree, nodeId, N, wo, sample, heroWavelengthNm);
            }
            if (_IsEffectivelyIsotropic(data.roughness)) {
                auto sample = Bsdf::SampleGGXSpecular(
                    _AverageAlphaAsRoughness(data.roughness),
                    1.5f,
                    data.color0,
                    shadingN,
                    wo,
                    u1,
                    u2);
                return _FinalizeSubtreeSample(
                    tree, nodeId, N, wo, sample, heroWavelengthNm);
            }
            auto sample = _SampleGGXSpecularAnisotropic(
                data.roughness,
                data.tangent,
                shadingN,
                wo,
                u1,
                u2);
            return _FinalizeSubtreeSample(
                tree, nodeId, N, wo, sample, heroWavelengthNm);
        } else if constexpr (std::is_same_v<T, Bsdf::SheenData>) {
            auto sample = Bsdf::SampleLambertian(data.color * data.weight, N, wo, u1, u2);
            return _FinalizeSubtreeSample(
                tree, nodeId, N, wo, sample, heroWavelengthNm);
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
            auto sample = _SampleNode(
                tree, chosen, N, wo, u1, u2, remapped, heroWavelengthNm);
            if ((sample.isSpecular || sample.isSubsurface) &&
                sample.pdf > 0.0f && chooseProb > 0.0f) {
                sample.f /= chooseProb;
            }
            return _FinalizeSubtreeSample(
                tree, nodeId, N, wo, sample, heroWavelengthNm);
        } else if constexpr (std::is_same_v<T, Bsdf::LayerData>) {
            float topWeight = _ApproxWeight(
                tree, data.top, N, wo, heroWavelengthNm);
            float baseWeight = _ApproxWeight(
                tree, data.base, N, wo, heroWavelengthNm) *
                _Luminance(
                    _EvalThroughput(tree, data.top, N, wo, heroWavelengthNm));
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
            auto sample = _SampleNode(
                tree, chosen, N, wo, u1, u2, remapped, heroWavelengthNm);
            if ((sample.isSpecular || sample.isSubsurface) &&
                sample.pdf > 0.0f) {
                if (!chooseTop) {
                    const Vec3f topThroughputOut =
                        _EvalThroughput(
                            tree, data.top, N, wo, heroWavelengthNm);
                    const Vec3f topThroughputIn =
                        _EvalThroughput(
                            tree, data.top, N, sample.wi, heroWavelengthNm);
                    sample.f = CompMul(
                        sample.f,
                        CompMul(topThroughputOut, topThroughputIn));
                }
                if (chooseProb > 0.0f) {
                    sample.f /= chooseProb;
                }
            }
            return _FinalizeSubtreeSample(
                tree, nodeId, N, wo, sample, heroWavelengthNm);
        } else if constexpr (std::is_same_v<T, Bsdf::AddData>) {
            float w1 = _ApproxWeight(tree, data.in1, N, wo, heroWavelengthNm);
            float w2 = _ApproxWeight(tree, data.in2, N, wo, heroWavelengthNm);
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
            auto sample = _SampleNode(
                tree, chosen, N, wo, u1, u2, remapped, heroWavelengthNm);
            if ((sample.isSpecular || sample.isSubsurface) &&
                sample.pdf > 0.0f && chooseProb > 0.0f) {
                sample.f /= chooseProb;
            }
            return _FinalizeSubtreeSample(
                tree, nodeId, N, wo, sample, heroWavelengthNm);
        } else if constexpr (std::is_same_v<T, Bsdf::MultiplyData>) {
            auto sample = _SampleNode(
                tree, data.input, N, wo, u1, u2, uChoice, heroWavelengthNm);
            if ((sample.isSpecular || sample.isSubsurface) &&
                sample.pdf > 0.0f) {
                sample.f = CompMul(data.weight, sample.f);
            }
            return _FinalizeSubtreeSample(
                tree, nodeId, N, wo, sample, heroWavelengthNm);
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
    float G = _GGX_G(alpha, NdotV, NdotL);
    Vec3f F = _SchlickFresnel(specularColor, VdotH);
    return _SafeVec(CompMul(
        F,
        Vec3f(D * G / std::max(4.0f * NdotL * NdotV, _kEpsilon))));
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
    float G = _GGX_G(alpha, NdotV, NdotL);
    float F = _SchlickFresnelScalar(coatIor, VdotH);
    return Vec3f(
        coatWeight * D * G * F /
        std::max(4.0f * NdotL * NdotV, _kEpsilon));
}

Vec3f
Bsdf::EvalSurface(
    const SurfaceClosure& closure,
    const Vec3f& N,
    const Vec3f& wi,
    const Vec3f& wo,
    float heroWavelengthNm)
{
    Vec3f f = closure.HasBsdfTree()
        ? _EvalNode(
            closure.bsdfTree,
            closure.bsdfTree.root,
            N,
            wi,
            wo,
            heroWavelengthNm)
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
    float G = _GGX_G(alpha, NdotV, NdotL);
    Vec3f F = _SchlickFresnel(specularColor, VdotH);
    Vec3f f = CompMul(
        F,
        Vec3f(D * G / std::max(4.0f * NdotL * NdotV, _kEpsilon)));
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
    float u1, float u2, float uLobe,
    float heroWavelengthNm)
{
    if (closure.HasBsdfTree()) {
        auto sample = _SampleNode(closure.bsdfTree, closure.bsdfTree.root,
                                  N, wo, u1, u2, uLobe, heroWavelengthNm);
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
    const Vec3f& wo,
    float heroWavelengthNm)
{
    if (closure.HasBsdfTree()) {
        return _PdfNode(
            closure.bsdfTree,
            closure.bsdfTree.root,
            N,
            wi,
            wo,
            heroWavelengthNm);
    }
    return _PdfLegacySurface(closure, N, wi, wo);
}

}  // namespace mxcpp
