//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "bsdf.h"

#include "adobeOpenPbr.h"
#include "bsdfDielectricBothLut.h"
#include "bsdfDielectricReflFrontLut.h"
#include "bsdfDielectricTransmissionLut.h"

#include "../spectral.h"
#include "../nodes/helpers/colorHelpers.h"
#include "../nodes/helpers/mathHelpers.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <utility>
#include <variant>

namespace mxcpp {

namespace {

constexpr float _kEpsilon = 1e-7f;
constexpr float _kMinMicrofacetAlpha = 1.0e-6f;
constexpr float _kEffectivelySmoothMicrofacetAlpha = 1.0e-3f;
// The 16-sample cosine LUT cannot preserve the sharp inside critical-angle
// transition this close to smooth, where exact Fresnel is already accurate.
constexpr float _kTransmissionExactFresnelMaxAlpha = 2.0e-3f;
constexpr float _kTransmissionFresnelBlendMaxAlpha = 7.0e-2f;
constexpr float _kTurquinMicrofacetMsMinAlpha = 0.04f * 0.04f;
constexpr int _kThinFilmAiryIterations = 2;
constexpr int _kGgxEnergyCosThetaCount = 16;
constexpr int _kGgxEnergyAlphaCount = 16;
constexpr float _kBsdlDielectricIorMin = 1.001f;
constexpr float _kBsdlDielectricIorMax = 5.0f;

constexpr float _kGgxEnergyCosTheta[_kGgxEnergyCosThetaCount] = {
    0.00000100f, 0.00444444f, 0.01777778f, 0.04000000f,
    0.07111111f, 0.11111111f, 0.16000000f, 0.21777778f,
    0.28444444f, 0.36000000f, 0.44444444f, 0.53777778f,
    0.64000000f, 0.75111111f, 0.87111111f, 1.00000000f
};

std::atomic<bool> _gGgxMicrofacetMultipleScatteringEnabled{true};
std::atomic<int> _gDielectricLayerThroughputMode{
    static_cast<int>(Bsdf::DielectricLayerThroughputMode::Bsdl)};

constexpr float _kGgxMissingEnergy[_kGgxEnergyAlphaCount]
                                  [_kGgxEnergyCosThetaCount] = {
    {0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f,
     0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f,
     0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f,
     0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f},
    {0.00251842f, 0.10873969f, 0.03560815f, 0.00730103f,
     0.00219183f, 0.00086720f, 0.00041042f, 0.00022733f,
     0.00013854f, 0.00009099f, 0.00005874f, 0.00004668f,
     0.00003773f, 0.00003439f, 0.00002834f, 0.00001910f},
    {0.00016554f, 0.04769413f, 0.10825214f, 0.07807820f,
     0.03569875f, 0.01556790f, 0.00744928f, 0.00397593f,
     0.00233956f, 0.00150004f, 0.00102161f, 0.00073913f,
     0.00057000f, 0.00045126f, 0.00037881f, 0.00032441f},
    {0.00003580f, 0.02308404f, 0.07602938f, 0.10933454f,
     0.09546772f, 0.06268572f, 0.03667217f, 0.02125753f,
     0.01283863f, 0.00821539f, 0.00558342f, 0.00401587f,
     0.00304407f, 0.00241294f, 0.00199295f, 0.00170253f},
    {0.00001365f, 0.01406628f, 0.05014461f, 0.09097353f,
     0.11231652f, 0.10537768f, 0.08230404f, 0.05800849f,
     0.03931191f, 0.02673055f, 0.01868067f, 0.01357192f,
     0.01028648f, 0.00812463f, 0.00666786f, 0.00565942f},
    {0.00000777f, 0.01032934f, 0.03686307f, 0.07183600f,
     0.10302268f, 0.11854025f, 0.11517593f, 0.09896376f,
     0.07851367f, 0.05972929f, 0.04489350f, 0.03404923f,
     0.02639741f, 0.02105702f, 0.01731795f, 0.01466948f},
    {0.00000602f, 0.00891040f, 0.03085004f, 0.06074776f,
     0.09209311f, 0.11708671f, 0.12968219f, 0.12861719f,
     0.11721273f, 0.10062687f, 0.08327757f, 0.06777745f,
     0.05511462f, 0.04528339f, 0.03785342f, 0.03230157f},
    {0.00000574f, 0.00873295f, 0.02908104f, 0.05650993f,
     0.08675254f, 0.11495294f, 0.13632866f, 0.14768555f,
     0.14851948f, 0.14078673f, 0.12768892f, 0.11239502f,
     0.09726997f, 0.08368530f, 0.07219535f, 0.06283393f},
    {0.00000613f, 0.00933429f, 0.03003326f, 0.05727624f,
     0.08755878f, 0.11748968f, 0.14371256f, 0.16334926f,
     0.17466622f, 0.17747106f, 0.17295896f, 0.16315704f,
     0.15028963f, 0.13629718f, 0.12259652f, 0.11005023f},
    {0.00000692f, 0.01048651f, 0.03289562f, 0.06172527f,
     0.09359129f, 0.12571895f, 0.15562138f, 0.18110315f,
     0.20046429f, 0.21276041f, 0.21792964f, 0.21670574f,
     0.21036727f, 0.20042989f, 0.18837675f, 0.17547331f},
    {0.00000797f, 0.01205832f, 0.03718911f, 0.06897347f,
     0.10383376f, 0.13917673f, 0.17294456f, 0.20344586f,
     0.22932807f, 0.24963102f, 0.26386024f, 0.27201595f,
     0.27454422f, 0.27222674f, 0.26604328f, 0.25703797f},
    {0.00000925f, 0.01396980f, 0.04261073f, 0.07840838f,
     0.11742203f, 0.15701454f, 0.19529044f, 0.23084265f,
     0.26262467f, 0.28988098f, 0.31213071f, 0.32916261f,
     0.34102497f, 0.34799622f, 0.35053956f, 0.34924430f},
    {0.00001071f, 0.01616679f, 0.04894407f, 0.08957929f,
     0.13365179f, 0.17835573f, 0.22180320f, 0.26271909f,
     0.30024740f, 0.33383413f, 0.36315057f, 0.38804170f,
     0.40849688f, 0.42462658f, 0.43664166f, 0.44483192f},
    {0.00001234f, 0.01860772f, 0.05602832f, 0.10213552f,
     0.15194483f, 0.20239539f, 0.25152039f, 0.29807272f,
     0.34129469f, 0.38076790f, 0.41630668f, 0.44788110f,
     0.47556614f, 0.49950582f, 0.51988951f, 0.53693525f},
    {0.00001412f, 0.02125776f, 0.06373329f, 0.11579522f,
     0.17181902f, 0.22842697f, 0.28351459f, 0.33580008f,
     0.38455736f, 0.42943946f, 0.47035274f, 0.50736840f,
     0.54065913f, 0.57045365f, 0.59700798f, 0.62058531f},
    {0.00001606f, 0.02409140f, 0.07195516f, 0.13032342f,
     0.19286820f, 0.25584307f, 0.31696033f, 0.37486268f,
     0.42881246f, 0.47848923f, 0.52384684f, 0.56501516f,
     0.60222926f, 0.63577955f, 0.66598027f, 0.69314718f}
};

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
_LookupGgxMissingEnergy(float cosTheta, float alphaRoughness)
{
    const float c = _Clamp01(cosTheta);
    const float alpha = _Clamp01(alphaRoughness);

    const float alphaCoord =
        std::sqrt(alpha) * static_cast<float>(_kGgxEnergyAlphaCount - 1);
    const int alpha0 = std::clamp(
        static_cast<int>(alphaCoord), 0, _kGgxEnergyAlphaCount - 1);
    const int alpha1 = std::min(alpha0 + 1, _kGgxEnergyAlphaCount - 1);
    const float alphaT = alphaCoord - static_cast<float>(alpha0);

    int cos0 = 0;
    while (cos0 + 1 < _kGgxEnergyCosThetaCount &&
           c > _kGgxEnergyCosTheta[cos0 + 1]) {
        ++cos0;
    }
    const int cos1 = std::min(cos0 + 1, _kGgxEnergyCosThetaCount - 1);
    const float cosDenom =
        std::max(_kGgxEnergyCosTheta[cos1] - _kGgxEnergyCosTheta[cos0],
                 _kEpsilon);
    const float cosT =
        std::clamp((c - _kGgxEnergyCosTheta[cos0]) / cosDenom, 0.0f, 1.0f);

    const float e00 = _kGgxMissingEnergy[alpha0][cos0];
    const float e01 = _kGgxMissingEnergy[alpha0][cos1];
    const float e10 = _kGgxMissingEnergy[alpha1][cos0];
    const float e11 = _kGgxMissingEnergy[alpha1][cos1];
    const float e0 = e00 * (1.0f - cosT) + e01 * cosT;
    const float e1 = e10 * (1.0f - cosT) + e11 * cosT;
    return _Clamp01(e0 * (1.0f - alphaT) + e1 * alphaT);
}

inline float
_BsdlDielectricReflFrontCosine(int index)
{
    const float t =
        static_cast<float>(index) /
        static_cast<float>(
            bsdf_luts::kBsdlDielectricReflFrontCosThetaCount - 1);
    return std::max(t, 1.0e-6f);
}

inline float
_LookupBsdlDielectricReflFrontFilter(
    float cosTheta,
    float perceptualRoughness,
    float ior)
{
    namespace lut = bsdf_luts;

    const float c = _Clamp01(cosTheta);
    const float roughness = _Clamp01(perceptualRoughness);
    const float clampedIor = std::clamp(
        ior,
        _kBsdlDielectricIorMin,
        _kBsdlDielectricIorMax);

    const float iorIndex = std::sqrt(
        (clampedIor - _kBsdlDielectricIorMin) /
        (_kBsdlDielectricIorMax - _kBsdlDielectricIorMin));
    const float iorCoord =
        iorIndex *
        static_cast<float>(lut::kBsdlDielectricReflFrontIorCount - 1);
    const int ior0 = std::clamp(
        static_cast<int>(iorCoord),
        0,
        lut::kBsdlDielectricReflFrontIorCount - 1);
    const int ior1 = std::min(
        ior0 + 1,
        lut::kBsdlDielectricReflFrontIorCount - 1);
    const float iorT = iorCoord - static_cast<float>(ior0);

    const float roughnessCoord =
        roughness *
        static_cast<float>(lut::kBsdlDielectricReflFrontRoughnessCount - 1);
    const int roughness0 = std::clamp(
        static_cast<int>(roughnessCoord),
        0,
        lut::kBsdlDielectricReflFrontRoughnessCount - 1);
    const int roughness1 = std::min(
        roughness0 + 1,
        lut::kBsdlDielectricReflFrontRoughnessCount - 1);
    const float roughnessT =
        roughnessCoord - static_cast<float>(roughness0);

    int cos0 = 0;
    int cos1 = 0;
    float cosT = 0.0f;
    float prevCos = _BsdlDielectricReflFrontCosine(0);
    if (c > prevCos) {
        cos0 = lut::kBsdlDielectricReflFrontCosThetaCount - 1;
        cos1 = cos0;
        for (int i = 1; i < lut::kBsdlDielectricReflFrontCosThetaCount; ++i) {
            const float nextCos = _BsdlDielectricReflFrontCosine(i);
            if (c < nextCos) {
                cos0 = i - 1;
                cos1 = i;
                cosT = (c - prevCos) / (nextCos - prevCos);
                break;
            }
            prevCos = nextCos;
        }
    }

    const auto lookup = [&](int i, int r, int c0) {
        return lut::kBsdlDielectricReflFrontFilter[i][r][c0];
    };
    const auto lerpCos = [&](int i, int r) {
        return lookup(i, r, cos0) * (1.0f - cosT) +
               lookup(i, r, cos1) * cosT;
    };
    const auto lerpRoughness = [&](int i) {
        return lerpCos(i, roughness0) * (1.0f - roughnessT) +
               lerpCos(i, roughness1) * roughnessT;
    };

    return _Clamp01(
        lerpRoughness(ior0) * (1.0f - iorT) +
        lerpRoughness(ior1) * iorT);
}

inline float
_LookupBsdlDielectricBothMissingEnergy(
    float cosTheta,
    float perceptualRoughness,
    float ior,
    bool backfacing)
{
    namespace lut = bsdf_luts;

    const float c = _Clamp01(cosTheta);
    const float roughness = _Clamp01(perceptualRoughness);
    const float clampedIor = std::clamp(
        ior,
        _kBsdlDielectricIorMin,
        _kBsdlDielectricIorMax);

    const float iorIndex = std::sqrt(
        (clampedIor - _kBsdlDielectricIorMin) /
        (_kBsdlDielectricIorMax - _kBsdlDielectricIorMin));
    const float iorCoord =
        iorIndex * static_cast<float>(lut::kBsdlDielectricBothIorCount - 1);
    const int ior0 = std::clamp(
        static_cast<int>(iorCoord), 0, lut::kBsdlDielectricBothIorCount - 1);
    const int ior1 =
        std::min(ior0 + 1, lut::kBsdlDielectricBothIorCount - 1);
    const float iorT = iorCoord - static_cast<float>(ior0);

    const float roughnessCoord = roughness * static_cast<float>(
        lut::kBsdlDielectricBothRoughnessCount - 1);
    const int roughness0 = std::clamp(
        static_cast<int>(roughnessCoord),
        0,
        lut::kBsdlDielectricBothRoughnessCount - 1);
    const int roughness1 = std::min(
        roughness0 + 1,
        lut::kBsdlDielectricBothRoughnessCount - 1);
    const float roughnessT =
        roughnessCoord - static_cast<float>(roughness0);

    const float cosCoord = c * static_cast<float>(
        lut::kBsdlDielectricBothCosThetaCount - 1);
    const int cos0 = std::clamp(
        static_cast<int>(cosCoord),
        0,
        lut::kBsdlDielectricBothCosThetaCount - 1);
    const int cos1 =
        std::min(cos0 + 1, lut::kBsdlDielectricBothCosThetaCount - 1);
    const float cosT = cosCoord - static_cast<float>(cos0);

    const float* values = backfacing
        ? lut::kBsdlDielectricBothBackMissingEnergy
        : lut::kBsdlDielectricBothFrontMissingEnergy;
    const auto lookup = [&](int i, int r, int cosine) {
        const int index =
            (i * lut::kBsdlDielectricBothRoughnessCount + r) *
                lut::kBsdlDielectricBothCosThetaCount +
            cosine;
        return values[index];
    };
    const auto lerpCos = [&](int i, int r) {
        return lookup(i, r, cos0) * (1.0f - cosT) +
            lookup(i, r, cos1) * cosT;
    };
    const auto lerpRoughness = [&](int i) {
        return lerpCos(i, roughness0) * (1.0f - roughnessT) +
            lerpCos(i, roughness1) * roughnessT;
    };

    return _Clamp01(
        lerpRoughness(ior0) * (1.0f - iorT) +
        lerpRoughness(ior1) * iorT);
}

inline float
_LookupBsdlDielectricTransmissionSingleScatterAlbedo(
    float cosTheta,
    float perceptualRoughness,
    float ior,
    bool backfacing)
{
    namespace lut = bsdf_luts;

    const float c = _Clamp01(cosTheta);
    const float roughness = _Clamp01(perceptualRoughness);
    const float clampedIor = std::clamp(
        ior,
        _kBsdlDielectricIorMin,
        _kBsdlDielectricIorMax);

    const float iorIndex = std::sqrt(
        (clampedIor - _kBsdlDielectricIorMin) /
        (_kBsdlDielectricIorMax - _kBsdlDielectricIorMin));
    const float iorCoord = iorIndex * static_cast<float>(
        lut::kBsdlDielectricTransmissionIorCount - 1);
    const int ior0 = std::clamp(
        static_cast<int>(iorCoord),
        0,
        lut::kBsdlDielectricTransmissionIorCount - 1);
    const int ior1 = std::min(
        ior0 + 1, lut::kBsdlDielectricTransmissionIorCount - 1);
    const float iorT = iorCoord - static_cast<float>(ior0);

    // Transmission rows use quadratic perceptual-roughness spacing so the
    // narrow lobe and critical-angle transition remain resolved near smooth.
    const float roughnessCoord = std::sqrt(roughness) * static_cast<float>(
        lut::kBsdlDielectricTransmissionRoughnessCount - 1);
    const int roughness0 = std::clamp(
        static_cast<int>(roughnessCoord),
        0,
        lut::kBsdlDielectricTransmissionRoughnessCount - 1);
    const int roughness1 = std::min(
        roughness0 + 1,
        lut::kBsdlDielectricTransmissionRoughnessCount - 1);
    const float roughnessT =
        roughnessCoord - static_cast<float>(roughness0);

    const float cosCoord = c * static_cast<float>(
        lut::kBsdlDielectricTransmissionCosThetaCount - 1);
    const int cos0 = std::clamp(
        static_cast<int>(cosCoord),
        0,
        lut::kBsdlDielectricTransmissionCosThetaCount - 1);
    const int cos1 = std::min(
        cos0 + 1,
        lut::kBsdlDielectricTransmissionCosThetaCount - 1);
    const float cosT = cosCoord - static_cast<float>(cos0);

    const float* values = backfacing
        ? lut::kBsdlDielectricTransmissionBackSingleScatterAlbedo
        : lut::kBsdlDielectricTransmissionFrontSingleScatterAlbedo;
    const auto lookup = [&](int i, int r, int cosine) {
        const int index =
            (i * lut::kBsdlDielectricTransmissionRoughnessCount + r) *
                lut::kBsdlDielectricTransmissionCosThetaCount +
            cosine;
        return values[index];
    };
    const auto lerpCos = [&](int i, int r) {
        return lookup(i, r, cos0) * (1.0f - cosT) +
            lookup(i, r, cos1) * cosT;
    };
    const auto lerpRoughness = [&](int i) {
        return lerpCos(i, roughness0) * (1.0f - roughnessT) +
            lerpCos(i, roughness1) * roughnessT;
    };

    return _Clamp01(
        lerpRoughness(ior0) * (1.0f - iorT) +
        lerpRoughness(ior1) * iorT);
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

inline float
_ResolveDielectricIor(
    const Bsdf::DielectricInterfaceData& data,
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
        std::clamp(alpha[0], _kMinMicrofacetAlpha, 1.0f),
        std::clamp(alpha[1], _kMinMicrofacetAlpha, 1.0f));
}

inline float
_AverageAlphaForEnergy(const Vec2f& alpha)
{
    const float clampedX = std::clamp(alpha[0], _kMinMicrofacetAlpha, 1.0f);
    const float clampedY = std::clamp(alpha[1], _kMinMicrofacetAlpha, 1.0f);
    return std::sqrt(clampedX * clampedY);
}

inline float
_AverageAlphaAsRoughness(const Vec2f& alpha)
{
    return std::sqrt(_AverageAlphaForEnergy(alpha));
}

inline float
_BsdlLayerRoughnessFromAlpha(const Vec2f& alpha)
{
    const float alphaX = std::clamp(alpha[0], _kMinMicrofacetAlpha, 1.0f);
    const float alphaY = std::clamp(alpha[1], _kMinMicrofacetAlpha, 1.0f);
    return std::sqrt((std::max(alphaX, alphaY) +
                      std::min(alphaX, alphaY)) * 0.5f);
}

inline bool
_IsEffectivelyIsotropic(const Vec2f& roughness)
{
    return std::abs(roughness[0] - roughness[1]) < 1.0e-6f;
}

inline bool
_IsEffectivelyDeltaAlpha(const Vec2f& alpha)
{
    // Match pbrt-v4's TrowbridgeReitzDistribution::EffectivelySmooth().
    // Below this alpha, finite GGX is numerically valid but produces severe
    // near-mirror indirect-light variance in a unidirectional path tracer.
    return std::max(alpha[0], alpha[1]) <
        _kEffectivelySmoothMicrofacetAlpha;
}

inline Vec3f
_LerpVec(const Vec3f& a, const Vec3f& b, float t)
{
    return a * (1.0f - t) + b * t;
}

inline Vec3f
_MirrorAcrossSurface(const Vec3f& v, const Vec3f& normal)
{
    return v - 2.0f * Dot(v, normal) * normal;
}

inline Vec3f
_ThinWalledWindowReflectance(const Vec3f& frontReflectance)
{
    const Vec3f r(
        _Clamp01(frontReflectance[0]),
        _Clamp01(frontReflectance[1]),
        _Clamp01(frontReflectance[2]));
    return Vec3f(
        (2.0f * r[0]) / (1.0f + r[0]),
        (2.0f * r[1]) / (1.0f + r[1]),
        (2.0f * r[2]) / (1.0f + r[2]));
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

inline float
_MaterialXDielectricFresnel(float cosTheta, float ior)
{
    const float c = _Clamp01(cosTheta);
    const float eta = std::max(ior, _kEpsilon);
    const float g2 = eta * eta + c * c - 1.0f;
    if (g2 < 0.0f) {
        return 1.0f;
    }

    const float g = std::sqrt(g2);
    const float sNumer = g - c;
    const float sDenom = std::max(g + c, _kEpsilon);
    const float pNumer = (g + c) * c - 1.0f;
    const float pDenom = (g - c) * c + 1.0f;
    const float s = sNumer / sDenom;
    const float p = pNumer / std::copysign(
        std::max(std::abs(pDenom), _kEpsilon), pDenom);
    return _Clamp01(0.5f * s * s * (1.0f + p * p));
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
    const float clampedAlpha =
        std::clamp(alpha, _kMinMicrofacetAlpha, 1.0f);
    const float clampedNdotH = std::clamp(NdotH, 0.0f, 1.0f);
    const float a2 = clampedAlpha * clampedAlpha;
    const float nDotH2 = clampedNdotH * clampedNdotH;
    // Avoid cancellation in NdotH^2 * (a2 - 1) + 1 for mirror-like lobes.
    const float denom = (1.0f - nDotH2) + a2 * nDotH2;
    const float denom2 = denom * denom;
    if (!std::isfinite(denom2) || denom2 <= 0.0f) {
        return 0.0f;
    }
    return a2 / (kPi * denom2);
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
    return std::max(4.0f * NdotV * NdotL * _GGX_V(alpha, NdotV, NdotL),
                    0.0f);
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

constexpr float _kFonConstantA = 0.5f - 2.0f / (3.0f * kPi);
constexpr float _kFonConstantB = 2.0f / 3.0f - 28.0f / (15.0f * kPi);

inline float
_FonDirectionalAlbedoApprox(float mu, float roughness)
{
    const float clampedMu = _Clamp01(mu);
    const float muComp = 1.0f - clampedMu;
    constexpr float g1 = 0.0571085289f;
    constexpr float g2 = 0.491881867f;
    constexpr float g3 = -0.332181442f;
    constexpr float g4 = 0.0714429953f;
    const float gOverPi =
        muComp * (g1 + muComp * (g2 + muComp * (g3 + muComp * g4)));
    return (1.0f + roughness * gOverPi) /
           (1.0f + _kFonConstantA * roughness);
}

inline Vec3f
_EvalEonDiffuse(
    const Vec3f& color,
    float roughness,
    float NdotV,
    float NdotL,
    float LdotV)
{
    const float r = _Clamp01(roughness);
    if (r <= 0.0f) {
        return color * kInvPi;
    }

    const float muI = std::max(NdotL, _kEpsilon);
    const float muO = std::max(NdotV, _kEpsilon);
    const float s = LdotV - muI * muO;
    const float sOverF = (s > 0.0f) ? s / std::max(muI, muO) : s;
    const float aF = 1.0f / (1.0f + _kFonConstantA * r);
    const Vec3f fSS = color * (kInvPi * aF * (1.0f + r * sOverF));

    const float eFi = _FonDirectionalAlbedoApprox(muI, r);
    const float eFo = _FonDirectionalAlbedoApprox(muO, r);
    const float avgEF = aF * (1.0f + _kFonConstantB * r);
    const Vec3f rhoMS = CompDiv(
        CompMul(color, color) * avgEF,
        Vec3f(1.0f) - color * (1.0f - avgEF));
    const float fMSScale =
        kInvPi *
        std::max(_kEpsilon, 1.0f - eFi) *
        std::max(_kEpsilon, 1.0f - eFo) /
        std::max(_kEpsilon, 1.0f - avgEF);

    return fSS + rhoMS * fMSScale;
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
    return (std::isfinite(denom) && denom > 0.0f) ? 1.0f / denom : 0.0f;
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
_IsGgxMicrofacetMultipleScatteringEnabled()
{
    return _gGgxMicrofacetMultipleScatteringEnabled.load(
        std::memory_order_relaxed);
}

inline float
_AverageFresnelDielectric(float eta)
{
    if (eta < 1.0f) {
        return 0.997118f +
            eta * (0.1014f + eta * (-0.965241f - eta * 0.130607f));
    }
    return (eta - 1.0f) / (4.08567f + 1.00071f * eta);
}

float
_AverageBsdlDielectricBothMissingEnergy(
    float perceptualRoughness,
    float ior,
    bool backfacing)
{
    // Four-point Gauss-Legendre integration of the cosine-weighted average
    // 2 * integral(E(c) * c, c=0..1). The factor of two cancels the interval
    // transform, leaving the standard quadrature weights below.
    constexpr float cosTheta[4] = {
        0.0694318442f, 0.3300094782f, 0.6699905218f, 0.9305681558f};
    constexpr float weight[4] = {
        0.1739274226f, 0.3260725774f, 0.3260725774f, 0.1739274226f};
    float average = 0.0f;
    for (int i = 0; i < 4; ++i) {
        average += weight[i] * cosTheta[i] *
            _LookupBsdlDielectricBothMissingEnergy(
                cosTheta[i], perceptualRoughness, ior, backfacing);
    }
    return _Clamp01(average);
}

struct _CoupledDielectricCompensation
{
    float missingEnergy = 0.0f;
    float reflectionRatio = 0.0f;
};

inline _CoupledDielectricCompensation
_BsdlCoupledDielectricCompensation(
    float cosThetaO,
    float perceptualRoughness,
    float ior,
    bool backfacing)
{
    if (!_IsGgxMicrofacetMultipleScatteringEnabled() ||
        perceptualRoughness < std::sqrt(_kTurquinMicrofacetMsMinAlpha)) {
        return {};
    }

    const float missingEnergy = _LookupBsdlDielectricBothMissingEnergy(
        cosThetaO,
        perceptualRoughness,
        ior,
        backfacing);
    if (missingEnergy <= 0.0f) {
        return {};
    }

    const float eta = backfacing
        ? 1.0f / std::max(ior, _kEpsilon)
        : std::max(ior, _kEpsilon);
    const float ratioFront = _Clamp01(_AverageFresnelDielectric(eta));
    const float ratioBack = _Clamp01(_AverageFresnelDielectric(1.0f / eta));
    const float averageCurrent = _AverageBsdlDielectricBothMissingEnergy(
        perceptualRoughness, ior, backfacing);
    const float averageOpposite = _AverageBsdlDielectricBothMissingEnergy(
        perceptualRoughness, ior, !backfacing);
    const float left = (1.0f - ratioFront) /
        std::max(averageOpposite, _kEpsilon);
    const float right = (1.0f - ratioBack) /
        std::max(averageCurrent, _kEpsilon) * eta * eta;
    const float x = right > 1.0e12f
        ? 1.0f
        : right / std::max(left + right, _kEpsilon);
    const float reflectionRatio = _Clamp01(
        1.0f - x * (1.0f - ratioFront));
    return {missingEnergy, reflectionRatio};
}

inline Bsdf::DielectricLayerThroughputMode
_GetDielectricLayerThroughputMode()
{
    const auto mode = static_cast<Bsdf::DielectricLayerThroughputMode>(
        _gDielectricLayerThroughputMode.load(std::memory_order_relaxed));
    switch (mode) {
        case Bsdf::DielectricLayerThroughputMode::Bsdl:
        case Bsdf::DielectricLayerThroughputMode::MaterialXGlsl:
            return mode;
    }
    return Bsdf::DielectricLayerThroughputMode::Bsdl;
}

inline Vec3f
_TurquinMicrofacetMsScale(
    float alphaRoughness,
    float cosThetaO,
    const Vec3f& fresnel)
{
    if (!_IsGgxMicrofacetMultipleScatteringEnabled() ||
        alphaRoughness < _kTurquinMicrofacetMsMinAlpha) {
        return Vec3f(1.0f);
    }

    const float missingEnergy =
        _LookupGgxMissingEnergy(cosThetaO, alphaRoughness);
    const float singleScatterEnergy = std::max(0.01f, 1.0f - missingEnergy);
    const float missingToSingle = missingEnergy / singleScatterEnergy;

    // Turquin compensation keeps the primary lobe shape and scales it by the
    // outgoing-direction missing energy. This is intentionally not reciprocal.
    return Vec3f(1.0f) +
        _SaturateVec(fresnel) * missingToSingle;
}

inline Vec3f
_TurquinDirectionalReflectance(
    float alphaRoughness,
    float cosThetaO,
    const Vec3f& fresnel)
{
    if (!_IsGgxMicrofacetMultipleScatteringEnabled() ||
        alphaRoughness < _kTurquinMicrofacetMsMinAlpha) {
        return _SaturateVec(fresnel);
    }

    const float missingEnergy =
        _LookupGgxMissingEnergy(cosThetaO, alphaRoughness);
    const float singleScatterEnergy = 1.0f - missingEnergy;
    const Vec3f F = _SaturateVec(fresnel);
    return _SaturateVec(
        F * singleScatterEnergy +
        CompMul(F, F) * missingEnergy);
}

inline Vec3f
_LayerThroughputReflectance(
    float alphaRoughness,
    float cosThetaO,
    const Vec3f& fresnel)
{
    return _TurquinDirectionalReflectance(alphaRoughness, cosThetaO, fresnel);
}

inline float
_ClampFinite01(float x)
{
    return std::isfinite(x) ? _Clamp01(x) : 0.0f;
}

inline Vec2f
_MaterialXGgxDirAlbedoAnalyticAB(float NdotV, float alpha)
{
    const float x = _Clamp01(NdotV);
    const float y = _Clamp01(alpha);
    const float x2 = x * x;
    const float y2 = y * y;
    const float xy = x * y;
    const float x2y = x2 * y;
    const float xy2 = x * y2;
    const float x2y2 = x2 * y2;

    const float r0 =
        0.1003f +
        (-0.6303f * x) +
        (9.748f * y) +
        (-2.038f * xy) +
        (29.34f * x2) +
        (-8.245f * y2) +
        (-26.44f * x2y) +
        (19.99f * xy2) +
        (-5.448f * x2y2);
    const float r1 =
        0.9345f +
        (-2.323f * x) +
        (2.229f * y) +
        (-3.748f * xy) +
        (1.424f * x2) +
        (-0.7684f * y2) +
        (1.436f * x2y) +
        (0.2913f * xy2) +
        (0.6286f * x2y2);
    const float r2 =
        1.0f +
        (-1.765f * x) +
        (8.263f * y) +
        (11.53f * xy) +
        (28.96f * x2) +
        (-7.507f * y2) +
        (-36.11f * x2y) +
        (15.86f * xy2) +
        (33.37f * x2y2);
    const float r3 =
        1.0f +
        (0.2281f * x) +
        (15.94f * y) +
        (-55.83f * xy) +
        (13.08f * x2) +
        (41.26f * y2) +
        (54.9f * x2y) +
        (300.2f * xy2) +
        (-285.1f * x2y2);

    const float ab0 = r0 / std::copysign(
        std::max(std::abs(r2), _kEpsilon), r2);
    const float ab1 = r1 / std::copysign(
        std::max(std::abs(r3), _kEpsilon), r3);
    return Vec2f(_ClampFinite01(ab0), _ClampFinite01(ab1));
}

inline Vec3f
_MaterialXGgxDirAlbedoAnalytic(
    float NdotV,
    float alpha,
    const Vec3f& F0,
    const Vec3f& F90)
{
    const Vec2f ab = _MaterialXGgxDirAlbedoAnalyticAB(NdotV, alpha);
    return F0 * ab[0] + F90 * ab[1];
}

inline Vec3f
_MaterialXGgxEnergyCompensation(
    float NdotV,
    float alpha,
    const Vec3f& singleScatterFresnel)
{
    const float Ess = std::max(
        _MaterialXGgxDirAlbedoAnalytic(
            NdotV, alpha, Vec3f(1.0f), Vec3f(1.0f))[0],
        _kEpsilon);
    return Vec3f(1.0f) +
        singleScatterFresnel * ((1.0f - Ess) / Ess);
}

inline Vec3f
_MaterialXGlslDielectricLayerReflectance(
    float alphaRoughness,
    float cosThetaO,
    float ior)
{
    float F0 = (ior - 1.0f) / (ior + 1.0f);
    F0 *= F0;
    const Vec3f F(_MaterialXDielectricFresnel(cosThetaO, ior));
    return CompMul(
        _MaterialXGgxDirAlbedoAnalytic(
            cosThetaO,
            alphaRoughness,
            Vec3f(F0),
            Vec3f(1.0f)),
        _MaterialXGgxEnergyCompensation(cosThetaO, alphaRoughness, F));
}

inline bool
_HasThinFilm(float weight, float thickness, float /*ior*/)
{
    return weight > _kEpsilon &&
           thickness > _kEpsilon;
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
    const Vec3f eta2Squared = CompMul(eta2, eta2);
    const Vec3f sinThetaSqr(1.0f - cosTheta * cosTheta);
    const Vec3f A =
        eta2Squared * (Vec3f(1.0f) - CompMul(k2, k2)) -
        eta1 * eta1 * sinThetaSqr;
    const Vec3f twoEta2SquaredK2 = 2.0f * CompMul(eta2Squared, k2);
    const Vec3f B = _SqrtVec(
        CompMul(A, A) + CompMul(twoEta2SquaredK2, twoEta2SquaredK2));
    const Vec3f U = _SqrtVec((A + B) * 0.5f);
    const Vec3f V = _MaxVec(_SqrtVec((B - A) * 0.5f), 0.0f);

    *phiS = Vec3f(
        std::atan2(2.0f * eta1 * V[0] * cosTheta,
                   U[0] * U[0] + V[0] * V[0] - eta1 * eta1 * cosTheta * cosTheta),
        std::atan2(2.0f * eta1 * V[1] * cosTheta,
                   U[1] * U[1] + V[1] * V[1] - eta1 * eta1 * cosTheta * cosTheta),
        std::atan2(2.0f * eta1 * V[2] * cosTheta,
                   U[2] * U[2] + V[2] * V[2] - eta1 * eta1 * cosTheta * cosTheta));

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
_FresnelConductor(
    float cosTheta,
    const Vec3f& ior,
    const Vec3f& extinction)
{
    Vec3f Rp(0.0f), Rs(0.0f);
    _FresnelConductorPolarized(cosTheta, ior, extinction, &Rp, &Rs);
    return _SaturateVec((Rp + Rs) * 0.5f);
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
_DielectricInterfaceReflectanceUntinted(
    const Bsdf::DielectricInterfaceData& data,
    float cosTheta,
    float effectiveIor,
    bool backside)
{
    // The interface models MaterialX dielectric_bsdf, whose reference
    // implementation evaluates the exact dielectric Fresnel equations
    // (mx_fresnel_dielectric).  Schlick fits the outside curve but badly
    // underestimates reflectance from inside the medium near the critical
    // angle, so pick the relative eta by side.  Thin-walled sheets respond
    // symmetrically, so they always use the outside eta.
    const float safeIor = std::max(effectiveIor, 1.0f + 1.0e-4f);
    const float eta = (backside && !data.thinWalled)
        ? 1.0f / safeIor
        : safeIor;
    const Vec2f polarized = _FresnelDielectricPolarized(cosTheta, eta);
    const Vec3f baseReflectance(
        _Clamp01(0.5f * (polarized[0] + polarized[1])));
    _ThinFilmParams thinFilm;
    thinFilm.model = _ThinFilmModel::Dielectric;
    thinFilm.ior = Vec3f(std::max(effectiveIor, 1.0f));
    thinFilm.tint = Vec3f(1.0f);
    const Vec3f reflectance = _ApplyThinFilm(
        baseReflectance,
        cosTheta,
        data.thinFilmWeight,
        data.thinFilmThickness,
        data.thinFilmIor,
        thinFilm);
    return data.thinWalled
        ? _ThinWalledWindowReflectance(reflectance)
        : reflectance;
}

inline Vec3f
_DielectricInterfaceReflectionCoefficient(
    const Bsdf::DielectricInterfaceData& data,
    float cosTheta,
    float effectiveIor,
    bool backside)
{
    return CompMul(
        _DielectricInterfaceReflectanceUntinted(
            data, cosTheta, effectiveIor, backside),
        _SafeVec(data.reflectionTint)) * _Clamp01(data.reflectionWeight);
}

inline Vec3f
_DielectricInterfaceTransmissionCoefficient(
    const Bsdf::DielectricInterfaceData& data,
    float cosTheta,
    float effectiveIor,
    bool backside)
{
    return CompMul(
        Vec3f(1.0f) -
            _DielectricInterfaceReflectanceUntinted(
                data, cosTheta, effectiveIor, backside),
        _SafeVec(data.transmissionTint)) * _Clamp01(data.transmissionWeight);
}

struct _DielectricInterfaceSelection
{
    float reflection = 0.0f;
    float transmission = 0.0f;
};

inline _DielectricInterfaceSelection
_DielectricInterfaceSelectionProbabilities(
    const Bsdf::DielectricInterfaceData& data,
    float cosTheta,
    float effectiveIor,
    bool backside)
{
    const float reflectionWeight = std::max(
        _Luminance(_DielectricInterfaceReflectionCoefficient(
            data, cosTheta, effectiveIor, backside)),
        0.0f);
    const float transmissionWeight = std::max(
        _Luminance(_DielectricInterfaceTransmissionCoefficient(
            data, cosTheta, effectiveIor, backside)),
        0.0f);
    const float total = reflectionWeight + transmissionWeight;
    if (total <= 0.0f) {
        return {};
    }
    return {reflectionWeight / total, transmissionWeight / total};
}

inline Vec3f
_ConductorReflectionFresnel(
    const Bsdf::ConductorData& data,
    float cosTheta)
{
    const Vec3f baseReflectance = _FresnelConductor(
        cosTheta,
        _MaxVec(data.ior, 0.0f),
        _MaxVec(data.extinction, 0.0f));
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

template<typename DataT>
inline Vec3f
_ResolveReflectionNormal(const DataT& data,
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
    float weight,
    const Vec3f& N,
    const Vec3f& wi,
    const Vec3f& wo,
    bool compensateMissingEnergy = true)
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
    const Vec3f compensatedFresnel = compensateMissingEnergy
        ? CompMul(
              fresnel,
              _TurquinMicrofacetMsScale(
                  _AverageAlphaForEnergy(alpha), cosThetaO, fresnel)) * weight
        : fresnel * weight;
    return _SafeVec(CompMul(
        compensatedFresnel,
        Vec3f(D * G / std::max(4.0f * cosThetaI * cosThetaO, _kEpsilon))));
}

Vec3f
_EvalMicrofacetReflectionIsotropic(
    float alpha,
    const Vec3f& fresnel,
    float weight,
    const Vec3f& N,
    const Vec3f& wi,
    const Vec3f& wo,
    bool compensateMissingEnergy = true)
{
    const float clampedAlpha =
        std::clamp(alpha, _kMinMicrofacetAlpha, 1.0f);
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
    const Vec3f compensatedFresnel = compensateMissingEnergy
        ? CompMul(
              fresnel,
              _TurquinMicrofacetMsScale(clampedAlpha, NdotV, fresnel)) * weight
        : fresnel * weight;
    return _SafeVec(CompMul(
        compensatedFresnel,
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

inline bool
_UsesCoupledRoughDielectricSampling(
    const Bsdf::DielectricInterfaceData& data)
{
    return data.compensateCoupledDielectric &&
        !data.thinWalled &&
        data.reflectionWeight > 0.0f &&
        data.transmissionWeight > 0.0f &&
        !_IsEffectivelyDeltaAlpha(data.roughness);
}

inline bool
_IsCoupledTransmissionInterfaceForStraightShadow(
    const Bsdf::DielectricInterfaceData& data)
{
    const bool hasThinFilm =
        data.thinFilmWeight > _kEpsilon &&
        data.thinFilmThickness > _kEpsilon;
    return data.compensateCoupledDielectric &&
        !data.thinWalled &&
        data.reflectionWeight > 0.0f &&
        data.transmissionWeight > 0.0f &&
        !hasThinFilm;
}

inline bool
_CollectCoupledTransmissionInterfaceForStraightShadow(
    const Bsdf::ClosureTree& tree,
    Bsdf::NodeId nodeId,
    const Bsdf::DielectricInterfaceData** result)
{
    const Bsdf::Node* const node = tree.Get(nodeId);
    if (!node) {
        return true;
    }

    return std::visit(
        [&](const auto& data) -> bool {
            using T = std::decay_t<decltype(data)>;
            if constexpr (std::is_same_v<T, Bsdf::DielectricInterfaceData>) {
                if (!_IsCoupledTransmissionInterfaceForStraightShadow(
                        data)) {
                    return true;
                }
                if (*result) {
                    return false;
                }
                *result = &data;
                return true;
            } else if constexpr (std::is_same_v<T, Bsdf::MixData>) {
                if (data.mix <= _kEpsilon) {
                    return _CollectCoupledTransmissionInterfaceForStraightShadow(
                        tree, data.bg, result);
                }
                if (data.mix >= 1.0f - _kEpsilon) {
                    return _CollectCoupledTransmissionInterfaceForStraightShadow(
                        tree, data.fg, result);
                }
                return _CollectCoupledTransmissionInterfaceForStraightShadow(
                           tree, data.fg, result) &&
                    _CollectCoupledTransmissionInterfaceForStraightShadow(
                        tree, data.bg, result);
            } else if constexpr (std::is_same_v<T, Bsdf::LayerData>) {
                return _CollectCoupledTransmissionInterfaceForStraightShadow(
                           tree, data.top, result) &&
                    _CollectCoupledTransmissionInterfaceForStraightShadow(
                        tree, data.base, result);
            } else if constexpr (std::is_same_v<T, Bsdf::AddData>) {
                return _CollectCoupledTransmissionInterfaceForStraightShadow(
                           tree, data.in1, result) &&
                    _CollectCoupledTransmissionInterfaceForStraightShadow(
                        tree, data.in2, result);
            } else if constexpr (std::is_same_v<T, Bsdf::MultiplyData>) {
                return _CollectCoupledTransmissionInterfaceForStraightShadow(
                    tree, data.input, result);
            } else {
                return true;
            }
        },
        node->data);
}

inline const Bsdf::DielectricInterfaceData*
_FindCoupledTransmissionInterfaceForStraightShadow(
    const SurfaceClosure& closure)
{
    if (!closure.HasBsdfTree()) {
        return nullptr;
    }
    const Bsdf::DielectricInterfaceData* result = nullptr;
    return _CollectCoupledTransmissionInterfaceForStraightShadow(
               closure.bsdfTree, closure.bsdfTree.root, &result)
        ? result
        : nullptr;
}

inline _CoupledDielectricCompensation
_GetCoupledDielectricCompensation(
    const Bsdf::DielectricInterfaceData& data,
    float effectiveIor,
    const Vec3f& geometricN,
    const Vec3f& shadingN,
    const Vec3f& wo)
{
    if (!_UsesCoupledRoughDielectricSampling(data)) {
        return {};
    }
    return _BsdlCoupledDielectricCompensation(
        std::max(std::abs(Dot(shadingN, wo)), _kEpsilon),
        _BsdlLayerRoughnessFromAlpha(data.roughness),
        effectiveIor,
        Dot(geometricN, wo) < 0.0f);
}

Vec3f
_EvalCoupledRoughDielectricTransmission(
    const Bsdf::DielectricInterfaceData& data,
    float effectiveIor,
    const Vec3f& geometricN,
    const Vec3f& shadingN,
    const Vec3f& wi,
    const Vec3f& wo)
{
    const _Frame frame = _Frame::FromNormalAndTangent(
        shadingN, data.tangent);
    const Vec3f woLocal = frame.ToLocal(wo);
    const Vec3f wiLocal = frame.ToLocal(wi);
    const float cosThetaO = woLocal[2];
    const float cosThetaI = -wiLocal[2];
    if (cosThetaO <= _kEpsilon || cosThetaI <= _kEpsilon) {
        return Vec3f(0.0f);
    }

    const bool backside = Dot(geometricN, wo) < 0.0f;
    const float etap = backside
        ? 1.0f / std::max(effectiveIor, _kEpsilon)
        : std::max(effectiveIor, _kEpsilon);
    Vec3f wmLocal = wiLocal * etap + woLocal;
    if (wmLocal.length() < _kEpsilon) {
        return Vec3f(0.0f);
    }
    wmLocal.normalize();
    if (wmLocal[2] < 0.0f) {
        wmLocal = -wmLocal;
    }

    const float cosMO = Dot(woLocal, wmLocal);
    const float cosMI = Dot(wiLocal, wmLocal);
    if (cosMO <= _kEpsilon || cosMI >= -_kEpsilon) {
        return Vec3f(0.0f);
    }
    const float denom = cosMI + cosMO / etap;
    const float denom2 = denom * denom;
    if (denom2 <= _kEpsilon) {
        return Vec3f(0.0f);
    }

    const Vec2f alpha = _ClampAlpha(data.roughness);
    const float D = _GGX_D_Anisotropic(alpha, wmLocal);
    const Vec3f wiReflectionSide(
        wiLocal[0], wiLocal[1], -wiLocal[2]);
    const float G = _GGX_G_Anisotropic(
        alpha, woLocal, wiReflectionSide);
    const Vec3f transmission =
        _DielectricInterfaceTransmissionCoefficient(
            data, cosMO, effectiveIor, backside);
    const float scale = D * G * std::abs(cosMO * cosMI) /
        std::max(cosThetaO * cosThetaI * denom2, _kEpsilon);
    return _SafeVec(transmission * scale);
}

float
_PdfCoupledRoughDielectric(
    const Bsdf::DielectricInterfaceData& data,
    float effectiveIor,
    const Vec3f& geometricN,
    const Vec3f& shadingN,
    const Vec3f& wi,
    const Vec3f& wo)
{
    const _Frame frame = _Frame::FromNormalAndTangent(
        shadingN, data.tangent);
    const Vec3f woLocal = frame.ToLocal(wo);
    const Vec3f wiLocal = frame.ToLocal(wi);
    if (woLocal[2] <= 0.0f || std::abs(wiLocal[2]) <= _kEpsilon) {
        return 0.0f;
    }

    const bool backside = Dot(geometricN, wo) < 0.0f;
    const bool reflection = _IsSameSide(geometricN, wi, wo);
    const float etap = backside
        ? 1.0f / std::max(effectiveIor, _kEpsilon)
        : std::max(effectiveIor, _kEpsilon);

    Vec3f wmLocal = reflection
        ? wiLocal + woLocal
        : wiLocal * etap + woLocal;
    if (wmLocal.length() < _kEpsilon) {
        return 0.0f;
    }
    wmLocal.normalize();
    if (wmLocal[2] < 0.0f) {
        wmLocal = -wmLocal;
    }

    const float cosMO = std::abs(Dot(woLocal, wmLocal));
    if (cosMO <= _kEpsilon) {
        return 0.0f;
    }
    const _DielectricInterfaceSelection selection =
        _DielectricInterfaceSelectionProbabilities(
            data, cosMO, effectiveIor, backside);
    const Vec2f alpha = _ClampAlpha(data.roughness);
    const float pdfWm = _PdfGGX_VNDF_Anisotropic(
        woLocal, wmLocal, alpha);
    float specularPdf = 0.0f;
    if (reflection) {
        specularPdf = selection.reflection * pdfWm / (4.0f * cosMO);
    } else if (Dot(wiLocal, wmLocal) * Dot(woLocal, wmLocal) < 0.0f) {
        const float denom =
            Dot(wiLocal, wmLocal) + Dot(woLocal, wmLocal) / etap;
        const float denom2 = denom * denom;
        if (denom2 > _kEpsilon) {
            const float dwmDwi =
                std::abs(Dot(wiLocal, wmLocal)) / denom2;
            specularPdf = selection.transmission * pdfWm * dwmDwi;
        }
    }

    const _CoupledDielectricCompensation compensation =
        _GetCoupledDielectricCompensation(
            data, effectiveIor, geometricN, shadingN, wo);
    const float specularProbability = 1.0f - compensation.missingEnergy;
    const float compensationSideRatio = reflection
        ? compensation.reflectionRatio
        : 1.0f - compensation.reflectionRatio;
    const float compensationPdf = compensation.missingEnergy *
        compensationSideRatio *
        _CosineHemispherePdf(std::abs(wiLocal[2]));
    return specularProbability * specularPdf + compensationPdf;
}

Bsdf::BsdfSample
_SampleCoupledRoughDielectric(
    const Bsdf::DielectricInterfaceData& data,
    float effectiveIor,
    const Vec3f& geometricN,
    const Vec3f& shadingN,
    const Vec3f& wo,
    float u1,
    float u2,
    float uChoice)
{
    const _Frame frame = _Frame::FromNormalAndTangent(
        shadingN, data.tangent);
    const Vec3f woLocal = frame.ToLocal(wo);
    if (woLocal[2] <= 0.0f) {
        return Bsdf::BsdfSample{
            Vec3f(0.0f), Vec3f(0.0f), 0.0f, false};
    }

    const bool backside = Dot(geometricN, wo) < 0.0f;
    const _CoupledDielectricCompensation compensation =
        _GetCoupledDielectricCompensation(
            data, effectiveIor, geometricN, shadingN, wo);
    const float specularProbability = 1.0f - compensation.missingEnergy;
    if (compensation.missingEnergy > 0.0f && u1 >= specularProbability) {
        const float remappedU1 = (u1 - specularProbability) /
            compensation.missingEnergy;
        Vec3f wiLocal = _SampleCosineHemisphere(remappedU1, u2);
        const bool sampleReflection =
            uChoice < compensation.reflectionRatio;
        if (!sampleReflection) {
            wiLocal[2] = -wiLocal[2];
        }
        Bsdf::BsdfSample sample{
            frame.ToWorld(wiLocal), Vec3f(0.0f), 1.0f, false};
        sample.eta = sampleReflection
            ? 1.0f
            : (backside
                ? std::max(effectiveIor, _kEpsilon)
                : 1.0f / std::max(effectiveIor, _kEpsilon));
        return sample;
    }

    if (specularProbability <= 0.0f) {
        return Bsdf::BsdfSample{
            Vec3f(0.0f), Vec3f(0.0f), 0.0f, false};
    }
    u1 /= specularProbability;
    const Vec2f alpha = _ClampAlpha(data.roughness);
    const Vec3f wmLocal = _SampleGGX_VNDF_Anisotropic(
        woLocal, alpha, u1, u2);
    const float cosMO = std::abs(Dot(woLocal, wmLocal));
    const _DielectricInterfaceSelection selection =
        _DielectricInterfaceSelectionProbabilities(
            data, cosMO, effectiveIor, backside);
    if (selection.reflection + selection.transmission <= 0.0f) {
        return Bsdf::BsdfSample{
            Vec3f(0.0f), Vec3f(0.0f), 0.0f, false};
    }

    const float eta = backside
        ? std::max(effectiveIor, _kEpsilon)
        : 1.0f / std::max(effectiveIor, _kEpsilon);
    const float sin2T = eta * eta *
        std::max(0.0f, 1.0f - cosMO * cosMO);
    const bool totalInternalReflection = sin2T >= 1.0f;
    const bool chooseReflection = totalInternalReflection ||
        uChoice < selection.reflection;

    Vec3f wiLocal;
    if (chooseReflection) {
        wiLocal = 2.0f * Dot(woLocal, wmLocal) * wmLocal - woLocal;
        if (wiLocal[2] <= 0.0f) {
            return Bsdf::BsdfSample{
                Vec3f(0.0f), Vec3f(0.0f), 0.0f, false};
        }
    } else {
        float cosT = std::sqrt(std::max(0.0f, 1.0f - sin2T));
        wiLocal = -eta * woLocal +
            (eta * cosMO - cosT) * wmLocal;
        if (wiLocal.length() < _kEpsilon) {
            return Bsdf::BsdfSample{
                Vec3f(0.0f), Vec3f(0.0f), 0.0f, false};
        }
        wiLocal.normalize();
        if (wiLocal[2] >= 0.0f) {
            return Bsdf::BsdfSample{
                Vec3f(0.0f), Vec3f(0.0f), 0.0f, false};
        }
    }

    Bsdf::BsdfSample sample{
        frame.ToWorld(wiLocal), Vec3f(0.0f), 1.0f, false};
    sample.eta = chooseReflection ? 1.0f : eta;
    return sample;
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
_SampleDeltaTotalInternalReflection(
    float weight,
    const Vec3f& N,
    const Vec3f& wo)
{
    const Vec3f n = Dot(N, wo) >= 0.0f ? N : -N;
    Vec3f wi = 2.0f * Dot(n, wo) * n - wo;
    wi.normalize();
    Bsdf::BsdfSample sample{wi, Vec3f(weight), 1.0f, true};
    sample.eta = 1.0f;
    return sample;
}

inline bool
_WouldTotalInternalReflect(
    float ior,
    const Vec3f& N,
    const Vec3f& wo)
{
    float cosI = Dot(N, wo);
    float eta = 1.0f / std::max(ior, _kEpsilon);
    if (cosI < 0.0f) {
        eta = std::max(ior, _kEpsilon);
        cosI = -cosI;
    }

    return eta * eta * (1.0f - cosI * cosI) >= 1.0f;
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
        return _SampleDeltaTotalInternalReflection(weight, N, wo);
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
_SampleDeltaReflection(
    const Vec3f& reflectance,
    float weight,
    const Vec3f& shadingN,
    const Vec3f& wo)
{
    Vec3f wi = 2.0f * Dot(shadingN, wo) * shadingN - wo;
    wi.normalize();

    Bsdf::BsdfSample sample{
        wi,
        _SafeVec(reflectance * weight),
        1.0f,
        true
    };
    sample.eta = 1.0f;
    return sample;
}

inline Bsdf::BsdfSample
_SampleDeltaDielectricReflection(
    const Bsdf::DielectricData& data,
    float effectiveIor,
    const Vec3f& shadingN,
    const Vec3f& wo)
{
    const float cosTheta =
        std::max(std::abs(Dot(shadingN, wo)), _kEpsilon);
    return _SampleDeltaReflection(
        _DielectricReflectionFresnel(data, cosTheta, effectiveIor),
        data.weight,
        shadingN,
        wo);
}

inline Bsdf::BsdfSample
_SampleDeltaDielectricTransmission(
    const Bsdf::DielectricData& data,
    float effectiveIor,
    float fresnelCos,
    const Vec3f& N,
    const Vec3f& wo)
{
    if (_WouldTotalInternalReflect(effectiveIor, N, wo)) {
        // A transmission-only lobe is paired with a separate reflection lobe
        // that keeps contributing its Schlick reflectance from inside the
        // medium, so a full-weight TIR sample here would double-count
        // reflection and gain energy on every internal bounce.  Carry only
        // the remainder so the reflection/transmission pair totals one.
        const float pairedReflectance = _Clamp01(_Luminance(
            _DielectricReflectionFresnelUntinted(
                data, fresnelCos, effectiveIor)));
        return _SampleDeltaTotalInternalReflection(
            data.weight * (1.0f - pairedReflectance), N, wo);
    }

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

inline Bsdf::BsdfSample
_SampleDeltaDielectricInterfaceReflection(
    const Bsdf::DielectricInterfaceData& data,
    float effectiveIor,
    const Vec3f& shadingN,
    const Vec3f& wo,
    bool backside)
{
    const float cosTheta =
        std::max(std::abs(Dot(shadingN, wo)), _kEpsilon);
    return _SampleDeltaReflection(
        _DielectricInterfaceReflectionCoefficient(
            data, cosTheta, effectiveIor, backside),
        1.0f,
        shadingN,
        wo);
}

inline Bsdf::BsdfSample
_SampleDeltaDielectricInterfaceTransmission(
    const Bsdf::DielectricInterfaceData& data,
    float effectiveIor,
    float fresnelCos,
    const Vec3f& N,
    const Vec3f& wo)
{
    const bool backside = Dot(N, wo) < 0.0f;
    if (data.thinWalled) {
        Vec3f wi = -wo;
        wi.normalize();
        Bsdf::BsdfSample sample{
            wi,
            _DielectricInterfaceTransmissionCoefficient(
                data, fresnelCos, effectiveIor, backside),
            1.0f,
            true
        };
        sample.eta = 1.0f;
        return sample;
    }

    auto sample = _SampleDeltaTransmission(
        effectiveIor,
        data.transmissionTint,
        _Clamp01(data.transmissionWeight),
        N,
        wo);
    const float baseReflectance =
        _SchlickFresnelScalar(effectiveIor, fresnelCos);
    sample.f = CompMul(
        sample.f,
        _TransmissionScale(
            baseReflectance,
            _DielectricInterfaceReflectanceUntinted(
                data, fresnelCos, effectiveIor, backside)));
    return sample;
}

inline Bsdf::BsdfSample
_SampleDeltaConductorReflection(
    const Bsdf::ConductorData& data,
    const Vec3f& shadingN,
    const Vec3f& wo)
{
    const float cosTheta =
        std::max(std::abs(Dot(shadingN, wo)), _kEpsilon);
    return _SampleDeltaReflection(
        _ConductorReflectionFresnel(data, cosTheta),
        data.weight,
        shadingN,
        wo);
}

inline Bsdf::BsdfSample
_SampleDeltaGeneralizedSchlickReflection(
    const Bsdf::GeneralizedSchlickData& data,
    const Vec3f& shadingN,
    const Vec3f& wo)
{
    const float cosTheta =
        std::max(std::abs(Dot(shadingN, wo)), _kEpsilon);
    return _SampleDeltaReflection(
        _GeneralizedSchlickReflectionFresnel(data, cosTheta),
        data.weight,
        shadingN,
        wo);
}

Vec3f
_EvalLegacySurface(
    const SurfaceClosure& c,
    const Vec3f& N,
    const Vec3f& wi,
    const Vec3f& wo)
{
    const Vec3f shadingN = _FaceForwardNormal(N, wo);
    float NdotL = Dot(shadingN, wi);

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
            diffuse = CompMul(kD, Bsdf::EvalLambertian(c.baseColor, shadingN, wi, wo));

            specular = Bsdf::EvalGGXSpecular(
                c.roughness, c.specularIor,
                CompMul(c.specularColor, F0), shadingN, wi, wo);
        } else {
            Vec3f kD = Vec3f(1.0f - c.metallic) * (1.0f - c.transmission);
            diffuse = CompMul(kD, Bsdf::EvalLambertian(c.baseColor, shadingN, wi, wo));
        }

        Vec3f sheen(0.0f);
        if (c.sheen > 0.0f) {
            sheen = Bsdf::EvalSheen(c.sheenColor, c.sheenRoughness, shadingN, wi, wo) *
                    c.sheen;
        }

        Vec3f coatContrib(0.0f);
        float coatAttenuation = 1.0f;
        if (c.coat > 0.0f) {
            coatContrib = Bsdf::EvalCoat(
                c.coat, c.coatRoughness, c.coatIor, shadingN, wi, wo);
            float coatFresnel = _SchlickFresnelScalar(c.coatIor, VdotH);
            coatAttenuation = 1.0f - c.coat * coatFresnel;
        }

        reflected = (diffuse + specular + sheen) * coatAttenuation +
                    coatContrib;
    }

    Vec3f transmitted(0.0f);
    if (c.transmission > 0.0f && NdotL < 0.0f) {
        float absNdotV = std::max(std::abs(Dot(shadingN, wo)), _kEpsilon);
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
    const Vec3f shadingN = _FaceForwardNormal(N, wo);
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
    pdf += pDiffuse * Bsdf::PdfLambertian(shadingN, wi);
    pdf += pSpecular * Bsdf::PdfGGXSpecular(c.roughness, shadingN, wi, wo);
    if (c.coat > 0.0f) {
        pdf += pCoat * Bsdf::PdfGGXSpecular(c.coatRoughness, shadingN, wi, wo);
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
    const Vec3f shadingN = _FaceForwardNormal(N, wo);
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
        auto sample = Bsdf::SampleLambertian(c.baseColor, shadingN, wo, u1, u2);
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
            c.roughness, c.specularIor, specCol, shadingN, wo, u1, u2);
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
            c.coatRoughness, c.coatIor, Vec3f(coatF0), shadingN, wo, u1, u2);
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

float
_PerceptualRoughnessToAlpha(float roughness)
{
    const float clamped = std::clamp(roughness, _kMinMicrofacetAlpha, 1.0f);
    return clamped * clamped;
}

bool
_IsEffectivelySmoothPerceptualRoughness(float roughness)
{
    return _PerceptualRoughnessToAlpha(roughness) <
           _kEffectivelySmoothMicrofacetAlpha;
}

void
_ClearLegacyBsdfSummary(SurfaceClosure* closure)
{
    closure->baseColor = Vec3f(0.0f);
    closure->metallic = 0.0f;
    closure->specular = 0.0f;
    closure->specularColor = Vec3f(0.0f);
    closure->transmission = 0.0f;
    closure->transmissionColor = Vec3f(0.0f);
    closure->coat = 0.0f;
    closure->sheen = 0.0f;
    closure->subsurfaceWeight = 0.0f;
}

class _CausticClassPruner
{
public:
    explicit _CausticClassPruner(const Bsdf::ClosureTree& source)
        : _source(source)
    {
    }

    Bsdf::ClosureTree Run()
    {
        _result.root = _PruneNode(_source.root);
        if (!_result.IsValid(_result.root)) {
            _result.Clear();
        }
        return std::move(_result);
    }

private:
    Bsdf::NodeId _PruneNode(Bsdf::NodeId nodeId)
    {
        const Bsdf::Node* node = _source.Get(nodeId);
        if (!node) {
            return Bsdf::InvalidNodeId;
        }

        return std::visit([&](const auto& data) -> Bsdf::NodeId {
            using T = std::decay_t<decltype(data)>;
            if constexpr (std::is_same_v<T, Bsdf::OrenNayarDiffuseData> ||
                          std::is_same_v<T, Bsdf::BurleyDiffuseData> ||
                          std::is_same_v<T, Bsdf::TranslucentData> ||
                          std::is_same_v<T, Bsdf::SubsurfaceData> ||
                          std::is_same_v<T, Bsdf::SheenData> ||
                          std::is_same_v<T, Bsdf::UnsupportedData>) {
                return _result.Add(data);
            } else if constexpr (std::is_same_v<T, Bsdf::DielectricData>) {
                if (_IsEffectivelyDeltaAlpha(data.roughness) ||
                    data.scatterMode == Bsdf::ScatterMode::Transmission) {
                    return Bsdf::InvalidNodeId;
                }
                Bsdf::DielectricData pruned = data;
                if (pruned.scatterMode ==
                    Bsdf::ScatterMode::ReflectionTransmission) {
                    pruned.scatterMode = Bsdf::ScatterMode::Reflection;
                }
                return _result.Add(pruned);
            } else if constexpr (
                std::is_same_v<T, Bsdf::DielectricInterfaceData>) {
                if (_IsEffectivelyDeltaAlpha(data.roughness)) {
                    return Bsdf::InvalidNodeId;
                }
                Bsdf::DielectricInterfaceData pruned = data;
                pruned.transmissionWeight = 0.0f;
                if (pruned.reflectionWeight <= _kEpsilon) {
                    return Bsdf::InvalidNodeId;
                }
                return _result.Add(pruned);
            } else if constexpr (std::is_same_v<T, Bsdf::ConductorData>) {
                if (_IsEffectivelyDeltaAlpha(data.roughness)) {
                    return Bsdf::InvalidNodeId;
                }
                return _result.Add(data);
            } else if constexpr (
                std::is_same_v<T, Bsdf::GeneralizedSchlickData>) {
                if (_IsEffectivelyDeltaAlpha(data.roughness) ||
                    data.scatterMode == Bsdf::ScatterMode::Transmission) {
                    return Bsdf::InvalidNodeId;
                }
                Bsdf::GeneralizedSchlickData pruned = data;
                if (pruned.scatterMode ==
                    Bsdf::ScatterMode::ReflectionTransmission) {
                    pruned.scatterMode = Bsdf::ScatterMode::Reflection;
                }
                return _result.Add(pruned);
            } else if constexpr (std::is_same_v<T, Bsdf::AdobeOpenPbrData>) {
                Bsdf::AdobeOpenPbrData pruned = data;
                pruned.transmissionWeight = 0.0f;
                if (_IsEffectivelySmoothPerceptualRoughness(
                        pruned.specularRoughness)) {
                    pruned.specularWeight = 0.0f;
                }
                if (_IsEffectivelySmoothPerceptualRoughness(
                        pruned.coatRoughness)) {
                    pruned.coatWeight = 0.0f;
                }
                return _result.Add(pruned);
            } else if constexpr (std::is_same_v<T, Bsdf::MixData>) {
                const float mix = _Clamp01(data.mix);
                const Bsdf::NodeId bg = _PruneNode(data.bg);
                const Bsdf::NodeId fg = _PruneNode(data.fg);
                if (_IsValid(bg) && _IsValid(fg)) {
                    Bsdf::MixData pruned = data;
                    pruned.bg = bg;
                    pruned.fg = fg;
                    pruned.mix = mix;
                    return _result.Add(pruned);
                }
                if (_IsValid(bg)) {
                    return _ScaleNode(bg, 1.0f - mix);
                }
                if (_IsValid(fg)) {
                    return _ScaleNode(fg, mix);
                }
                return Bsdf::InvalidNodeId;
            } else if constexpr (std::is_same_v<T, Bsdf::LayerData>) {
                const Bsdf::NodeId top = _PruneNode(data.top);
                const Bsdf::NodeId base = _PruneNode(data.base);
                if (_IsValid(top) && _IsValid(base)) {
                    Bsdf::LayerData pruned;
                    pruned.top = top;
                    pruned.base = base;
                    return _result.Add(pruned);
                }
                return _IsValid(top) ? top : base;
            } else if constexpr (std::is_same_v<T, Bsdf::AddData>) {
                const Bsdf::NodeId in1 = _PruneNode(data.in1);
                const Bsdf::NodeId in2 = _PruneNode(data.in2);
                if (_IsValid(in1) && _IsValid(in2)) {
                    Bsdf::AddData pruned;
                    pruned.in1 = in1;
                    pruned.in2 = in2;
                    return _result.Add(pruned);
                }
                return _IsValid(in1) ? in1 : in2;
            } else if constexpr (std::is_same_v<T, Bsdf::MultiplyData>) {
                const Bsdf::NodeId input = _PruneNode(data.input);
                if (!_IsValid(input) || _Luminance(data.weight) <= _kEpsilon) {
                    return Bsdf::InvalidNodeId;
                }
                Bsdf::MultiplyData pruned = data;
                pruned.input = input;
                return _result.Add(pruned);
            } else {
                return Bsdf::InvalidNodeId;
            }
        }, node->data);
    }

    bool _IsValid(Bsdf::NodeId nodeId) const
    {
        return _result.IsValid(nodeId);
    }

    Bsdf::NodeId _ScaleNode(Bsdf::NodeId nodeId, float weight)
    {
        if (!_IsValid(nodeId) || weight <= _kEpsilon) {
            return Bsdf::InvalidNodeId;
        }
        if (weight >= 1.0f - _kEpsilon) {
            return nodeId;
        }
        Bsdf::MultiplyData multiply;
        multiply.input = nodeId;
        multiply.weight = Vec3f(weight);
        return _result.Add(multiply);
    }

    const Bsdf::ClosureTree& _source;
    Bsdf::ClosureTree _result;
};

Vec3f
_EvalLayerBaseThroughput(const Bsdf::ClosureTree& tree,
                         Bsdf::NodeId topNodeId,
                         const Vec3f& N,
                         const Vec3f& wo,
                         float heroWavelengthNm)
{
    return _EvalThroughput(tree, topNodeId, N, wo, heroWavelengthNm);
}

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
            const Vec3f shadingN = _ResolveReflectionNormal(data, N, wo);
            if (Dot(shadingN, wi) <= 0.0f || data.weight <= 0.0f) {
                return Vec3f(0.0f);
            }
            float NdotL = std::max(Dot(shadingN, wi), 0.0f);
            float NdotV = std::max(Dot(shadingN, wo), _kEpsilon);
            float LdotV = Dot(wi, wo);
            if (data.energyCompensation) {
                return _SafeVec(
                    _EvalEonDiffuse(
                        data.color, data.roughness, NdotV, NdotL, LdotV) *
                    data.weight);
            }
            float factor = _OrenNayarFactor(
                NdotV, NdotL, LdotV, data.roughness);
            return _SafeVec(data.color * (data.weight * factor * kInvPi));
        } else if constexpr (std::is_same_v<T, Bsdf::BurleyDiffuseData>) {
            const Vec3f shadingN = _ResolveReflectionNormal(data, N, wo);
            if (Dot(shadingN, wi) <= 0.0f || data.weight <= 0.0f) {
                return Vec3f(0.0f);
            }
            float NdotL = std::max(Dot(shadingN, wi), 0.0f);
            float NdotV = std::max(Dot(shadingN, wo), _kEpsilon);
            Vec3f H = (wi + wo).normalized();
            float LdotH = std::max(Dot(wi, H), 0.0f);
            float factor = _BurleyFactor(NdotV, NdotL, LdotH, data.roughness);
            return _SafeVec(data.color * (data.weight * factor * kInvPi));
        } else if constexpr (std::is_same_v<T, Bsdf::TranslucentData>) {
            const Vec3f shadingN = _ResolveReflectionNormal(data, N, wo);
            return _SafeVec(
                _EvalTranslucent(data.color, data.weight, shadingN, wi));
        } else if constexpr (std::is_same_v<T, Bsdf::SubsurfaceData>) {
            return Vec3f(0.0f);
        } else if constexpr (std::is_same_v<T, Bsdf::DielectricData>) {
            Vec3f result(0.0f);
            bool sameSide = _IsSameSide(N, wi, wo);
            const Vec3f shadingN = _ResolveReflectionNormal(data, N, wo);
            const float effectiveIor =
                _ResolveDielectricIor(data, heroWavelengthNm);
            if (_IsEffectivelyDeltaAlpha(data.roughness)) {
                return Vec3f(0.0f);
            }
            if (sameSide &&
                data.scatterMode != Bsdf::ScatterMode::Transmission) {
                const Vec3f fresnel = _DielectricReflectionFresnel(
                    data,
                    _ReflectionFresnelCosTheta(wi, wo),
                    effectiveIor);
                if (_IsEffectivelyIsotropic(data.roughness)) {
                    result += _EvalMicrofacetReflectionIsotropic(
                        std::clamp(
                            data.roughness[0], _kMinMicrofacetAlpha, 1.0f),
                        fresnel,
                        data.weight,
                        shadingN,
                        wi,
                        wo);
                } else {
                    result += _EvalMicrofacetReflectionAnisotropic(
                        data.roughness,
                        data.tangent,
                        fresnel,
                        data.weight,
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
        } else if constexpr (
            std::is_same_v<T, Bsdf::DielectricInterfaceData>) {
            Vec3f result(0.0f);
            const bool sameSide = _IsSameSide(N, wi, wo);
            const Vec3f shadingN = _ResolveReflectionNormal(data, N, wo);
            const float effectiveIor =
                _ResolveDielectricIor(data, heroWavelengthNm);
            if (_IsEffectivelyDeltaAlpha(data.roughness)) {
                return Vec3f(0.0f);
            }
            const bool compensateCoupledDielectric =
                _UsesCoupledRoughDielectricSampling(data);
            if (sameSide && data.reflectionWeight > 0.0f) {
                const Vec3f fresnel = _DielectricInterfaceReflectionCoefficient(
                    data,
                    _ReflectionFresnelCosTheta(wi, wo),
                    effectiveIor,
                    Dot(N, wo) < 0.0f);
                if (_IsEffectivelyIsotropic(data.roughness)) {
                    result += _EvalMicrofacetReflectionIsotropic(
                        std::clamp(
                            data.roughness[0], _kMinMicrofacetAlpha, 1.0f),
                        fresnel,
                        1.0f,
                        shadingN,
                        wi,
                        wo,
                        !compensateCoupledDielectric);
                } else {
                    result += _EvalMicrofacetReflectionAnisotropic(
                        data.roughness,
                        data.tangent,
                        fresnel,
                        1.0f,
                        shadingN,
                        wi,
                        wo,
                        !compensateCoupledDielectric);
                }
            }
            if (!sameSide && data.transmissionWeight > 0.0f) {
                if (data.thinWalled) {
                    const float NdotV =
                        std::max(std::abs(Dot(shadingN, wo)), _kEpsilon);
                    const Vec3f transmission =
                        _DielectricInterfaceTransmissionCoefficient(
                            data, NdotV, effectiveIor,
                            /* backside = */ false);
                    const Vec3f transmissionN = -shadingN;
                    Vec3f mirroredWo = _MirrorAcrossSurface(wo, shadingN);
                    mirroredWo.normalize();
                    if (_IsEffectivelyIsotropic(data.roughness)) {
                        result += _EvalMicrofacetReflectionIsotropic(
                            std::clamp(
                                data.roughness[0],
                                _kMinMicrofacetAlpha,
                                1.0f),
                            transmission,
                            1.0f,
                            transmissionN,
                            wi,
                            mirroredWo);
                    } else {
                        result += _EvalMicrofacetReflectionAnisotropic(
                            data.roughness,
                            data.tangent,
                            transmission,
                            1.0f,
                            transmissionN,
                            wi,
                            mirroredWo);
                    }
                    return _SafeVec(result);
                }

                if (compensateCoupledDielectric) {
                    result += _EvalCoupledRoughDielectricTransmission(
                        data, effectiveIor, N, shadingN, wi, wo);
                } else {
                    const float fresnelCos =
                        _TransmissionFresnelCosTheta(
                            effectiveIor, N, wi, wo);
                    const float baseReflectance =
                        _SchlickFresnelScalar(effectiveIor, fresnelCos);
                    const Vec3f transmissionScale = _TransmissionScale(
                        baseReflectance,
                        _DielectricInterfaceReflectanceUntinted(
                            data, fresnelCos, effectiveIor,
                            Dot(N, wo) < 0.0f));
                    result += CompMul(
                        Bsdf::EvalGGXTransmission(
                            _AverageAlphaAsRoughness(data.roughness),
                            effectiveIor,
                            data.transmissionTint,
                            N,
                            wi,
                            wo) * _Clamp01(data.transmissionWeight),
                        transmissionScale);
                }
            }
            if (compensateCoupledDielectric) {
                const _CoupledDielectricCompensation compensation =
                    _GetCoupledDielectricCompensation(
                        data, effectiveIor, N, shadingN, wo);
                if (compensation.missingEnergy > 0.0f) {
                    const float sideRatio = sameSide
                        ? compensation.reflectionRatio
                        : 1.0f - compensation.reflectionRatio;
                    const Vec3f tint = sameSide
                        ? _SafeVec(data.reflectionTint) *
                            _Clamp01(data.reflectionWeight)
                        : _SafeVec(data.transmissionTint) *
                            _Clamp01(data.transmissionWeight);
                    result += tint *
                        (compensation.missingEnergy * sideRatio * kInvPi);
                }
            }
            return _SafeVec(result);
        } else if constexpr (std::is_same_v<T, Bsdf::ConductorData>) {
            const Vec3f shadingN = _ResolveReflectionNormal(data, N, wo);
            if (Dot(shadingN, wi) <= 0.0f || data.weight <= 0.0f) {
                return Vec3f(0.0f);
            }
            if (_IsEffectivelyDeltaAlpha(data.roughness)) {
                return Vec3f(0.0f);
            }
            const Vec3f fresnel = _ConductorReflectionFresnel(
                data,
                _ReflectionFresnelCosTheta(wi, wo));
            if (_IsEffectivelyIsotropic(data.roughness)) {
                return _EvalMicrofacetReflectionIsotropic(
                    std::clamp(
                        data.roughness[0], _kMinMicrofacetAlpha, 1.0f),
                    fresnel,
                    data.weight,
                    shadingN,
                    wi,
                    wo);
            }
            return _EvalMicrofacetReflectionAnisotropic(
                data.roughness,
                data.tangent,
                fresnel,
                data.weight,
                shadingN,
                wi,
                wo);
        } else if constexpr (std::is_same_v<T, Bsdf::GeneralizedSchlickData>) {
            Vec3f result(0.0f);
            bool sameSide = _IsSameSide(N, wi, wo);
            const Vec3f shadingN = _ResolveReflectionNormal(data, N, wo);
            if (_IsEffectivelyDeltaAlpha(data.roughness)) {
                return Vec3f(0.0f);
            }
            if (sameSide &&
                data.scatterMode != Bsdf::ScatterMode::Transmission) {
                const Vec3f fresnel = _GeneralizedSchlickReflectionFresnel(
                    data,
                    _ReflectionFresnelCosTheta(wi, wo));
                if (_IsEffectivelyIsotropic(data.roughness)) {
                    result += _EvalMicrofacetReflectionIsotropic(
                        std::clamp(
                            data.roughness[0], _kMinMicrofacetAlpha, 1.0f),
                        fresnel,
                        data.weight,
                        shadingN,
                        wi,
                        wo);
                } else {
                    result += _EvalMicrofacetReflectionAnisotropic(
                        data.roughness,
                        data.tangent,
                        fresnel,
                        data.weight,
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
            const Vec3f shadingN = _ResolveReflectionNormal(data, N, wo);
            if (Dot(shadingN, wi) <= 0.0f || data.weight <= 0.0f) {
                return Vec3f(0.0f);
            }
            return _SafeVec(Bsdf::EvalSheen(
                data.color, data.roughness, shadingN, wi, wo) * data.weight);
        } else if constexpr (std::is_same_v<T, Bsdf::AdobeOpenPbrData>) {
            return _SafeVec(EvalAdobeOpenPbr(data, N, wi, wo));
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
            return topEval + CompMul(
                baseEval,
                _EvalLayerBaseThroughput(
                    tree, data.top, N, wo, heroWavelengthNm));
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
                const bool useDirectionalLayerThroughput =
                    _IsGgxMicrofacetMultipleScatteringEnabled() &&
                    !_HasThinFilm(
                        data.thinFilmWeight,
                        data.thinFilmThickness,
                        data.thinFilmIor);
                if (useDirectionalLayerThroughput) {
                    if (_GetDielectricLayerThroughputMode() ==
                        Bsdf::DielectricLayerThroughputMode::MaterialXGlsl) {
                        const Vec3f reflectance =
                            _MaterialXGlslDielectricLayerReflectance(
                                _AverageAlphaForEnergy(data.roughness),
                                NdotV,
                                effectiveIor);
                        throughput -= reflectance * data.weight;
                    } else {
                        const float filter =
                            _LookupBsdlDielectricReflFrontFilter(
                                NdotV,
                                _BsdlLayerRoughnessFromAlpha(data.roughness),
                                effectiveIor);
                        throughput = _LerpVec(
                            Vec3f(1.0f),
                            Vec3f(filter),
                            _Clamp01(data.weight));
                    }
                } else {
                    const Vec3f reflectance = _LayerThroughputReflectance(
                        _AverageAlphaForEnergy(data.roughness),
                        NdotV,
                        _DielectricReflectionFresnelUntinted(
                            data, NdotV, effectiveIor));
                    throughput -= reflectance * data.weight;
                }
            }
            return _SaturateVec(throughput);
        } else if constexpr (
            std::is_same_v<T, Bsdf::DielectricInterfaceData>) {
            const Vec3f shadingN = _ResolveReflectionNormal(data, N, wo);
            const float NdotV =
                std::max(std::abs(Dot(shadingN, wo)), _kEpsilon);
            const float effectiveIor =
                _ResolveDielectricIor(data, heroWavelengthNm);
            Vec3f throughput(1.0f);
            if (data.reflectionWeight > 0.0f) {
                const bool useDirectionalLayerThroughput =
                    _IsGgxMicrofacetMultipleScatteringEnabled() &&
                    !_HasThinFilm(
                        data.thinFilmWeight,
                        data.thinFilmThickness,
                        data.thinFilmIor);
                if (useDirectionalLayerThroughput) {
                    if (_GetDielectricLayerThroughputMode() ==
                        Bsdf::DielectricLayerThroughputMode::MaterialXGlsl) {
                        const Vec3f reflectance =
                            _MaterialXGlslDielectricLayerReflectance(
                                _AverageAlphaForEnergy(data.roughness),
                                NdotV,
                                effectiveIor);
                        throughput -=
                            reflectance * _Clamp01(data.reflectionWeight);
                    } else {
                        const float filter =
                            _LookupBsdlDielectricReflFrontFilter(
                                NdotV,
                                _BsdlLayerRoughnessFromAlpha(data.roughness),
                                effectiveIor);
                        throughput = _LerpVec(
                            Vec3f(1.0f),
                            Vec3f(filter),
                            _Clamp01(data.reflectionWeight));
                    }
                } else {
                    const Vec3f reflectance = _LayerThroughputReflectance(
                        _AverageAlphaForEnergy(data.roughness),
                        NdotV,
                        _DielectricInterfaceReflectanceUntinted(
                            data, NdotV, effectiveIor,
                            Dot(N, wo) < 0.0f));
                    throughput -=
                        reflectance * _Clamp01(data.reflectionWeight);
                }
            }
            return _SaturateVec(throughput);
        } else if constexpr (std::is_same_v<T, Bsdf::ConductorData>) {
            const Vec3f shadingN = _ResolveReflectionNormal(data, N, wo);
            const float NdotV =
                std::max(std::abs(Dot(shadingN, wo)), _kEpsilon);
            const Vec3f reflectance = _LayerThroughputReflectance(
                _AverageAlphaForEnergy(data.roughness),
                NdotV,
                _ConductorReflectionFresnel(data, NdotV));
            return _SaturateVec(
                Vec3f(1.0f) -
                reflectance * data.weight);
        } else if constexpr (std::is_same_v<T, Bsdf::GeneralizedSchlickData>) {
            const Vec3f shadingN = _ResolveReflectionNormal(data, N, wo);
            const float NdotV =
                std::max(std::abs(Dot(shadingN, wo)), _kEpsilon);
            const Vec3f reflectance = _LayerThroughputReflectance(
                _AverageAlphaForEnergy(data.roughness),
                NdotV,
                _GeneralizedSchlickReflectionFresnel(data, NdotV));
            return _SaturateVec(
                Vec3f(1.0f) -
                reflectance * data.weight);
        } else if constexpr (std::is_same_v<T, Bsdf::SheenData>) {
            const Vec3f shadingN = _ResolveReflectionNormal(data, N, wo);
            float NdotV = std::max(std::abs(Dot(shadingN, wo)), _kEpsilon);
            float dirAlbedo = _ApproxSheenDirAlbedo(NdotV, data.roughness);
            return _SaturateVec(Vec3f(1.0f - dirAlbedo * data.weight));
        } else if constexpr (std::is_same_v<T, Bsdf::AdobeOpenPbrData>) {
            return Vec3f(1.0f);
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
        } else if constexpr (
            std::is_same_v<T, Bsdf::DielectricInterfaceData>) {
            const Vec3f shadingN = _ResolveReflectionNormal(data, N, wo);
            const float NdotV =
                std::max(std::abs(Dot(shadingN, wo)), _kEpsilon);
            const float effectiveIor =
                _ResolveDielectricIor(data, heroWavelengthNm);
            const Vec3f reflection =
                _DielectricInterfaceReflectionCoefficient(
                    data, NdotV, effectiveIor,
                    Dot(N, wo) < 0.0f);
            const Vec3f transmission =
                _DielectricInterfaceTransmissionCoefficient(
                    data, NdotV, effectiveIor,
                    Dot(N, wo) < 0.0f);
            const float weight =
                _Luminance(reflection) + _Luminance(transmission);
            return weight > 0.0f ? std::max(weight, 0.05f) : 0.0f;
        } else if constexpr (std::is_same_v<T, Bsdf::ConductorData>) {
            const Vec3f shadingN = _ResolveReflectionNormal(data, N, wo);
            return data.weight *
                std::max(
                    _Luminance(_ConductorReflectionFresnel(
                        data,
                        std::max(std::abs(Dot(shadingN, wo)), _kEpsilon))),
                    0.05f);
        } else if constexpr (std::is_same_v<T, Bsdf::GeneralizedSchlickData>) {
            const Vec3f shadingN = _ResolveReflectionNormal(data, N, wo);
            const float NdotV =
                std::max(std::abs(Dot(shadingN, wo)), _kEpsilon);
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
        } else if constexpr (std::is_same_v<T, Bsdf::AdobeOpenPbrData>) {
            return 1.0f;
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
            const Vec3f shadingN = _ResolveReflectionNormal(data, N, wo);
            return (Dot(shadingN, wi) > 0.0f)
                ? Bsdf::PdfLambertian(shadingN, wi)
                : 0.0f;
        } else if constexpr (std::is_same_v<T, Bsdf::TranslucentData>) {
            const Vec3f shadingN = _ResolveReflectionNormal(data, N, wo);
            return _PdfTranslucent(shadingN, wi);
        } else if constexpr (std::is_same_v<T, Bsdf::SubsurfaceData>) {
            return 0.0f;
        } else if constexpr (std::is_same_v<T, Bsdf::DielectricData>) {
            bool sameSide = _IsSameSide(N, wi, wo);
            const Vec3f shadingN = _ResolveReflectionNormal(data, N, wo);
            const float effectiveIor =
                _ResolveDielectricIor(data, heroWavelengthNm);
            if (_IsEffectivelyDeltaAlpha(data.roughness)) {
                return 0.0f;
            }
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
        } else if constexpr (
            std::is_same_v<T, Bsdf::DielectricInterfaceData>) {
            const bool sameSide = _IsSameSide(N, wi, wo);
            const Vec3f shadingN = _ResolveReflectionNormal(data, N, wo);
            const float effectiveIor =
                _ResolveDielectricIor(data, heroWavelengthNm);
            if (_IsEffectivelyDeltaAlpha(data.roughness)) {
                return 0.0f;
            }
            if (_UsesCoupledRoughDielectricSampling(data)) {
                return _PdfCoupledRoughDielectric(
                    data, effectiveIor, N, shadingN, wi, wo);
            }
            const float NdotV =
                std::max(std::abs(Dot(shadingN, wo)), _kEpsilon);
            const _DielectricInterfaceSelection selection =
                _DielectricInterfaceSelectionProbabilities(
                    data, NdotV, effectiveIor,
                    Dot(N, wo) < 0.0f);
            if (sameSide && data.reflectionWeight > 0.0f) {
                const float branchPdf =
                    _IsEffectivelyIsotropic(data.roughness)
                    ? Bsdf::PdfGGXSpecular(
                        _AverageAlphaAsRoughness(data.roughness),
                        shadingN,
                        wi,
                        wo)
                    : _PdfGGXSpecularAnisotropic(
                        data.roughness,
                        data.tangent,
                        shadingN,
                        wi,
                        wo);
                return selection.reflection * branchPdf;
            }
            if (!sameSide && data.transmissionWeight > 0.0f) {
                if (data.thinWalled) {
                    const Vec3f transmissionN = -shadingN;
                    Vec3f mirroredWo = _MirrorAcrossSurface(wo, shadingN);
                    mirroredWo.normalize();
                    const float branchPdf =
                        _IsEffectivelyIsotropic(data.roughness)
                        ? Bsdf::PdfGGXSpecular(
                            _AverageAlphaAsRoughness(data.roughness),
                            transmissionN,
                            wi,
                            mirroredWo)
                        : _PdfGGXSpecularAnisotropic(
                            data.roughness,
                            data.tangent,
                            transmissionN,
                            wi,
                            mirroredWo);
                    return selection.transmission * branchPdf;
                }

                return selection.transmission *
                    Bsdf::PdfGGXTransmission(
                        _AverageAlphaAsRoughness(data.roughness),
                        effectiveIor,
                        N,
                        wi,
                        wo);
            }
            return 0.0f;
        } else if constexpr (std::is_same_v<T, Bsdf::ConductorData>) {
            if (_IsEffectivelyDeltaAlpha(data.roughness)) {
                return 0.0f;
            }
            const Vec3f shadingN = _ResolveReflectionNormal(data, N, wo);
            return (Dot(shadingN, wi) > 0.0f)
                ? (_IsEffectivelyIsotropic(data.roughness)
                    ? Bsdf::PdfGGXSpecular(
                        _AverageAlphaAsRoughness(data.roughness),
                        shadingN,
                        wi,
                        wo)
                    : _PdfGGXSpecularAnisotropic(
                        data.roughness,
                        data.tangent,
                        shadingN,
                        wi,
                        wo))
                : 0.0f;
        } else if constexpr (std::is_same_v<T, Bsdf::GeneralizedSchlickData>) {
            bool sameSide = _IsSameSide(N, wi, wo);
            const Vec3f shadingN = _ResolveReflectionNormal(data, N, wo);
            if (_IsEffectivelyDeltaAlpha(data.roughness)) {
                return 0.0f;
            }
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
            const Vec3f shadingN = _ResolveReflectionNormal(data, N, wo);
            return (Dot(shadingN, wi) > 0.0f)
                ? Bsdf::PdfLambertian(shadingN, wi)
                : 0.0f;
        } else if constexpr (std::is_same_v<T, Bsdf::AdobeOpenPbrData>) {
            return PdfAdobeOpenPbr(data, N, wi, wo);
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
            const Vec3f shadingN = _ResolveReflectionNormal(data, N, wo);
            auto sample = Bsdf::SampleLambertian(
                data.color * data.weight, shadingN, wo, u1, u2);
            return _FinalizeSubtreeSample(
                tree, nodeId, N, wo, sample, heroWavelengthNm);
        } else if constexpr (std::is_same_v<T, Bsdf::BurleyDiffuseData>) {
            const Vec3f shadingN = _ResolveReflectionNormal(data, N, wo);
            auto sample = Bsdf::SampleLambertian(
                data.color * data.weight, shadingN, wo, u1, u2);
            return _FinalizeSubtreeSample(
                tree, nodeId, N, wo, sample, heroWavelengthNm);
        } else if constexpr (std::is_same_v<T, Bsdf::TranslucentData>) {
            const Vec3f shadingN = _ResolveReflectionNormal(data, N, wo);
            _Frame frame = _Frame::FromNormal(-shadingN);
            Vec3f wiLocal = _SampleCosineHemisphere(u1, u2);
            Vec3f wi = frame.ToWorld(wiLocal);
            Bsdf::BsdfSample sample{
                wi,
                _EvalTranslucent(data.color, data.weight, shadingN, wi),
                _CosineHemispherePdf(wiLocal[2]),
                false
            };
            sample.isDiffuseLike = true;
            return _FinalizeSubtreeSample(
                tree, nodeId, N, wo, sample, heroWavelengthNm);
        } else if constexpr (std::is_same_v<T, Bsdf::SubsurfaceData>) {
            Bsdf::BsdfSample sample{
                Vec3f(0.0f),
                Vec3f(data.weight),
                1.0f,
                false
            };
            sample.isSubsurface = true;
            sample.isDiffuseLike = true;
            return sample;
        } else if constexpr (std::is_same_v<T, Bsdf::DielectricData>) {
            const Vec3f shadingN = _ResolveReflectionNormal(data, N, wo);
            const float NdotV =
                std::max(std::abs(Dot(shadingN, wo)), _kEpsilon);
            const float effectiveIor =
                _ResolveDielectricIor(data, heroWavelengthNm);
            const bool hasDeltaRoughness =
                _IsEffectivelyDeltaAlpha(data.roughness);
            if (hasDeltaRoughness &&
                data.scatterMode != Bsdf::ScatterMode::Reflection &&
                _WouldTotalInternalReflect(effectiveIor, N, wo)) {
                if (data.scatterMode == Bsdf::ScatterMode::Transmission) {
                    // Transmission-only lobes are paired with a separate
                    // reflection lobe; the helper splits the TIR energy with
                    // that pair instead of double-counting it.
                    return _SampleDeltaDielectricTransmission(
                        data, effectiveIor, NdotV, N, wo);
                }
                return _SampleDeltaTotalInternalReflection(
                    data.weight, N, wo);
            }
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
        } else if constexpr (
            std::is_same_v<T, Bsdf::DielectricInterfaceData>) {
            const Vec3f shadingN = _ResolveReflectionNormal(data, N, wo);
            const float NdotV =
                std::max(std::abs(Dot(shadingN, wo)), _kEpsilon);
            const float effectiveIor =
                _ResolveDielectricIor(data, heroWavelengthNm);
            const _DielectricInterfaceSelection selection =
                _DielectricInterfaceSelectionProbabilities(
                    data, NdotV, effectiveIor,
                    Dot(N, wo) < 0.0f);
            if (selection.reflection + selection.transmission <= 0.0f) {
                return Bsdf::BsdfSample{
                    Vec3f(0.0f), Vec3f(0.0f), 0.0f, false};
            }

            const bool hasDeltaRoughness =
                _IsEffectivelyDeltaAlpha(data.roughness);
            if (_UsesCoupledRoughDielectricSampling(data)) {
                auto sample = _SampleCoupledRoughDielectric(
                    data,
                    effectiveIor,
                    N,
                    shadingN,
                    wo,
                    u1,
                    u2,
                    uChoice);
                return _FinalizeSubtreeSample(
                    tree, nodeId, N, wo, sample, heroWavelengthNm);
            }
            if (hasDeltaRoughness &&
                !data.thinWalled &&
                _WouldTotalInternalReflect(effectiveIor, N, wo)) {
                const float tirWeight = std::max(
                    _Clamp01(data.reflectionWeight),
                    _Clamp01(data.transmissionWeight));
                return _SampleDeltaTotalInternalReflection(
                    tirWeight, N, wo);
            }
            if (uChoice < selection.reflection) {
                if (selection.reflection <= 0.0f) {
                    return Bsdf::BsdfSample{
                        Vec3f(0.0f), Vec3f(0.0f), 0.0f, false};
                }
                if (hasDeltaRoughness) {
                    return _ScaleDiscreteSpecularSample(
                        _SampleDeltaDielectricInterfaceReflection(
                            data, effectiveIor, shadingN, wo,
                            Dot(N, wo) < 0.0f),
                        selection.reflection);
                }
                if (_IsEffectivelyIsotropic(data.roughness)) {
                    auto sample = Bsdf::SampleGGXSpecular(
                        _AverageAlphaAsRoughness(data.roughness),
                        effectiveIor,
                        data.reflectionTint,
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

            if (selection.transmission <= 0.0f) {
                return Bsdf::BsdfSample{
                    Vec3f(0.0f), Vec3f(0.0f), 0.0f, false};
            }
            if (hasDeltaRoughness) {
                return _ScaleDiscreteSpecularSample(
                    _SampleDeltaDielectricInterfaceTransmission(
                        data, effectiveIor, NdotV, N, wo),
                    selection.transmission);
            }
            if (data.thinWalled) {
                const Vec3f transmissionN = -shadingN;
                Vec3f mirroredWo = _MirrorAcrossSurface(wo, shadingN);
                mirroredWo.normalize();
                if (_IsEffectivelyIsotropic(data.roughness)) {
                    auto sample = Bsdf::SampleGGXSpecular(
                        _AverageAlphaAsRoughness(data.roughness),
                        1.0f,
                        data.transmissionTint,
                        transmissionN,
                        mirroredWo,
                        u1,
                        u2);
                    if (sample.pdf <= 0.0f) {
                        return _ScaleDiscreteSpecularSample(
                            _SampleDeltaDielectricInterfaceTransmission(
                                data, effectiveIor, NdotV, N, wo),
                            selection.transmission);
                    }
                    return _FinalizeSubtreeSample(
                        tree, nodeId, N, wo, sample, heroWavelengthNm);
                }
                auto sample = _SampleGGXSpecularAnisotropic(
                    data.roughness,
                    data.tangent,
                    transmissionN,
                    mirroredWo,
                    u1,
                    u2);
                if (sample.pdf <= 0.0f) {
                    return _ScaleDiscreteSpecularSample(
                        _SampleDeltaDielectricInterfaceTransmission(
                            data, effectiveIor, NdotV, N, wo),
                        selection.transmission);
                }
                return _FinalizeSubtreeSample(
                    tree, nodeId, N, wo, sample, heroWavelengthNm);
            }
            auto sample = Bsdf::SampleGGXTransmission(
                _AverageAlphaAsRoughness(data.roughness),
                effectiveIor,
                data.transmissionTint,
                N,
                wo,
                u1,
                u2);
            // Rough samples are finalized by re-evaluating this interface,
            // which applies the same BSDL front/back transmission-energy
            // scale used by direct evaluation.
            if (sample.pdf <= 0.0f) {
                return _ScaleDiscreteSpecularSample(
                    _SampleDeltaDielectricInterfaceTransmission(
                        data, effectiveIor, NdotV, N, wo),
                    selection.transmission);
            }
            sample.f *= _Clamp01(data.transmissionWeight);
            return _FinalizeSubtreeSample(
                tree, nodeId, N, wo, sample, heroWavelengthNm);
        } else if constexpr (std::is_same_v<T, Bsdf::ConductorData>) {
            const Vec3f shadingN = _ResolveReflectionNormal(data, N, wo);
            if (_IsEffectivelyDeltaAlpha(data.roughness)) {
                return _SampleDeltaConductorReflection(data, shadingN, wo);
            }
            if (_IsEffectivelyIsotropic(data.roughness)) {
                auto sample = Bsdf::SampleGGXSpecular(
                    _AverageAlphaAsRoughness(data.roughness),
                    1.5f,
                    _ConductorF0(data.ior, data.extinction),
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
        } else if constexpr (std::is_same_v<T, Bsdf::GeneralizedSchlickData>) {
            const Vec3f shadingN = _ResolveReflectionNormal(data, N, wo);
            const float NdotV =
                std::max(std::abs(Dot(shadingN, wo)), _kEpsilon);
            const bool hasDeltaRoughness =
                _IsEffectivelyDeltaAlpha(data.roughness);
            float fresnelProb = _Clamp01(_Luminance(
                _GeneralizedSchlickReflectionFresnel(data, NdotV)));
            if (data.scatterMode == Bsdf::ScatterMode::Transmission) {
                float avgF0 = _Clamp01(_Luminance(_SaturateVec(data.color0)));
                float sqrtF0 = std::sqrt(std::max(avgF0, 0.01f));
                float ior = (1.0f + sqrtF0) / (1.0f - sqrtF0);
                // A transmission-only lobe is paired with a separate
                // reflection lobe that keeps contributing its Schlick
                // reflectance from inside the medium, so a full-weight TIR
                // sample would double-count reflection and gain energy on
                // every internal bounce.  Carry only the remainder so the
                // reflection/transmission pair totals one.
                const auto samplePairedTir = [&]() {
                    auto tir = _SampleDeltaTotalInternalReflection(
                        data.weight, N, wo);
                    tir.f = CompMul(
                        tir.f,
                        _SaturateVec(
                            Vec3f(1.0f) -
                            _GeneralizedSchlickReflectionFresnel(
                                data, NdotV)));
                    return tir;
                };
                if (hasDeltaRoughness &&
                    _WouldTotalInternalReflect(ior, N, wo)) {
                    return samplePairedTir();
                }
                if (hasDeltaRoughness) {
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
                auto sample = Bsdf::SampleGGXTransmission(
                    _AverageAlphaAsRoughness(data.roughness),
                    ior,
                    Vec3f(1.0f),
                    N,
                    wo,
                    u1,
                    u2);
                if (sample.pdf <= 0.0f) {
                    if (_WouldTotalInternalReflect(ior, N, wo)) {
                        return samplePairedTir();
                    }
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
                if (hasDeltaRoughness &&
                    _WouldTotalInternalReflect(ior, N, wo)) {
                    return _SampleDeltaTotalInternalReflection(
                        data.weight, N, wo);
                }
                if (hasDeltaRoughness) {
                    auto deltaSample = _SampleDeltaTransmission(
                        ior, Vec3f(1.0f), data.weight, N, wo);
                    deltaSample.f = CompMul(
                        deltaSample.f,
                        _TransmissionScale(
                            _SchlickFresnelScalar(ior, NdotV),
                            _GeneralizedSchlickReflectionFresnel(
                                data, NdotV)));
                    return _ScaleDiscreteSpecularSample(
                        deltaSample,
                        1.0f - fresnelProb);
                }
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
            if (hasDeltaRoughness) {
                auto sample =
                    _SampleDeltaGeneralizedSchlickReflection(data, shadingN, wo);
                if (data.scatterMode ==
                    Bsdf::ScatterMode::ReflectionTransmission) {
                    return _ScaleDiscreteSpecularSample(sample, fresnelProb);
                }
                return sample;
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
            const Vec3f shadingN = _ResolveReflectionNormal(data, N, wo);
            auto sample = Bsdf::SampleLambertian(
                data.color * data.weight, shadingN, wo, u1, u2);
            return _FinalizeSubtreeSample(
                tree, nodeId, N, wo, sample, heroWavelengthNm);
        } else if constexpr (std::is_same_v<T, Bsdf::AdobeOpenPbrData>) {
            return SampleAdobeOpenPbr(data, N, wo, u1, u2, uChoice);
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
                    sample.f = CompMul(
                        sample.f,
                        _EvalLayerBaseThroughput(
                            tree, data.top, N, wo, heroWavelengthNm));
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

void
Bsdf::SetGgxMicrofacetMultipleScatteringEnabled(bool enabled)
{
    _gGgxMicrofacetMultipleScatteringEnabled.store(
        enabled, std::memory_order_relaxed);
}

bool
Bsdf::IsGgxMicrofacetMultipleScatteringEnabled()
{
    return _IsGgxMicrofacetMultipleScatteringEnabled();
}

void
Bsdf::SetDielectricLayerThroughputMode(DielectricLayerThroughputMode mode)
{
    _gDielectricLayerThroughputMode.store(
        static_cast<int>(mode), std::memory_order_relaxed);
}

Bsdf::DielectricLayerThroughputMode
Bsdf::GetDielectricLayerThroughputMode()
{
    return _GetDielectricLayerThroughputMode();
}

Vec3f
Bsdf::EvalLambertian(
    const Vec3f& baseColor,
    const Vec3f& N,
    const Vec3f& wi,
    const Vec3f& wo)
{
    // Lambertian reflection: both wi and wo must lie on the front side of N.
    // Previously this function ignored N/wi/wo entirely and returned
    // baseColor/pi unconditionally, causing back-face hits (e.g. viewing an
    // opaque ground from below, or secondary rays landing on the inside of
    // a closed mesh) to "leak" light through the surface. The SSS exit
    // path post-flip was especially vulnerable to this because the flipped
    // ray routinely crosses surfaces at grazing angles.
    if (Dot(N, wi) <= 0.0f || Dot(N, wo) <= 0.0f) {
        return Vec3f(0.0f);
    }
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
    Vec3f compensatedF = CompMul(
        F,
        _TurquinMicrofacetMsScale(alpha, NdotV, F));
    return _SafeVec(CompMul(
        compensatedF,
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
    float heroWavelengthNm,
    bool frontFacing)
{
    const Vec3f interfaceN = frontFacing ? N : -N;
    Vec3f f = closure.HasBsdfTree()
        ? _EvalNode(
            closure.bsdfTree,
            closure.bsdfTree.root,
            interfaceN,
            wi,
            wo,
            heroWavelengthNm)
        : _EvalLegacySurface(closure, interfaceN, wi, wo);
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
    BsdfSample sample{wi, baseColor * kInvPi, pdf, false};
    sample.isDiffuseLike = true;
    return sample;
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
    Vec3f compensatedF = CompMul(
        F,
        _TurquinMicrofacetMsScale(alpha, NdotV, F));
    Vec3f f = CompMul(
        compensatedF,
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

float
Bsdf::GgxDirectionalMissingEnergy(
    float cosTheta,
    float alphaRoughness)
{
    return _LookupGgxMissingEnergy(cosTheta, alphaRoughness);
}

float
Bsdf::GgxDirectionalSingleScatterEnergy(
    float cosTheta,
    float alphaRoughness)
{
    return 1.0f - _LookupGgxMissingEnergy(cosTheta, alphaRoughness);
}

float
Bsdf::CoupledRoughDielectricDirectionalTransmissionAlbedo(
    float cosTheta,
    const Vec2f& roughness,
    float ior,
    bool backfacing,
    bool compensateMultipleScattering)
{
    const float alpha = std::clamp(
        std::max(roughness[0], roughness[1]), 0.0f, 1.0f);
    const float safeIor = std::clamp(
        ior,
        _kBsdlDielectricIorMin,
        _kBsdlDielectricIorMax);
    const float relativeEta = backfacing ? 1.0f / safeIor : safeIor;
    const float exactAlbedo = 1.0f - _MaterialXDielectricFresnel(
        cosTheta, relativeEta);
    if (alpha <= _kTransmissionExactFresnelMaxAlpha) {
        return exactAlbedo;
    }

    const float perceptualRoughness =
        _BsdlLayerRoughnessFromAlpha(roughness);
    float albedo = _LookupBsdlDielectricTransmissionSingleScatterAlbedo(
        cosTheta, perceptualRoughness, ior, backfacing);
    if (compensateMultipleScattering) {
        const _CoupledDielectricCompensation compensation =
            _BsdlCoupledDielectricCompensation(
                cosTheta, perceptualRoughness, ior, backfacing);
        albedo += compensation.missingEnergy *
            (1.0f - compensation.reflectionRatio);
    }
    if (alpha < _kTransmissionFresnelBlendMaxAlpha) {
        float t = (alpha - _kTransmissionExactFresnelMaxAlpha) /
            (_kTransmissionFresnelBlendMaxAlpha -
             _kTransmissionExactFresnelMaxAlpha);
        t = t * t * (3.0f - 2.0f * t);
        albedo = exactAlbedo * (1.0f - t) + albedo * t;
    }
    return _Clamp01(albedo);
}

float
Bsdf::StraightShadowDielectricTransmission(
    const SurfaceClosure& closure,
    float signedCosTheta)
{
    const float cosTheta = _Clamp01(std::abs(signedCosTheta));
    const float safeIor = std::max(closure.specularIor, 1.0f);
    const auto* interface =
        _FindCoupledTransmissionInterfaceForStraightShadow(closure);
    if (!interface) {
        return _Clamp01(
            1.0f - _SchlickFresnelScalar(safeIor, cosTheta));
    }
    return CoupledRoughDielectricDirectionalTransmissionAlbedo(
        cosTheta,
        interface->roughness,
        interface->ior,
        signedCosTheta > 0.0f,
        interface->compensateCoupledDielectric);
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
    float heroWavelengthNm,
    bool frontFacing)
{
    const Vec3f interfaceN = frontFacing ? N : -N;
    if (closure.HasBsdfTree()) {
        auto sample = _SampleNode(closure.bsdfTree, closure.bsdfTree.root,
                                  interfaceN, wo, u1, u2, uLobe, heroWavelengthNm);
        if (!sample.isSpecular) {
            sample.f *= closure.presence;
        }
        return sample;
    }
    return _SampleLegacySurface(closure, interfaceN, wo, u1, u2, uLobe);
}

float
Bsdf::PdfSurface(
    const SurfaceClosure& closure,
    const Vec3f& N,
    const Vec3f& wi,
    const Vec3f& wo,
    float heroWavelengthNm,
    bool frontFacing)
{
    const Vec3f interfaceN = frontFacing ? N : -N;
    if (closure.HasBsdfTree()) {
        return _PdfNode(
            closure.bsdfTree,
            closure.bsdfTree.root,
            interfaceN,
            wi,
            wo,
            heroWavelengthNm);
    }
    return _PdfLegacySurface(closure, interfaceN, wi, wo);
}

SurfaceClosure
Bsdf::PruneCausticClassLobes(const SurfaceClosure& closure)
{
    SurfaceClosure pruned = closure;
    if (closure.HasBsdfTree()) {
        _CausticClassPruner pruner(closure.bsdfTree);
        pruned.bsdfTree = pruner.Run();
        if (!pruned.HasBsdfTree()) {
            _ClearLegacyBsdfSummary(&pruned);
        }
        return pruned;
    }

    pruned.transmission = 0.0f;
    if (_IsEffectivelySmoothPerceptualRoughness(pruned.roughness)) {
        pruned.specular = 0.0f;
        pruned.specularColor = Vec3f(0.0f);
    }
    if (_IsEffectivelySmoothPerceptualRoughness(pruned.coatRoughness)) {
        pruned.coat = 0.0f;
    }
    return pruned;
}

namespace {
constexpr float _kSmoothRoughnessThreshold = 1.0e-3f;
}

bool
Bsdf::SampleSubsurfaceEntry(
    const SurfaceClosure& closure,
    const Vec3f& N,
    const Vec3f& wo,
    float u1, float u2,
    Vec3f& wi_into_medium)
{
    // Clamp IOR >= 1 to avoid TIR at entry (matches Cycles).
    const float ior = std::max(closure.specularIor, 1.0f);
    const float eta = 1.0f / ior;  // outside -> inside

    const float cosNI = Dot(N, wo);
    if (cosNI <= 0.0f) {
        return false;
    }

    // Smooth surface: deterministic Snell refraction about geometric normal.
    if (closure.roughness < _kSmoothRoughnessThreshold) {
        const float sin2T = eta * eta * (1.0f - cosNI * cosNI);
        if (sin2T >= 1.0f) {
            // Shouldn't happen with IOR >= 1 but guard anyway.
            wi_into_medium = -wo;
            return true;
        }
        const float cosT = std::sqrt(std::max(0.0f, 1.0f - sin2T));
        wi_into_medium = -eta * wo + (eta * cosNI - cosT) * N;
        return true;
    }

    // Rough surface: GGX VNDF samples microfacet normal H, then Snell about H.
    const _Frame frame = _Frame::FromNormal(N);
    const Vec3f woLocal = frame.ToLocal(wo);
    const float alpha = _RoughnessToAlpha(closure.roughness);
    const Vec3f hLocal = _SampleGGX_VNDF(woLocal, alpha, u1, u2);
    const Vec3f H = frame.ToWorld(hLocal);

    const float cosHI = Dot(H, wo);
    const float sin2T = eta * eta * (1.0f - cosHI * cosHI);
    if (sin2T >= 1.0f) {
        wi_into_medium = -wo;
        return true;
    }
    const float cosT = std::sqrt(std::max(0.0f, 1.0f - sin2T));
    wi_into_medium = -eta * wo + (eta * cosHI - cosT) * H;

    // Fallback if the refracted direction ends up outward (numerical edge):
    // sample a cosine-weighted hemisphere around -N so we still enter the
    // medium.
    if (Dot(wi_into_medium, N) >= 0.0f) {
        const Vec3f hemi = _SampleCosineHemisphere(u1, u2);
        // hemi is expressed in a frame with +Z = up; reflect to -N by
        // negating the Z component when transforming back through `frame`.
        wi_into_medium = frame.ToWorld(
            Vec3f(hemi[0], hemi[1], -hemi[2]));
    }
    return true;
}

}  // namespace mxcpp
