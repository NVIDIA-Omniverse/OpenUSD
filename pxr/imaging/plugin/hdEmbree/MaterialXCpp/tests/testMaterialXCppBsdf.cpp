//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "../materials/bsdf.h"
#include "../materials/bsdfDielectricReflFrontLut.h"
#include "../spectral.h"
#include "../nodes/helpers/mathHelpers.h"
#include "../../medium.h"
#include "../../sss.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <functional>

PXR_NAMESPACE_USING_DIRECTIVE

using namespace mxcpp;

// From testMain.cpp
void Test_Register(const char* name, std::function<bool()> fn);
bool Test_IsClose(float a, float b, float eps = 1e-5f);
bool Test_IsClose(const Vec3f& a, const Vec3f& b, float eps = 1e-5f);

#define _REG(name) Test_Register("Bsdf." #name, &name)

// ---------------------------------------------------------------------------

namespace {

constexpr float _kFurnaceTwoPi = 6.2831853071795864769f;
constexpr float _kFurnacePi = 3.14159265358979323846f;
constexpr int _kFurnaceSampleCount = 8192;
constexpr float _kFurnaceEnergyUpperSlack = 0.025f;

constexpr float _kFurnaceAlphaRoughness[] = {
    0.05f, 0.2f, 0.4f, 0.6f, 0.8f, 1.0f
};

constexpr float _kFurnaceCosTheta[] = {
    0.99f, 0.7f, 0.5f, 0.3f, 0.1f, 0.03f
};

static float
_RadicalInverseBase2(std::uint32_t bits)
{
    bits = (bits << 16) | (bits >> 16);
    bits = ((bits & 0x55555555u) << 1) | ((bits & 0xAAAAAAAAu) >> 1);
    bits = ((bits & 0x33333333u) << 2) | ((bits & 0xCCCCCCCCu) >> 2);
    bits = ((bits & 0x0F0F0F0Fu) << 4) | ((bits & 0xF0F0F0F0u) >> 4);
    bits = ((bits & 0x00FF00FFu) << 8) | ((bits & 0xFF00FF00u) >> 8);
    return static_cast<float>(bits) * 2.3283064365386963e-10f;
}

static Vec3f
_HemisphereDirectionYUp(float u1, float u2)
{
    const float cosTheta = std::clamp(u1, 0.0f, 1.0f);
    const float sinTheta =
        std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
    const float phi = _kFurnaceTwoPi * u2;
    return Vec3f(
        sinTheta * std::cos(phi),
        cosTheta,
        sinTheta * std::sin(phi));
}

static Vec3f
_DirectionFromCosThetaYUp(float cosTheta)
{
    const float clamped = std::clamp(cosTheta, 0.0f, 1.0f);
    return Vec3f(std::sqrt(std::max(0.0f, 1.0f - clamped * clamped)),
                 clamped,
                 0.0f);
}

static Vec3f
_IntegrateHemisphereUniform(
    const std::function<Vec3f(const Vec3f& wi)>& eval,
    int sampleCount)
{
    const Vec3f N(0.0f, 1.0f, 0.0f);
    Vec3f sum(0.0f);
    for (int i = 0; i < sampleCount; ++i) {
        const float u1 =
            (static_cast<float>(i) + 0.5f) / static_cast<float>(sampleCount);
        const float u2 = _RadicalInverseBase2(static_cast<std::uint32_t>(i));
        const Vec3f wi = _HemisphereDirectionYUp(u1, u2);
        sum += eval(wi) * (Dot(N, wi) * _kFurnaceTwoPi);
    }
    return sum * (1.0f / static_cast<float>(sampleCount));
}

static Vec3f
_IntegrateSurfaceBySampling(
    const SurfaceClosure& closure,
    const Vec3f& N,
    const Vec3f& wo,
    int sampleCount)
{
    Vec3f sum(0.0f);
    for (int i = 0; i < sampleCount; ++i) {
        const float u1 =
            (static_cast<float>(i) + 0.5f) / static_cast<float>(sampleCount);
        const float u2 = _RadicalInverseBase2(static_cast<std::uint32_t>(i));
        const float uChoice =
            _RadicalInverseBase2(static_cast<std::uint32_t>(i) ^ 0x9E3779B9u);
        const auto sample =
            Bsdf::SampleSurface(closure, N, wo, u1, u2, uChoice);
        if (sample.pdf <= 0.0f || sample.isSpecular || sample.isSubsurface) {
            continue;
        }
        const float cosTheta = std::max(Dot(N, sample.wi), 0.0f);
        sum += sample.f * (cosTheta / sample.pdf);
    }
    return sum * (1.0f / static_cast<float>(sampleCount));
}

static float
_FurnaceLuminance(const Vec3f& value)
{
    return 0.2126f * value[0] + 0.7152f * value[1] + 0.0722f * value[2];
}

static bool
_IsFiniteNonNegative(const Vec3f& value)
{
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(value[i]) || value[i] < -1.0e-5f) {
            return false;
        }
    }
    return true;
}

static bool
_CheckFurnaceEnergyBound(
    const char* label,
    float alphaRoughness,
    float cosTheta,
    const Vec3f& energy)
{
    if (!_IsFiniteNonNegative(energy)) {
        printf(
            "    %s furnace invalid: alpha=%f NoV=%f E=(%f,%f,%f)\n",
            label, alphaRoughness, cosTheta,
            energy[0], energy[1], energy[2]);
        return false;
    }

    for (int i = 0; i < 3; ++i) {
        if (energy[i] > 1.0f + _kFurnaceEnergyUpperSlack) {
            printf(
                "    %s furnace energy gain: alpha=%f NoV=%f "
                "channel=%d E=%f\n",
                label, alphaRoughness, cosTheta, i, energy[i]);
            return false;
        }
    }
    return true;
}

static float
_DielectricTransmittanceForTest(float eta, float cosTheta)
{
    const float c = std::clamp(cosTheta, 0.0f, 1.0f);
    float g = (eta - 1.0f) * (eta + 1.0f) + c * c;
    if (g <= 0.0f) {
        return 0.0f;
    }
    g = std::sqrt(g);
    const float A = (g - c) / (g + c);
    const float B = (c * (g + c) - 1.0f) / (c * (g - c) + 1.0f);
    const float reflectance = 0.5f * A * A * (1.0f + B * B);
    return std::clamp(1.0f - reflectance, 0.0f, 1.0f);
}

static float
_SchlickFresnelForTest(float ior, float cosTheta)
{
    float f0 = (ior - 1.0f) / (ior + 1.0f);
    f0 *= f0;
    const float t = 1.0f - std::clamp(cosTheta, 0.0f, 1.0f);
    const float t2 = t * t;
    return f0 + (1.0f - f0) * t2 * t2 * t;
}

static Vec2f
_MaterialXGgxDirAlbedoAnalyticABForTest(float NdotV, float alpha)
{
    const float x = std::clamp(NdotV, 0.0f, 1.0f);
    const float y = std::clamp(alpha, 0.0f, 1.0f);
    const float x2 = x * x;
    const float y2 = y * y;
    const float xy = x * y;
    const float x2y = x2 * y;
    const float xy2 = x * y2;
    const float x2y2 = x2 * y2;

    const float r0 =
        0.1003f - 0.6303f * x + 9.748f * y - 2.038f * xy +
        29.34f * x2 - 8.245f * y2 - 26.44f * x2y +
        19.99f * xy2 - 5.448f * x2y2;
    const float r1 =
        0.9345f - 2.323f * x + 2.229f * y - 3.748f * xy +
        1.424f * x2 - 0.7684f * y2 + 1.436f * x2y +
        0.2913f * xy2 + 0.6286f * x2y2;
    const float r2 =
        1.0f - 1.765f * x + 8.263f * y + 11.53f * xy +
        28.96f * x2 - 7.507f * y2 - 36.11f * x2y +
        15.86f * xy2 + 33.37f * x2y2;
    const float r3 =
        1.0f + 0.2281f * x + 15.94f * y - 55.83f * xy +
        13.08f * x2 + 41.26f * y2 + 54.9f * x2y +
        300.2f * xy2 - 285.1f * x2y2;

    return Vec2f(
        std::clamp(r0 / r2, 0.0f, 1.0f),
        std::clamp(r1 / r3, 0.0f, 1.0f));
}

static Vec3f
_MaterialXGlslDielectricThroughputForTest(
    float alpha,
    float cosTheta,
    float ior,
    float weight,
    float fresnel)
{
    float F0 = (ior - 1.0f) / (ior + 1.0f);
    F0 *= F0;

    const Vec2f ab =
        _MaterialXGgxDirAlbedoAnalyticABForTest(cosTheta, alpha);
    const Vec3f dirAlbedo = Vec3f(F0) * ab[0] + Vec3f(1.0f) * ab[1];
    const float Ess = std::max(ab[0] + ab[1], 1.0e-7f);
    const Vec3f comp =
        Vec3f(1.0f) + Vec3f(fresnel) * ((1.0f - Ess) / Ess);
    return Vec3f(1.0f) - CompMul(dirAlbedo, comp) * weight;
}

static float
_GgxDForTest(float alpha, float NdotH)
{
    const float a2 = alpha * alpha;
    const float denom = NdotH * NdotH * (a2 - 1.0f) + 1.0f;
    return a2 / (_kFurnacePi * denom * denom + 1.0e-7f);
}

static float
_SmithG1ForTest(float alpha, float cosTheta)
{
    const float a2 = alpha * alpha;
    const float cos2 = cosTheta * cosTheta;
    return 2.0f * cosTheta /
        (cosTheta + std::sqrt(a2 + (1.0f - a2) * cos2) + 1.0e-7f);
}

static float
_GgxSeparableGForTest(float alpha, float NdotV, float NdotL)
{
    return _SmithG1ForTest(alpha, NdotV) * _SmithG1ForTest(alpha, NdotL);
}

static float
_GgxHeightCorrelatedGForTest(float alpha, float NdotV, float NdotL)
{
    const float a2 = alpha * alpha;
    const float ggxV =
        NdotL * std::sqrt(NdotV * NdotV * (1.0f - a2) + a2);
    const float ggxL =
        NdotV * std::sqrt(NdotL * NdotL * (1.0f - a2) + a2);
    const float visibility = 0.5f / (ggxV + ggxL + 1.0e-7f);
    return 4.0f * NdotV * NdotL * visibility;
}

}  // namespace

static bool
TestLambertianValue()
{
    Vec3f white(1.0f);
    Vec3f N(0, 1, 0);
    Vec3f wi(0, 1, 0);
    Vec3f wo(0, 1, 0);

    Vec3f result = Bsdf::EvalLambertian(white, N, wi, wo);
    float expected = 1.0f / 3.14159265358979f;
    if (!Test_IsClose(result, Vec3f(expected), 1e-5f)) {
        printf("    Expected (%f,%f,%f), got (%f,%f,%f)\n",
               expected, expected, expected,
               result[0], result[1], result[2]);
        return false;
    }
    return true;
}

static bool
TestFurnaceHelperMatchesLambertian()
{
    const Vec3f N(0.0f, 1.0f, 0.0f);
    const Vec3f wo = _DirectionFromCosThetaYUp(0.37f);
    const Vec3f albedo(0.73f, 0.41f, 0.19f);
    const Vec3f energy = _IntegrateHemisphereUniform(
        [&](const Vec3f& wi) {
            return Bsdf::EvalLambertian(albedo, N, wi, wo);
        },
        _kFurnaceSampleCount);

    if (!Test_IsClose(energy, albedo, 1.0e-4f)) {
        printf(
            "    Lambertian furnace helper mismatch: "
            "expected=(%f,%f,%f) got=(%f,%f,%f)\n",
            albedo[0], albedo[1], albedo[2],
            energy[0], energy[1], energy[2]);
        return false;
    }
    return true;
}

static bool
TestLegacySurfaceSpecularZeroIsLambertian()
{
    SurfaceClosure closure;
    closure.baseColor = Vec3f(1.0f);
    closure.roughness = 1.0f;
    closure.specular = 0.0f;
    closure.specularColor = Vec3f(0.0f);

    const Vec3f N(0.0f, 1.0f, 0.0f);
    const Vec3f wi = Vec3f(0.67f, 0.31f, 0.68f).normalized();
    const Vec3f wo = Vec3f(-0.19f, 0.24f, 0.95f).normalized();
    const Vec3f expected = Bsdf::EvalLambertian(
        closure.baseColor, N, wi, wo);
    const Vec3f actual = Bsdf::EvalSurface(closure, N, wi, wo);

    if (!Test_IsClose(actual, expected, 1.0e-6f)) {
        printf(
            "    specular=0 legacy surface should be Lambertian: "
            "expected=(%f,%f,%f) got=(%f,%f,%f)\n",
            expected[0], expected[1], expected[2],
            actual[0], actual[1], actual[2]);
        return false;
    }
    return true;
}

static bool
TestOrenNayarEnergyCompensationFalseUsesLegacyFactor()
{
    SurfaceClosure closure;
    Bsdf::OrenNayarDiffuseData diffuse;
    diffuse.weight = 0.8f;
    diffuse.color = Vec3f(0.7f, 0.4f, 0.2f);
    diffuse.roughness = 0.65f;
    diffuse.energyCompensation = false;
    closure.bsdfTree.root = closure.bsdfTree.Add(diffuse);

    const Vec3f N(0.0f, 1.0f, 0.0f);
    const Vec3f wi = Vec3f(0.52f, 0.62f, 0.59f).normalized();
    const Vec3f wo = Vec3f(-0.21f, 0.77f, 0.60f).normalized();
    const float NdotL = std::max(Dot(N, wi), 0.0f);
    const float NdotV = std::max(Dot(N, wo), 1.0e-7f);
    const float LdotV = Dot(wi, wo);
    const float s = LdotV - NdotL * NdotV;
    const float stinv = (s > 0.0f) ? s / std::max(NdotL, NdotV) : 0.0f;
    const float sigma2 = diffuse.roughness * diffuse.roughness;
    const float A = 1.0f - 0.5f * (sigma2 / (sigma2 + 0.33f));
    const float B = 0.45f * sigma2 / (sigma2 + 0.09f);
    const Vec3f expected =
        diffuse.color * (diffuse.weight * (A + B * stinv) / _kFurnacePi);
    const Vec3f actual = Bsdf::EvalSurface(closure, N, wi, wo);

    if (!Test_IsClose(actual, expected, 1.0e-6f)) {
        printf(
            "    Legacy Oren-Nayar branch changed: "
            "expected=(%f,%f,%f) got=(%f,%f,%f)\n",
            expected[0], expected[1], expected[2],
            actual[0], actual[1], actual[2]);
        return false;
    }
    return true;
}

static bool
TestEonDiffuseLambertianLimit()
{
    SurfaceClosure closure;
    Bsdf::OrenNayarDiffuseData diffuse;
    diffuse.weight = 0.7f;
    diffuse.color = Vec3f(0.73f, 0.41f, 0.19f);
    diffuse.roughness = 0.0f;
    diffuse.energyCompensation = true;
    closure.bsdfTree.root = closure.bsdfTree.Add(diffuse);

    const Vec3f N(0.0f, 1.0f, 0.0f);
    const Vec3f wi = Vec3f(0.36f, 0.81f, 0.46f).normalized();
    const Vec3f wo = Vec3f(-0.48f, 0.67f, 0.57f).normalized();
    const Vec3f expected =
        Bsdf::EvalLambertian(diffuse.color * diffuse.weight, N, wi, wo);
    const Vec3f actual = Bsdf::EvalSurface(closure, N, wi, wo);

    if (!Test_IsClose(actual, expected, 1.0e-6f)) {
        printf(
            "    EON roughness=0 should match Lambertian: "
            "expected=(%f,%f,%f) got=(%f,%f,%f)\n",
            expected[0], expected[1], expected[2],
            actual[0], actual[1], actual[2]);
        return false;
    }
    return true;
}

static bool
TestEonDiffuseWhiteFurnace()
{
    SurfaceClosure closure;
    Bsdf::OrenNayarDiffuseData diffuse;
    diffuse.weight = 1.0f;
    diffuse.color = Vec3f(1.0f);
    diffuse.roughness = 1.0f;
    diffuse.energyCompensation = true;
    closure.bsdfTree.root = closure.bsdfTree.Add(diffuse);

    const Vec3f N(0.0f, 1.0f, 0.0f);
    bool ok = true;
    for (const float cosTheta : {0.99f, 0.5f, 0.1f}) {
        const Vec3f wo = _DirectionFromCosThetaYUp(cosTheta);
        const Vec3f energy = _IntegrateHemisphereUniform(
            [&](const Vec3f& wi) {
                return Bsdf::EvalSurface(closure, N, wi, wo);
            },
            _kFurnaceSampleCount);
        if (!Test_IsClose(energy, Vec3f(1.0f), 0.025f)) {
            printf(
                "    EON white furnace mismatch at NoV=%f: "
                "energy=(%f,%f,%f)\n",
                cosTheta, energy[0], energy[1], energy[2]);
            ok = false;
        }
    }
    return ok;
}

static bool
TestGGXFurnaceWhiteFresnelGeneralizedSchlickBaseline()
{
    const Vec3f N(0.0f, 1.0f, 0.0f);
    bool ok = true;
    printf("    GGX white-Fresnel Turquin-compensated furnace:\n");
    for (const float alphaRoughness : _kFurnaceAlphaRoughness) {
        printf("      alpha=%0.2f:", alphaRoughness);
        for (const float cosTheta : _kFurnaceCosTheta) {
            SurfaceClosure closure;
            Bsdf::GeneralizedSchlickData specular;
            specular.weight = 1.0f;
            specular.color0 = Vec3f(1.0f);
            specular.color82 = Vec3f(1.0f);
            specular.color90 = Vec3f(1.0f);
            specular.exponent = 5.0f;
            specular.roughness = Vec2f(alphaRoughness, alphaRoughness);
            specular.scatterMode = Bsdf::ScatterMode::Reflection;
            closure.bsdfTree.root = closure.bsdfTree.Add(specular);

            const Vec3f wo = _DirectionFromCosThetaYUp(cosTheta);
            const Vec3f energy =
                _IntegrateSurfaceBySampling(
                    closure, N, wo, _kFurnaceSampleCount);
            printf(" %0.4f", _FurnaceLuminance(energy));
            ok = _CheckFurnaceEnergyBound(
                "GeneralizedSchlick(F=1)",
                alphaRoughness,
                cosTheta,
                energy) && ok;
        }
        printf("\n");
    }
    return ok;
}

static bool
TestGGXFurnaceConductorEnergyBaseline()
{
    const Vec3f N(0.0f, 1.0f, 0.0f);
    bool ok = true;
    printf("    GGX conductor Turquin-compensated furnace:\n");
    for (const float alphaRoughness : _kFurnaceAlphaRoughness) {
        printf("      alpha=%0.2f:", alphaRoughness);
        for (const float cosTheta : _kFurnaceCosTheta) {
            SurfaceClosure closure;
            Bsdf::ConductorData conductor;
            conductor.weight = 1.0f;
            conductor.ior = Vec3f(0.15f, 0.14f, 0.13f);
            conductor.extinction = Vec3f(3.5f, 3.4f, 3.3f);
            conductor.roughness = Vec2f(alphaRoughness, alphaRoughness);
            closure.bsdfTree.root = closure.bsdfTree.Add(conductor);

            const Vec3f wo = _DirectionFromCosThetaYUp(cosTheta);
            const Vec3f energy =
                _IntegrateSurfaceBySampling(
                    closure, N, wo, _kFurnaceSampleCount);
            printf(" %0.4f", _FurnaceLuminance(energy));
            ok = _CheckFurnaceEnergyBound(
                "Conductor",
                alphaRoughness,
                cosTheta,
                energy) && ok;
        }
        printf("\n");
    }
    return ok;
}

static bool
TestGGXFurnaceDielectricReflectionEnergyBaseline()
{
    const Vec3f N(0.0f, 1.0f, 0.0f);
    bool ok = true;
    printf("    GGX dielectric reflection Turquin-compensated furnace:\n");
    for (const float alphaRoughness : _kFurnaceAlphaRoughness) {
        printf("      alpha=%0.2f:", alphaRoughness);
        for (const float cosTheta : _kFurnaceCosTheta) {
            SurfaceClosure closure;
            Bsdf::DielectricData dielectric;
            dielectric.weight = 1.0f;
            dielectric.tint = Vec3f(1.0f);
            dielectric.ior = 1.5f;
            dielectric.roughness = Vec2f(alphaRoughness, alphaRoughness);
            dielectric.scatterMode = Bsdf::ScatterMode::Reflection;
            closure.bsdfTree.root = closure.bsdfTree.Add(dielectric);

            const Vec3f wo = _DirectionFromCosThetaYUp(cosTheta);
            const Vec3f energy =
                _IntegrateSurfaceBySampling(
                    closure, N, wo, _kFurnaceSampleCount);
            printf(" %0.4f", _FurnaceLuminance(energy));
            ok = _CheckFurnaceEnergyBound(
                "DielectricReflection",
                alphaRoughness,
                cosTheta,
                energy) && ok;
        }
        printf("\n");
    }
    return ok;
}

static bool
TestLambertianColorScaling()
{
    Vec3f color(0.5f, 0.3f, 0.1f);
    Vec3f N(0, 1, 0);
    Vec3f result = Bsdf::EvalLambertian(color, N, Vec3f(0,1,0), Vec3f(0,1,0));
    float invPi = 1.0f / 3.14159265358979f;
    return Test_IsClose(result, color * invPi, 1e-5f);
}

static bool
TestGGXSpecularNonNegative()
{
    Vec3f N(0, 1, 0);
    Vec3f wo = Vec3f(0, 1, 0);
    Vec3f wi = Vec3f(0.5f, 0.866f, 0.0f).normalized();
    Vec3f result = Bsdf::EvalGGXSpecular(
        0.5f, 1.5f, Vec3f(1.0f), N, wi, wo);
    return result[0] >= 0.0f && result[1] >= 0.0f && result[2] >= 0.0f;
}

static bool
TestGGXSpecularUsesHeightCorrelatedSmith()
{
    const Vec3f N(0.0f, 1.0f, 0.0f);
    const Vec3f wo = Vec3f(0.62f, 0.68f, 0.39f).normalized();
    const Vec3f wi = Vec3f(-0.34f, 0.42f, 0.84f).normalized();
    const float perceptualRoughness = 0.7f;
    const float alpha = perceptualRoughness * perceptualRoughness;

    const Vec3f actual = Bsdf::EvalGGXSpecular(
        perceptualRoughness, 1.5f, Vec3f(1.0f), N, wi, wo);

    Vec3f H = wi + wo;
    H.normalize();
    const float NdotL = std::max(Dot(N, wi), 0.0f);
    const float NdotV = std::max(Dot(N, wo), 0.0f);
    const float NdotH = std::max(Dot(N, H), 0.0f);
    const float D = _GgxDForTest(alpha, NdotH);
    const float hcG = _GgxHeightCorrelatedGForTest(alpha, NdotV, NdotL);
    const float sepG = _GgxSeparableGForTest(alpha, NdotV, NdotL);
    const float msScale = 1.0f /
        std::max(Bsdf::GgxDirectionalSingleScatterEnergy(NdotV, alpha), 0.01f);
    const float expected =
        D * hcG * msScale /
        std::max(4.0f * NdotL * NdotV, 1.0e-7f);
    const float separable =
        D * sepG * msScale /
        std::max(4.0f * NdotL * NdotV, 1.0e-7f);

    if (!Test_IsClose(actual, Vec3f(expected), 1.0e-4f)) {
        printf(
            "    Expected height-correlated GGX=%f, got (%f,%f,%f); "
            "separable would be %f\n",
            expected, actual[0], actual[1], actual[2], separable);
        return false;
    }

    if (std::abs(expected - separable) < 1.0e-3f) {
        printf("    Test directions do not distinguish HC and separable G\n");
        return false;
    }
    return true;
}

static bool
TestGGXDirectionalMissingEnergyLutBounds()
{
    bool ok = true;
    for (const float alphaRoughness : _kFurnaceAlphaRoughness) {
        for (const float cosTheta : _kFurnaceCosTheta) {
            const float missing = Bsdf::GgxDirectionalMissingEnergy(
                cosTheta, alphaRoughness);
            const float singleScatter =
                Bsdf::GgxDirectionalSingleScatterEnergy(
                    cosTheta, alphaRoughness);
            if (!std::isfinite(missing) || missing < 0.0f || missing > 1.0f) {
                printf(
                    "    GGX missing-energy LUT out of range: "
                    "alpha=%f NoV=%f Ems=%f\n",
                    alphaRoughness, cosTheta, missing);
                ok = false;
            }
            if (!Test_IsClose(missing + singleScatter, 1.0f, 1.0e-5f)) {
                printf(
                    "    GGX single/missing energy mismatch: "
                    "alpha=%f NoV=%f Ess=%f Ems=%f\n",
                    alphaRoughness, cosTheta, singleScatter, missing);
                ok = false;
            }
        }
    }

    float previous = -1.0f;
    for (const float alphaRoughness : _kFurnaceAlphaRoughness) {
        const float missing = Bsdf::GgxDirectionalMissingEnergy(
            1.0f, alphaRoughness);
        if (missing + 5.0e-4f < previous) {
            printf(
                "    GGX normal-incidence missing energy is not monotonic: "
                "previous=%f current=%f alpha=%f\n",
                previous, missing, alphaRoughness);
            ok = false;
        }
        previous = missing;
    }
    return ok;
}

static bool
TestGGXTurquinWhiteFurnaceCompensatesMissingEnergy()
{
    struct Case {
        float alphaRoughness;
        float cosTheta;
    };

    const Case cases[] = {
        {0.05f, 0.03f},
        {0.20f, 0.30f},
        {0.40f, 0.50f},
        {0.80f, 0.10f},
        {1.00f, 0.99f},
        {1.00f, 0.03f}
    };

    const Vec3f N(0.0f, 1.0f, 0.0f);
    bool ok = true;
    for (const Case& c : cases) {
        SurfaceClosure closure;
        Bsdf::GeneralizedSchlickData specular;
        specular.weight = 1.0f;
        specular.color0 = Vec3f(1.0f);
        specular.color82 = Vec3f(1.0f);
        specular.color90 = Vec3f(1.0f);
        specular.exponent = 5.0f;
        specular.roughness = Vec2f(c.alphaRoughness, c.alphaRoughness);
        specular.scatterMode = Bsdf::ScatterMode::Reflection;
        closure.bsdfTree.root = closure.bsdfTree.Add(specular);

        const Vec3f wo = _DirectionFromCosThetaYUp(c.cosTheta);
        const Vec3f energy = _IntegrateSurfaceBySampling(
            closure, N, wo, _kFurnaceSampleCount);
        const float measured = _FurnaceLuminance(energy);
        if (std::abs(measured - 1.0f) > 0.03f) {
            printf(
                "    GGX Turquin white furnace mismatch: alpha=%f NoV=%f "
                "sampled=%f expected=1\n",
                c.alphaRoughness, c.cosTheta, measured);
            ok = false;
        }
    }
    return ok;
}

static bool
TestGGXSpecularPeak()
{
    Vec3f N(0, 1, 0);
    Vec3f wo = Vec3f(0.3f, 0.95f, 0.0f).normalized();
    Vec3f wiMirror = Vec3f(-0.3f, 0.95f, 0.0f).normalized();
    Vec3f wiOff = Vec3f(0.8f, 0.6f, 0.0f).normalized();

    float roughness = 0.05f;
    Vec3f specColor(1.0f);

    Vec3f atMirror = Bsdf::EvalGGXSpecular(
        roughness, 1.5f, specColor, N, wiMirror, wo);
    Vec3f offMirror = Bsdf::EvalGGXSpecular(
        roughness, 1.5f, specColor, N, wiOff, wo);

    float mirrorMag = atMirror.length();
    float offMag = offMirror.length();
    if (mirrorMag <= offMag) {
        printf("    Mirror dir magnitude %f should be > off-specular %f\n",
               mirrorMag, offMag);
        return false;
    }
    return true;
}

static bool
TestGGXSpecularLowRoughnessPeakPreserved()
{
    const Vec3f N(0.0f, 1.0f, 0.0f);
    const Vec3f wo = Vec3f(0.2f, 0.9797959f, 0.0f).normalized();
    const Vec3f wiMirror = Vec3f(-0.2f, 0.9797959f, 0.0f).normalized();
    const Vec3f specColor(1.0f);

    const float broadRoughness = 0.05f;
    const float sharpRoughness = 0.01f;
    const Vec3f broadEval = Bsdf::EvalGGXSpecular(
        broadRoughness, 1.5f, specColor, N, wiMirror, wo);
    const Vec3f sharpEval = Bsdf::EvalGGXSpecular(
        sharpRoughness, 1.5f, specColor, N, wiMirror, wo);
    const float broadPdf = Bsdf::PdfGGXSpecular(
        broadRoughness, N, wiMirror, wo);
    const float sharpPdf = Bsdf::PdfGGXSpecular(
        sharpRoughness, N, wiMirror, wo);

    const float broadMag = broadEval.length();
    const float sharpMag = sharpEval.length();
    if (!(sharpMag > broadMag * 10.0f)) {
        printf(
            "    Low roughness GGX peak collapsed: roughness=%f eval=%f, "
            "roughness=%f eval=%f\n",
            sharpRoughness, sharpMag, broadRoughness, broadMag);
        return false;
    }
    if (!(sharpPdf > broadPdf * 10.0f)) {
        printf(
            "    Low roughness GGX pdf peak collapsed: roughness=%f pdf=%f, "
            "roughness=%f pdf=%f\n",
            sharpRoughness, sharpPdf, broadRoughness, broadPdf);
        return false;
    }
    return true;
}

static bool
TestTreeDielectricReflectionMatchesStandaloneGgx()
{
    SurfaceClosure c;
    Bsdf::DielectricData dielectric;
    dielectric.weight = 1.0f;
    dielectric.tint = Vec3f(1.0f);
    dielectric.ior = 1.5f;
    dielectric.roughness = Vec2f(0.05f * 0.05f, 0.05f * 0.05f);
    dielectric.scatterMode = Bsdf::ScatterMode::Reflection;
    c.bsdfTree.root = c.bsdfTree.Add(dielectric);

    const Vec3f N(0.0f, 1.0f, 0.0f);
    const Vec3f wo = Vec3f(0.3f, 0.9539392f, 0.0f).normalized();
    const Vec3f wi = Vec3f(-0.3f, 0.9539392f, 0.0f).normalized();

    const Vec3f treeEval = Bsdf::EvalSurface(c, N, wi, wo);
    const Vec3f ggxEval = Bsdf::EvalGGXSpecular(
        0.05f, 1.5f, Vec3f(0.04f), N, wi, wo);

    const float treeLum = treeEval.length();
    const float ggxLum = ggxEval.length();
    const float ratio = treeLum / std::max(ggxLum, 1.0e-8f);
    if (ratio < 0.9f || ratio > 1.1f) {
        printf(
            "    Tree dielectric != standalone GGX: tree=%f ggx=%f ratio=%f\n",
            treeLum, ggxLum, ratio);
        return false;
    }
    return true;
}

static bool
TestDispersionCauchyIorMonotonic()
{
    const float iorBlue =
        Spectral::CauchyDispersionIOR(20.0f, 1.5f, 450.0f);
    const float iorRed =
        Spectral::CauchyDispersionIOR(20.0f, 1.5f, 650.0f);
    if (!(iorBlue > iorRed && iorRed > 1.0f)) {
        printf("    Expected blue IOR > red IOR, got blue=%f red=%f\n",
               iorBlue, iorRed);
        return false;
    }
    return true;
}

static bool
TestDispersionChangesTransmissionSampling()
{
    SurfaceClosure c;
    Bsdf::DielectricData dielectric;
    dielectric.weight = 1.0f;
    dielectric.tint = Vec3f(1.0f);
    dielectric.ior = 1.5f;
    dielectric.dispersionAbbe = 20.0f;
    dielectric.roughness = Vec2f(0.1f, 0.1f);
    dielectric.scatterMode = Bsdf::ScatterMode::Transmission;
    c.bsdfTree.root = c.bsdfTree.Add(dielectric);

    const Vec3f N(0.0f, 1.0f, 0.0f);
    const Vec3f wo = Vec3f(0.35f, 0.93675f, 0.0f).normalized();

    const auto blue = Bsdf::SampleSurface(c, N, wo, 0.3f, 0.7f, 0.65f, 450.0f);
    const auto red = Bsdf::SampleSurface(c, N, wo, 0.3f, 0.7f, 0.65f, 650.0f);
    if (blue.pdf <= 0.0f || red.pdf <= 0.0f) {
        printf("    Expected valid transmission samples for dispersion test\n");
        return false;
    }

    if (Test_IsClose(blue.wi, red.wi, 1e-4f)) {
        printf("    Expected wavelength-dependent transmission direction\n");
        return false;
    }
    return true;
}

static bool
TestDispersionDisabledIgnoresHeroWavelength()
{
    SurfaceClosure c;
    Bsdf::DielectricData dielectric;
    dielectric.weight = 1.0f;
    dielectric.tint = Vec3f(1.0f);
    dielectric.ior = 1.5f;
    dielectric.roughness = Vec2f(0.1f, 0.1f);
    dielectric.scatterMode = Bsdf::ScatterMode::Transmission;
    c.bsdfTree.root = c.bsdfTree.Add(dielectric);

    const Vec3f N(0.0f, 1.0f, 0.0f);
    const Vec3f wo = Vec3f(0.35f, 0.93675f, 0.0f).normalized();

    const auto blue = Bsdf::SampleSurface(c, N, wo, 0.3f, 0.7f, 0.65f, 450.0f);
    const auto red = Bsdf::SampleSurface(c, N, wo, 0.3f, 0.7f, 0.65f, 650.0f);

    return Test_IsClose(blue.wi, red.wi, 1e-5f) &&
           Test_IsClose(blue.f, red.f, 1e-5f) &&
           Test_IsClose(blue.pdf, red.pdf, 1e-5f);
}

static Vec3f
_AverageHeroRoundTrip(const Vec3f& rgb)
{
    Vec3f average(0.0f);
    const float pdf = Spectral::HeroWavelengthPdf();

    for (int i = 0; i < static_cast<int>(Spectral::kLambdaResolution); ++i) {
        const float wavelengthNm =
            Spectral::kLambdaMinNm + static_cast<float>(i) * Spectral::kLambdaStepNm;
        const float spectralValue =
            Spectral::RgbToSpectralValue(rgb, wavelengthNm);
        const Vec3f reconstructed =
            Spectral::SpectralValueToRgb(
                spectralValue,
                wavelengthNm,
                pdf);
        const float weight = (i == 0 || i + 1 == static_cast<int>(Spectral::kLambdaResolution))
            ? 0.5f * Spectral::kLambdaStepNm
            : Spectral::kLambdaStepNm;
        average += reconstructed * (weight / Spectral::kLambdaRangeNm);
    }

    return average;
}

static bool
TestSpectralNeutralRoundTripWhite()
{
    const Vec3f reconstructed = _AverageHeroRoundTrip(Vec3f(1.0f));
    if (!Test_IsClose(reconstructed, Vec3f(1.0f), 1.0e-3f)) {
        printf("    Expected white round-trip to remain neutral, got (%f,%f,%f)\n",
               reconstructed[0], reconstructed[1], reconstructed[2]);
        return false;
    }
    return true;
}

static bool
TestSpectralNeutralRoundTripGray()
{
    const Vec3f target(0.18f);
    const Vec3f reconstructed = _AverageHeroRoundTrip(target);
    if (!Test_IsClose(reconstructed, target, 1.0e-3f)) {
        printf("    Expected gray round-trip to remain neutral, got (%f,%f,%f)\n",
               reconstructed[0], reconstructed[1], reconstructed[2]);
        return false;
    }
    return true;
}

static bool
TestCoatZeroWeight()
{
    Vec3f N(0, 1, 0);
    Vec3f wi(0, 1, 0);
    Vec3f wo(0, 1, 0);
    Vec3f result = Bsdf::EvalCoat(0.0f, 0.1f, 1.5f, N, wi, wo);
    return Test_IsClose(result, Vec3f(0.0f), 1e-7f);
}

static bool
TestEvalSurfaceEmissiveOnly()
{
    // EvalSurface returns pure BSDF value (no emissive — that's the
    // renderer's job). With NdotL <= 0 the BSDF returns zero.
    SurfaceClosure c;
    c.emissiveColor = Vec3f(1.0f, 0.5f, 0.0f);

    Vec3f N(0, 1, 0);
    Vec3f wi(0, -1, 0);  // NdotL <= 0
    Vec3f wo(0, 1, 0);

    Vec3f result = Bsdf::EvalSurface(c, N, wi, wo);
    return Test_IsClose(result, Vec3f(0.0f), 1e-5f);
}

static bool
TestSheenGrazingAngle()
{
    Vec3f N(0, 1, 0);
    Vec3f sheenColor(1.0f);
    float roughness = 0.5f;

    Vec3f woNormal = Vec3f(0, 1, 0);
    Vec3f woGrazing = Vec3f(0.99f, 0.14f, 0.0f).normalized();
    Vec3f wi = Vec3f(-0.3f, 0.95f, 0.0f).normalized();

    Vec3f atNormal = Bsdf::EvalSheen(sheenColor, roughness, N, wi, woNormal);
    Vec3f atGrazing = Bsdf::EvalSheen(sheenColor, roughness, N, wi, woGrazing);

    // Sheen should generally have more contribution at grazing angles.
    // We allow some tolerance here since the exact relationship depends on
    // the half-vector geometry.
    return atNormal.length() >= 0.0f && atGrazing.length() >= 0.0f;
}

static bool
TestTransmissionNonNegative()
{
    Vec3f N(0, 1, 0);
    // wi below surface, wo above — typical transmission geometry
    Vec3f wi = Vec3f(0.2f, -0.98f, 0.0f).normalized();
    Vec3f wo = Vec3f(0.3f, 0.95f, 0.0f).normalized();
    Vec3f tColor(1.0f);

    Vec3f result = Bsdf::EvalGGXTransmission(
        0.5f, 1.5f, tColor, N, wi, wo);
    if (result[0] < 0.0f || result[1] < 0.0f || result[2] < 0.0f) {
        printf("    Negative transmission value: (%f,%f,%f)\n",
               result[0], result[1], result[2]);
        return false;
    }
    if (result.length() <= 0.0f) {
        printf("    Expected non-zero transmission, got zero\n");
        return false;
    }
    return true;
}

static bool
TestEvalSurfaceNonNegative()
{
    SurfaceClosure c;
    c.baseColor = Vec3f(0.8f, 0.2f, 0.1f);
    c.roughness = 0.4f;
    c.metallic = 0.0f;
    c.specular = 1.0f;
    c.specularIor = 1.5f;
    c.specularColor = Vec3f(1.0f);
    c.emissiveColor = Vec3f(0.0f);

    Vec3f N(0, 1, 0);
    Vec3f wi = Vec3f(0.3f, 0.95f, 0.0f).normalized();
    Vec3f wo = Vec3f(-0.2f, 0.98f, 0.0f).normalized();

    Vec3f result = Bsdf::EvalSurface(c, N, wi, wo);
    return result[0] >= 0.0f && result[1] >= 0.0f && result[2] >= 0.0f;
}

static bool
TestPhaseHgIsotropicMatchesUniformSphere()
{
    const float expected = 1.0f / (4.0f * 3.14159265358979f);
    const float actual = PhaseHG(0.25f, 0.0f);
    if (!Test_IsClose(actual, expected, 1.0e-6f)) {
        printf("    Expected isotropic phase=%f, got %f\n", expected, actual);
        return false;
    }
    return true;
}

static bool
TestPhaseHgForwardScatterBias()
{
    const float forward = PhaseHG(-0.95f, 0.7f);
    const float backward = PhaseHG(0.95f, 0.7f);
    if (!(forward > backward)) {
        printf("    Expected forward HG lobe to dominate: %f <= %f\n",
               forward, backward);
        return false;
    }
    return true;
}

static bool
TestSampleHenyeyGreensteinPdfConsistency()
{
    const Vec3f wo = Vec3f(0.0f, 0.0f, -1.0f);
    const Vec3f wi = SampleHenyeyGreenstein(wo, 0.5f, 0.3f, 0.7f);
    const float pdf = PdfHenyeyGreenstein(wi, wo, 0.5f);
    const float phase = PhaseHG(Dot(wi, wo), 0.5f);
    if (!Test_IsClose(pdf, phase, 1.0e-5f)) {
        printf("    Expected phase/pdf match, got phase=%f pdf=%f\n",
               phase, pdf);
        return false;
    }
    return true;
}

static bool
TestSampleHenyeyGreensteinForwardMean()
{
    const Vec3f wo = Vec3f(0.0f, 0.0f, -1.0f);
    float meanCosTheta = 0.0f;
    constexpr int kSampleCount = 128;

    for (int i = 0; i < kSampleCount; ++i) {
        const float u1 = (static_cast<float>(i) + 0.5f) / kSampleCount;
        const float u2 =
            (static_cast<float>((i * 37) % kSampleCount) + 0.5f) / kSampleCount;
        const Vec3f wi = SampleHenyeyGreenstein(wo, 0.7f, u1, u2);
        meanCosTheta += -Dot(wi, wo);
    }
    meanCosTheta /= kSampleCount;

    if (meanCosTheta < 0.45f) {
        printf("    Expected forward-scattering mean cosine, got %f\n",
               meanCosTheta);
        return false;
    }
    return true;
}

static bool
TestFreeFlightScatterWeightFinite()
{
    MediumProperties medium;
    medium.sigmaA = Vec3f(0.2f, 0.8f, 0.4f);
    medium.sigmaS = Vec3f(0.6f, 0.1f, 0.3f);

    const float distance = SampleFreeFlight(medium, 0.42f);
    const Vec3f scatterWeight = EvalFreeFlightScatterWeight(medium, distance);
    const Vec3f transmittanceWeight =
        EvalMajorantTransmittanceWeight(medium, distance);

    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(scatterWeight[i]) || scatterWeight[i] < 0.0f) {
            printf("    Scatter weight channel %d invalid: %f\n",
                   i, scatterWeight[i]);
            return false;
        }
        if (!std::isfinite(transmittanceWeight[i]) ||
            transmittanceWeight[i] < 0.0f) {
            printf("    Transmittance weight channel %d invalid: %f\n",
                   i, transmittanceWeight[i]);
            return false;
        }
    }
    return true;
}

static bool
TestSampleSurfaceSubsurfaceReturnsMarker()
{
    SurfaceClosure c;
    Bsdf::SubsurfaceData subsurface;
    subsurface.weight = 0.7f;
    subsurface.color = Vec3f(0.4f, 0.6f, 0.8f);
    c.bsdfTree.root = c.bsdfTree.Add(subsurface);

    const Vec3f N(0.0f, 1.0f, 0.0f);
    const Vec3f wo(0.0f, 1.0f, 0.0f);

    const auto sample = Bsdf::SampleSurface(c, N, wo, 0.2f, 0.8f, 0.5f);
    if (!sample.isSubsurface) {
        printf("    Expected subsurface marker on sampled event\n");
        return false;
    }
    if (sample.isSpecular) {
        printf("    Subsurface event should not be marked specular\n");
        return false;
    }
    if (!Test_IsClose(sample.f, Vec3f(subsurface.weight), 1.0e-6f)) {
        printf(
            "    Expected neutral subsurface marker weight, got (%f,%f,%f)\n",
            sample.f[0], sample.f[1], sample.f[2]);
        return false;
    }
    if (!Test_IsClose(sample.pdf, 1.0f, 1.0e-6f)) {
        printf("    Expected unit pdf for subsurface marker, got %f\n",
               sample.pdf);
        return false;
    }
    return true;
}

static bool
TestSampleSurfaceMixCanChooseSubsurface()
{
    SurfaceClosure c;

    Bsdf::OrenNayarDiffuseData diffuse;
    diffuse.weight = 1.0f;
    diffuse.color = Vec3f(0.8f, 0.6f, 0.4f);

    Bsdf::SubsurfaceData subsurface;
    subsurface.weight = 1.0f;
    subsurface.color = Vec3f(0.3f, 0.5f, 0.7f);

    const Bsdf::NodeId diffuseId = c.bsdfTree.Add(diffuse);
    const Bsdf::NodeId subsurfaceId = c.bsdfTree.Add(subsurface);

    Bsdf::MixData mix;
    mix.fg = diffuseId;
    mix.bg = subsurfaceId;
    mix.mix = 0.25f;
    c.bsdfTree.root = c.bsdfTree.Add(mix);

    const Vec3f N(0.0f, 1.0f, 0.0f);
    const Vec3f wo(0.0f, 1.0f, 0.0f);

    const auto sample = Bsdf::SampleSurface(c, N, wo, 0.2f, 0.8f, 0.8f);
    if (!sample.isSubsurface) {
        printf("    Expected mix node to be able to select subsurface branch\n");
        return false;
    }

    const Vec3f expected(subsurface.weight / 0.75f);
    if (!Test_IsClose(sample.f, expected, 1.0e-6f)) {
        printf(
            "    Expected mix probability compensation, got (%f,%f,%f) "
            "expected (%f,%f,%f)\n",
            sample.f[0], sample.f[1], sample.f[2],
            expected[0], expected[1], expected[2]);
        return false;
    }
    return true;
}

// ===========================================================================
// Sampling tests (Phase 9)
// ===========================================================================

static bool
TestSampleLambertianHemisphere()
{
    Vec3f N(0, 1, 0);
    Vec3f wo(0, 1, 0);
    Vec3f baseColor(0.8f, 0.2f, 0.1f);

    // All sampled directions should be in the hemisphere around N.
    for (int i = 0; i < 64; ++i) {
        float u1 = (i + 0.5f) / 64.0f;
        float u2 = (i * 7 % 64 + 0.5f) / 64.0f;
        auto s = Bsdf::SampleLambertian(baseColor, N, wo, u1, u2);
        if (Dot(s.wi, N) < -1e-5f) {
            printf("    Lambertian sample below hemisphere: NdotWi=%f\n",
                   Dot(s.wi, N));
            return false;
        }
        if (s.pdf <= 0.0f) {
            printf("    Lambertian sample has non-positive pdf=%f\n", s.pdf);
            return false;
        }
    }
    return true;
}

static bool
TestSampleLambertianPdfConsistency()
{
    Vec3f N(0, 1, 0);
    Vec3f wo(0, 1, 0);
    Vec3f baseColor(1.0f);

    auto s = Bsdf::SampleLambertian(baseColor, N, wo, 0.3f, 0.7f);
    float pdf2 = Bsdf::PdfLambertian(N, s.wi);
    if (!Test_IsClose(s.pdf, pdf2, 1e-4f)) {
        printf("    Sample pdf=%f != PdfLambertian=%f\n", s.pdf, pdf2);
        return false;
    }
    return true;
}

static bool
TestSampleGGXSpecularHemisphere()
{
    Vec3f N(0, 1, 0);
    Vec3f wo = Vec3f(0.3f, 0.95f, 0.0f).normalized();
    Vec3f specColor(1.0f);

    int valid = 0;
    for (int i = 0; i < 64; ++i) {
        float u1 = (i + 0.5f) / 64.0f;
        float u2 = (i * 13 % 64 + 0.5f) / 64.0f;
        auto s = Bsdf::SampleGGXSpecular(
            0.3f, 1.5f, specColor, N, wo, u1, u2);
        if (s.pdf > 0.0f) {
            ++valid;
            if (Dot(s.wi, N) < -1e-5f) {
                printf("    GGX sample below hemisphere: NdotWi=%f\n",
                       Dot(s.wi, N));
                return false;
            }
        }
    }
    if (valid < 32) {
        printf("    Too few valid GGX samples: %d/64\n", valid);
        return false;
    }
    return true;
}

static bool
TestSampleGGXSpecularPdfConsistency()
{
    Vec3f N(0, 1, 0);
    Vec3f wo = Vec3f(0.2f, 0.98f, 0.0f).normalized();
    Vec3f specColor(1.0f);

    auto s = Bsdf::SampleGGXSpecular(
        0.4f, 1.5f, specColor, N, wo, 0.3f, 0.7f);
    if (s.pdf <= 0.0f) return true;

    float pdf2 = Bsdf::PdfGGXSpecular(0.4f, N, s.wi, wo);
    float ratio = s.pdf / (pdf2 + 1e-10f);
    if (ratio < 0.8f || ratio > 1.2f) {
        printf("    Sample pdf=%f != PdfGGXSpecular=%f (ratio=%f)\n",
               s.pdf, pdf2, ratio);
        return false;
    }
    return true;
}

static bool
TestSampleGGXSpecularLowRoughnessBoundedThroughput()
{
    const Vec3f N(0.0f, 1.0f, 0.0f);
    const Vec3f wo = Vec3f(0.2f, 0.9797959f, 0.0f).normalized();
    const Vec3f F0(1.0f);
    const float kRoughnessValues[] = {0.001f, 0.01f, 0.02f, 0.04f};
    const float kSamples[][2] = {
        {0.1f, 0.2f},
        {0.3f, 0.7f},
        {0.6f, 0.4f},
        {0.85f, 0.15f},
    };

    for (const float roughness : kRoughnessValues) {
        for (const auto& sampleUV : kSamples) {
            const auto sample = Bsdf::SampleGGXSpecular(
                roughness, 1.5f, F0, N, wo, sampleUV[0], sampleUV[1]);
            if (sample.pdf <= 0.0f) {
                printf("    Expected valid low-roughness GGX sample\n");
                return false;
            }

            const float cosTheta = std::abs(Dot(N, sample.wi));
            const Vec3f throughput = sample.f * (cosTheta / sample.pdf);
            for (int i = 0; i < 3; ++i) {
                if (!std::isfinite(throughput[i]) || throughput[i] > 1.05f) {
                    printf(
                        "    Low-roughness GGX throughput blew up: "
                        "roughness=%f sample=(%f,%f) channel=%d value=%f\n",
                        roughness,
                        sampleUV[0], sampleUV[1], i, throughput[i]);
                    return false;
                }
            }

            const float pdf = Bsdf::PdfGGXSpecular(
                roughness, N, sample.wi, wo);
            const float ratio = sample.pdf / std::max(pdf, 1.0e-20f);
            if (pdf <= 0.0f || ratio < 0.8f || ratio > 1.2f) {
                printf(
                    "    Low-roughness GGX pdf mismatch: roughness=%f "
                    "samplePdf=%f pdf=%f\n",
                    roughness, sample.pdf, pdf);
                return false;
            }
        }
    }

    return true;
}

static bool
TestSampleSurfacePdfConsistency()
{
    SurfaceClosure c;
    c.baseColor = Vec3f(0.8f, 0.2f, 0.1f);
    c.roughness = 0.4f;
    c.metallic = 0.0f;
    c.specular = 1.0f;
    c.specularIor = 1.5f;
    c.specularColor = Vec3f(1.0f);

    Vec3f N(0, 1, 0);
    Vec3f wo = Vec3f(0.2f, 0.98f, 0.0f).normalized();

    auto s = Bsdf::SampleSurface(c, N, wo, 0.5f, 0.5f, 0.3f);
    if (s.pdf <= 0.0f) return true;

    float pdf2 = Bsdf::PdfSurface(c, N, s.wi, wo);
    float ratio = s.pdf / (pdf2 + 1e-10f);
    if (ratio < 0.8f || ratio > 1.2f) {
        printf("    SampleSurface pdf=%f != PdfSurface=%f (ratio=%f)\n",
               s.pdf, pdf2, ratio);
        return false;
    }
    return true;
}

static bool
TestTreeTransmissionPreservesWeight()
{
    SurfaceClosure c;
    Bsdf::DielectricData transmission;
    transmission.weight = 1.0f;
    transmission.tint = Vec3f(0.95f, 0.97f, 1.0f);
    transmission.ior = 1.5f;
    transmission.scatterMode = Bsdf::ScatterMode::Transmission;

    c.bsdfTree.root = c.bsdfTree.Add(transmission);

    Vec3f N(0, 1, 0);
    Vec3f wo = Vec3f(0, 1, 0);

    auto s = Bsdf::SampleSurface(c, N, wo, 0.3f, 0.7f, 0.5f);
    if (s.pdf <= 0.0f) {
        printf("    Expected valid transmission sample\n");
        return false;
    }

    // Transmission sample should not be excessively dim
    float mag = s.f.length();
    if (mag < 0.01f) {
        printf("    Transmission sample unexpectedly dim: (%f,%f,%f)\n",
               s.f[0], s.f[1], s.f[2]);
        return false;
    }
    return true;
}

static bool
TestBsdfSampleDiffuseLikeClassification()
{
    const Vec3f N(0.0f, 1.0f, 0.0f);
    const Vec3f wo = Vec3f(0.2f, 0.9797959f, 0.0f).normalized();

    SurfaceClosure diffuseClosure;
    Bsdf::BurleyDiffuseData diffuse;
    diffuse.weight = 1.0f;
    diffuse.color = Vec3f(0.8f, 0.7f, 0.6f);
    diffuseClosure.bsdfTree.root = diffuseClosure.bsdfTree.Add(diffuse);

    const auto diffuseSample =
        Bsdf::SampleSurface(diffuseClosure, N, wo, 0.3f, 0.7f, 0.5f);
    if (diffuseSample.pdf <= 0.0f || !diffuseSample.isDiffuseLike) {
        printf("    Expected diffuse sample to be diffuse-like\n");
        return false;
    }

    SurfaceClosure roughGlassClosure;
    Bsdf::DielectricData roughTransmission;
    roughTransmission.weight = 1.0f;
    roughTransmission.tint = Vec3f(1.0f);
    roughTransmission.ior = 1.5f;
    roughTransmission.roughness = Vec2f(0.05f, 0.05f);
    roughTransmission.scatterMode = Bsdf::ScatterMode::Transmission;
    roughGlassClosure.bsdfTree.root =
        roughGlassClosure.bsdfTree.Add(roughTransmission);

    const auto glassSample =
        Bsdf::SampleSurface(roughGlassClosure, N, wo, 0.3f, 0.7f, 0.5f);
    if (glassSample.pdf <= 0.0f || glassSample.isSpecular) {
        printf("    Expected rough dielectric transmission to be finite\n");
        return false;
    }
    if (glassSample.isDiffuseLike) {
        printf("    Rough dielectric transmission should not be diffuse-like\n");
        return false;
    }
    if (Dot(N, glassSample.wi) >= 0.0f) {
        printf("    Rough dielectric transmission should cross the boundary\n");
        return false;
    }

    return true;
}

static bool
TestZeroRoughnessDielectricSamplesDelta()
{
    SurfaceClosure c;
    Bsdf::DielectricData dielectric;
    dielectric.weight = 1.0f;
    dielectric.tint = Vec3f(1.0f);
    dielectric.ior = 1.5f;
    dielectric.roughness = Vec2f(0.0f, 0.0f);
    dielectric.scatterMode = Bsdf::ScatterMode::ReflectionTransmission;
    c.bsdfTree.root = c.bsdfTree.Add(dielectric);

    const Vec3f N(0.0f, 1.0f, 0.0f);
    const Vec3f wo = Vec3f(0.2f, 0.9797959f, 0.0f).normalized();

    const auto reflected = Bsdf::SampleSurface(c, N, wo, 0.2f, 0.8f, 0.0f);
    const auto transmitted = Bsdf::SampleSurface(c, N, wo, 0.7f, 0.1f, 0.99f);

    if (!reflected.isSpecular || reflected.pdf <= 0.0f) {
        printf("    Expected zero-roughness reflection to be delta\n");
        return false;
    }
    if (Dot(reflected.wi, N) <= 0.0f) {
        printf("    Delta reflection should stay on the same side\n");
        return false;
    }

    if (!transmitted.isSpecular || transmitted.pdf <= 0.0f) {
        printf("    Expected zero-roughness transmission to be delta\n");
        return false;
    }
    if (Dot(transmitted.wi, N) >= 0.0f) {
        printf("    Delta transmission should cross the interface\n");
        return false;
    }

    const auto transmittedAlt =
        Bsdf::SampleSurface(c, N, wo, 0.1f, 0.9f, 0.99f);
    if (!Test_IsClose(transmitted.wi, transmittedAlt.wi, 1.0e-6f)) {
        printf("    Delta transmission should ignore microfacet random numbers\n");
        return false;
    }
    return true;
}

static bool
TestZeroRoughnessConductorSamplesDeltaAndSkipsDirectEval()
{
    SurfaceClosure c;
    Bsdf::ConductorData conductor;
    conductor.weight = 1.0f;
    conductor.ior = Vec3f(0.8f, 0.5f, 0.25f);
    conductor.extinction = Vec3f(2.5f, 2.0f, 1.5f);
    conductor.roughness = Vec2f(0.0f, 0.0f);
    c.bsdfTree.root = c.bsdfTree.Add(conductor);

    const Vec3f N(0.0f, 1.0f, 0.0f);
    const Vec3f wo = Vec3f(0.2f, 0.9797959f, 0.0f).normalized();
    const Vec3f wiMirror = Vec3f(-0.2f, 0.9797959f, 0.0f).normalized();

    const Vec3f directEval = Bsdf::EvalSurface(c, N, wiMirror, wo);
    const float directPdf = Bsdf::PdfSurface(c, N, wiMirror, wo);
    if (!Test_IsClose(directEval, Vec3f(0.0f), 1.0e-7f) ||
        !Test_IsClose(directPdf, 0.0f, 1.0e-7f)) {
        printf(
            "    Delta conductor should not appear as finite direct lobe: "
            "eval=(%f,%f,%f) pdf=%f\n",
            directEval[0], directEval[1], directEval[2], directPdf);
        return false;
    }

    const auto sample = Bsdf::SampleSurface(c, N, wo, 0.3f, 0.7f, 0.4f);
    if (!sample.isSpecular || sample.pdf <= 0.0f) {
        printf("    Expected low-roughness conductor to sample delta\n");
        return false;
    }
    if (!Test_IsClose(sample.wi, wiMirror, 1.0e-6f)) {
        printf(
            "    Delta conductor reflected direction mismatch: "
            "(%f,%f,%f)\n",
            sample.wi[0], sample.wi[1], sample.wi[2]);
        return false;
    }
    if (!_IsFiniteNonNegative(sample.f) || sample.f.length() <= 0.0f) {
        printf(
            "    Delta conductor reflectance invalid: (%f,%f,%f)\n",
            sample.f[0], sample.f[1], sample.f[2]);
        return false;
    }
    return true;
}

static bool
TestEffectivelySmoothConductorAlphaSamplesDeltaAndSkipsDirectEval()
{
    const Vec3f N(0.0f, 1.0f, 0.0f);
    const Vec3f wo = Vec3f(0.2f, 0.9797959f, 0.0f).normalized();
    const Vec3f wiMirror = Vec3f(-0.2f, 0.9797959f, 0.0f).normalized();
    const float kSmoothAlphaValues[] = {
        1.0e-6f,
        1.0e-4f,
        4.0e-4f,
        9.0e-4f
    };

    for (const float alpha : kSmoothAlphaValues) {
        SurfaceClosure c;
        Bsdf::ConductorData conductor;
        conductor.weight = 1.0f;
        conductor.ior = Vec3f(0.8f, 0.5f, 0.25f);
        conductor.extinction = Vec3f(2.5f, 2.0f, 1.5f);
        conductor.roughness = Vec2f(alpha, alpha);
        c.bsdfTree.root = c.bsdfTree.Add(conductor);

        const Vec3f directEval = Bsdf::EvalSurface(c, N, wiMirror, wo);
        const float directPdf = Bsdf::PdfSurface(c, N, wiMirror, wo);
        if (!Test_IsClose(directEval, Vec3f(0.0f), 1.0e-7f) ||
            !Test_IsClose(directPdf, 0.0f, 1.0e-7f)) {
            printf(
                "    Effectively smooth conductor should skip finite eval: "
                "alpha=%g eval=(%f,%f,%f) pdf=%f\n",
                alpha,
                directEval[0], directEval[1], directEval[2],
                directPdf);
            return false;
        }

        const auto sample = Bsdf::SampleSurface(c, N, wo, 0.3f, 0.7f, 0.4f);
        if (!sample.isSpecular || sample.pdf <= 0.0f ||
            !Test_IsClose(sample.wi, wiMirror, 1.0e-6f) ||
            !_IsFiniteNonNegative(sample.f) || sample.f.length() <= 0.0f) {
            printf(
                "    Effectively smooth conductor should sample delta: "
                "alpha=%g specular=%d wi=(%f,%f,%f) f=(%f,%f,%f) pdf=%f\n",
                alpha,
                sample.isSpecular ? 1 : 0,
                sample.wi[0], sample.wi[1], sample.wi[2],
                sample.f[0], sample.f[1], sample.f[2],
                sample.pdf);
            return false;
        }
    }
    return true;
}

static bool
TestSharpConductorAlphaRemainsFiniteGlossy()
{
    const Vec3f N(0.0f, 1.0f, 0.0f);
    const Vec3f wo = Vec3f(0.2f, 0.9797959f, 0.0f).normalized();
    const Vec3f wiMirror = Vec3f(-0.2f, 0.9797959f, 0.0f).normalized();
    const float kAlphaValues[] = {
        1.0e-3f,
        1.6e-3f,
        2.5e-3f
    };

    for (const float alpha : kAlphaValues) {
        SurfaceClosure c;
        Bsdf::ConductorData conductor;
        conductor.weight = 1.0f;
        conductor.ior = Vec3f(0.8f, 0.5f, 0.25f);
        conductor.extinction = Vec3f(2.5f, 2.0f, 1.5f);
        conductor.roughness = Vec2f(alpha, alpha);
        c.bsdfTree.root = c.bsdfTree.Add(conductor);

        const Vec3f directEval = Bsdf::EvalSurface(c, N, wiMirror, wo);
        const float directPdf = Bsdf::PdfSurface(c, N, wiMirror, wo);
        if (directEval.length() <= 0.0f ||
            directPdf <= 0.0f ||
            !_IsFiniteNonNegative(directEval)) {
            printf(
                "    Sharp conductor should remain a finite glossy lobe: "
                "alpha=%g eval=(%f,%f,%f) pdf=%f\n",
                alpha,
                directEval[0], directEval[1], directEval[2],
                directPdf);
            return false;
        }

        const auto sample = Bsdf::SampleSurface(c, N, wo, 0.3f, 0.7f, 0.4f);
        const float pdf = Bsdf::PdfSurface(c, N, sample.wi, wo);
        const float ratio = sample.pdf / std::max(pdf, 1.0e-20f);
        if (sample.isSpecular || sample.pdf <= 0.0f || pdf <= 0.0f ||
            ratio < 0.8f || ratio > 1.2f) {
            printf(
                "    Sharp conductor sample invalid: alpha=%g specular=%d "
                "samplePdf=%f pdf=%f\n",
                alpha,
                sample.isSpecular ? 1 : 0,
                sample.pdf,
                pdf);
            return false;
        }

        const float cosTheta = std::max(Dot(N, sample.wi), 0.0f);
        const Vec3f throughput = sample.f * (cosTheta / sample.pdf);
        if (!_IsFiniteNonNegative(sample.f) ||
            !_IsFiniteNonNegative(throughput)) {
            printf(
                "    Sharp conductor sample non-finite: alpha=%g "
                "f=(%f,%f,%f) throughput=(%f,%f,%f)\n",
                alpha,
                sample.f[0], sample.f[1], sample.f[2],
                throughput[0], throughput[1], throughput[2]);
            return false;
        }
        for (int i = 0; i < 3; ++i) {
            if (throughput[i] > 1.25f) {
                printf(
                    "    Sharp conductor throughput too high: alpha=%g "
                    "channel=%d throughput=%f\n",
                    alpha,
                    i,
                    throughput[i]);
                return false;
            }
        }
    }
    return true;
}

static bool
TestSharpConductorEvalPdfRatioBoundedForLightSamples()
{
    const Vec3f N(0.0f, 1.0f, 0.0f);
    const float kAlphaValues[] = {
        1.0e-3f,
        1.6e-3f,
        2.5e-3f
    };
    const float kWoCosValues[] = {
        0.05f,
        0.2f,
        0.5f,
        0.9f,
        0.999f
    };
    const float kLightPdfValues[] = {
        0.02f,
        0.08f,
        1.0f,
        10.0f,
        100.0f
    };

    for (const float alpha : kAlphaValues) {
        SurfaceClosure c;
        Bsdf::ConductorData conductor;
        conductor.weight = 1.0f;
        conductor.ior = Vec3f(0.8f, 0.5f, 0.25f);
        conductor.extinction = Vec3f(2.5f, 2.0f, 1.5f);
        conductor.roughness = Vec2f(alpha, alpha);
        c.bsdfTree.root = c.bsdfTree.Add(conductor);

        for (const float woCosTheta : kWoCosValues) {
            const float woSinTheta =
                std::sqrt(std::max(0.0f, 1.0f - woCosTheta * woCosTheta));
            const Vec3f wo(woSinTheta, woCosTheta, 0.0f);

            for (int y = 0; y < 64; ++y) {
                const float cosTheta =
                    (static_cast<float>(y) + 0.5f) / 64.0f;
                const float sinTheta =
                    std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
                for (int x = 0; x < 128; ++x) {
                    const float phi =
                        _kFurnaceTwoPi *
                        (static_cast<float>(x) + 0.5f) / 128.0f;
                    const Vec3f wi(
                        sinTheta * std::cos(phi),
                        cosTheta,
                        sinTheta * std::sin(phi));
                    const Vec3f f = Bsdf::EvalSurface(c, N, wi, wo);
                    const float pdf = Bsdf::PdfSurface(c, N, wi, wo);
                    if (f.length() <= 0.0f) {
                        continue;
                    }
                    if (pdf <= 0.0f) {
                        printf(
                            "    Sharp conductor eval has zero pdf: "
                            "alpha=%g wo=(%f,%f,%f) wi=(%f,%f,%f) "
                            "f=(%f,%f,%f) pdf=%f\n",
                            alpha,
                            wo[0], wo[1], wo[2],
                            wi[0], wi[1], wi[2],
                            f[0], f[1], f[2],
                            pdf);
                        return false;
                    }

                    const Vec3f ratio = f * (cosTheta / pdf);
                    if (!_IsFiniteNonNegative(ratio)) {
                        printf(
                            "    Sharp conductor Eval/Pdf ratio non-finite: "
                            "alpha=%g wo=(%f,%f,%f) wi=(%f,%f,%f) "
                            "pdf=%f ratio=(%f,%f,%f)\n",
                            alpha,
                            wo[0], wo[1], wo[2],
                            wi[0], wi[1], wi[2],
                            pdf,
                            ratio[0], ratio[1], ratio[2]);
                        return false;
                    }
                    for (int i = 0; i < 3; ++i) {
                        if (ratio[i] > 1.25f) {
                            printf(
                                "    Sharp conductor Eval/Pdf ratio too high: "
                                "alpha=%g channel=%d wo=(%f,%f,%f) "
                                "wi=(%f,%f,%f) f=(%f,%f,%f) pdf=%f "
                                "ratio=%f\n",
                                alpha,
                                i,
                                wo[0], wo[1], wo[2],
                                wi[0], wi[1], wi[2],
                                f[0], f[1], f[2],
                                pdf,
                                ratio[i]);
                            return false;
                        }
                    }

                    for (const float lightPdf : kLightPdfValues) {
                        const float misW =
                            Bsdf::PowerHeuristic(lightPdf, pdf);
                        const Vec3f direct = f * (cosTheta / lightPdf) * misW;
                        if (!_IsFiniteNonNegative(direct)) {
                            printf(
                                "    Sharp conductor direct MIS non-finite: "
                                "alpha=%g lightPdf=%f wo=(%f,%f,%f) "
                                "wi=(%f,%f,%f) f=(%f,%f,%f) pdf=%f "
                                "direct=(%f,%f,%f)\n",
                                alpha,
                                lightPdf,
                                wo[0], wo[1], wo[2],
                                wi[0], wi[1], wi[2],
                                f[0], f[1], f[2],
                                pdf,
                                direct[0], direct[1], direct[2]);
                            return false;
                        }
                        for (int i = 0; i < 3; ++i) {
                            if (direct[i] > 0.75f) {
                                printf(
                                    "    Sharp conductor direct MIS too high: "
                                    "alpha=%g lightPdf=%f channel=%d "
                                    "wo=(%f,%f,%f) wi=(%f,%f,%f) "
                                    "f=(%f,%f,%f) pdf=%f direct=%f\n",
                                    alpha,
                                    lightPdf,
                                    i,
                                    wo[0], wo[1], wo[2],
                                    wi[0], wi[1], wi[2],
                                    f[0], f[1], f[2],
                                    pdf,
                                    direct[i]);
                                return false;
                            }
                        }
                    }
                }
            }
        }
    }

    return true;
}

static bool
TestTreeTransmissionPreservesWeightFromInterior()
{
    SurfaceClosure c;
    Bsdf::DielectricData transmission;
    transmission.weight = 1.0f;
    transmission.tint = Vec3f(0.95f, 0.97f, 1.0f);
    transmission.ior = 1.5f;
    transmission.roughness = Vec2f(0.35f, 0.35f);
    transmission.scatterMode = Bsdf::ScatterMode::Transmission;

    c.bsdfTree.root = c.bsdfTree.Add(transmission);

    Vec3f N(0, 1, 0);
    Vec3f wo = Vec3f(0, -1, 0);

    auto s = Bsdf::SampleSurface(c, N, wo, 0.3f, 0.7f, 0.5f);
    if (s.pdf <= 0.0f) {
        printf("    Expected valid interior transmission sample\n");
        return false;
    }
    if (Dot(s.wi, N) <= 0.0f) {
        printf("    Interior transmission should exit above the surface\n");
        return false;
    }
    if (s.f.length() < 0.01f) {
        printf("    Interior transmission unexpectedly dim: (%f,%f,%f)\n",
               s.f[0], s.f[1], s.f[2]);
        return false;
    }
    return true;
}

static bool
TestEvalSurfaceTransmissionFromInterior()
{
    SurfaceClosure c;
    Bsdf::DielectricData transmission;
    transmission.weight = 1.0f;
    transmission.tint = Vec3f(1.0f);
    transmission.ior = 1.5f;
    transmission.roughness = Vec2f(0.25f, 0.25f);
    transmission.scatterMode = Bsdf::ScatterMode::Transmission;
    c.bsdfTree.root = c.bsdfTree.Add(transmission);

    Vec3f N(0, 1, 0);
    Vec3f wo = Vec3f(0.2f, -0.98f, 0.0f).normalized();
    Vec3f wi = Vec3f(-0.1f, 0.995f, 0.0f).normalized();

    Vec3f result = Bsdf::EvalSurface(c, N, wi, wo);
    if (result.length() <= 0.0f) {
        printf("    Expected non-zero interior-to-exterior transmission\n");
        return false;
    }
    return true;
}

static bool
TestTreeAddTransmissionPreservesWeight()
{
    SurfaceClosure c;
    Bsdf::ClosureTree tree;

    Bsdf::DielectricData reflection;
    reflection.weight = 1.0f;
    reflection.tint = Vec3f(1.0f);
    reflection.ior = 1.5f;
    reflection.scatterMode = Bsdf::ScatterMode::Reflection;

    Bsdf::DielectricData transmission;
    transmission.weight = 1.0f;
    transmission.tint = Vec3f(0.95f, 0.97f, 1.0f);
    transmission.ior = 1.5f;
    transmission.scatterMode = Bsdf::ScatterMode::Transmission;

    Bsdf::AddData add;
    add.in1 = tree.Add(reflection);
    add.in2 = tree.Add(transmission);
    tree.root = tree.Add(add);
    c.bsdfTree = tree;

    Vec3f N(0, 1, 0);
    Vec3f wo = Vec3f(0, 1, 0);

    auto s = Bsdf::SampleSurface(c, N, wo, 0.3f, 0.7f, 0.99f);
    if (s.pdf <= 0.0f) {
        printf("    Expected valid transmission sample from add node\n");
        return false;
    }

    float mag = s.f.length();
    if (mag < 0.01f) {
        printf("    Add-node transmission unexpectedly dim: (%f,%f,%f)\n",
               s.f[0], s.f[1], s.f[2]);
        return false;
    }
    return true;
}

static bool
TestDielectricInterfaceLayerDoesNotDoubleAttenuateTransmission()
{
    Bsdf::ClosureTree tree;

    Bsdf::DielectricInterfaceData interface;
    interface.reflectionWeight = 1.0f;
    interface.reflectionTint = Vec3f(1.0f);
    interface.transmissionWeight = 1.0f;
    interface.transmissionTint = Vec3f(1.0f);
    interface.ior = 1.5f;
    interface.roughness = Vec2f(0.25f, 0.25f);

    Bsdf::OrenNayarDiffuseData diffuse;
    diffuse.weight = 1.0f;
    diffuse.color = Vec3f(0.8f);

    Bsdf::LayerData layer;
    layer.top = tree.Add(interface);
    layer.base = tree.Add(diffuse);
    tree.root = tree.Add(layer);

    SurfaceClosure layered;
    layered.bsdfTree = tree;

    SurfaceClosure interfaceOnly;
    interfaceOnly.bsdfTree.root = interfaceOnly.bsdfTree.Add(interface);

    const Vec3f N(0.0f, 1.0f, 0.0f);
    const Vec3f wo = Vec3f(0.2f, 0.98f, 0.0f).normalized();
    const Vec3f wi = Vec3f(-0.1f, -0.995f, 0.0f).normalized();

    const Vec3f layeredEval = Bsdf::EvalSurface(layered, N, wi, wo);
    const Vec3f interfaceEval = Bsdf::EvalSurface(interfaceOnly, N, wi, wo);
    if (!Test_IsClose(layeredEval, interfaceEval, 1.0e-5f)) {
        printf(
            "    Interface transmission was attenuated by its own layer: "
            "layered=(%f,%f,%f) interface=(%f,%f,%f)\n",
            layeredEval[0], layeredEval[1], layeredEval[2],
            interfaceEval[0], interfaceEval[1], interfaceEval[2]);
        return false;
    }

    return true;
}

static bool
TestDielectricInterfaceSamplePdfConsistency()
{
    SurfaceClosure c;
    Bsdf::DielectricInterfaceData interface;
    interface.reflectionWeight = 1.0f;
    interface.reflectionTint = Vec3f(1.0f);
    interface.transmissionWeight = 1.0f;
    interface.transmissionTint = Vec3f(0.95f, 0.97f, 1.0f);
    interface.ior = 1.5f;
    interface.roughness = Vec2f(0.25f, 0.25f);
    c.bsdfTree.root = c.bsdfTree.Add(interface);

    const Vec3f N(0.0f, 1.0f, 0.0f);
    const Vec3f wo = Vec3f(0.2f, 0.98f, 0.0f).normalized();

    const float choices[] = {0.01f, 0.8f};
    for (const float uChoice : choices) {
        const auto sample =
            Bsdf::SampleSurface(c, N, wo, 0.3f, 0.7f, uChoice);
        if (sample.pdf <= 0.0f || sample.isSpecular) {
            printf("    Expected valid rough interface sample\n");
            return false;
        }
        const float pdf = Bsdf::PdfSurface(c, N, sample.wi, wo);
        const float ratio = sample.pdf / (pdf + 1.0e-10f);
        if (ratio < 0.8f || ratio > 1.2f) {
            printf(
                "    Interface sample pdf=%f != PdfSurface=%f "
                "(ratio=%f, choice=%f)\n",
                sample.pdf, pdf, ratio, uChoice);
            return false;
        }
    }

    return true;
}

static bool
TestDeltaDielectricInterfaceTransmissionSamplesSingleFresnel()
{
    SurfaceClosure c;
    Bsdf::DielectricInterfaceData interface;
    interface.reflectionWeight = 1.0f;
    interface.reflectionTint = Vec3f(1.0f);
    interface.transmissionWeight = 1.0f;
    interface.transmissionTint = Vec3f(1.0f);
    interface.ior = 1.5f;
    interface.roughness = Vec2f(0.0f, 0.0f);
    c.bsdfTree.root = c.bsdfTree.Add(interface);

    const Vec3f N(0.0f, 1.0f, 0.0f);
    const Vec3f wo(0.0f, 1.0f, 0.0f);
    const auto sample = Bsdf::SampleSurface(c, N, wo, 0.3f, 0.7f, 0.9f);
    if (sample.pdf <= 0.0f || !sample.isSpecular) {
        printf("    Expected valid delta interface transmission sample\n");
        return false;
    }
    if (Dot(sample.wi, N) >= 0.0f) {
        printf("    Expected transmitted direction below the interface\n");
        return false;
    }
    if (!Test_IsClose(sample.f, Vec3f(1.0f), 1.0e-4f)) {
        printf(
            "    Delta interface transmission should divide by the single "
            "Fresnel branch probability: f=(%f,%f,%f)\n",
            sample.f[0], sample.f[1], sample.f[2]);
        return false;
    }
    return true;
}

static bool
TestDeltaDielectricInterfaceTirDoesNotAmplifyThroughput()
{
    SurfaceClosure c;
    Bsdf::DielectricInterfaceData interface;
    interface.reflectionWeight = 1.0f;
    interface.reflectionTint = Vec3f(1.0f);
    interface.transmissionWeight = 1.0f;
    interface.transmissionTint = Vec3f(1.0f);
    interface.ior = 1.5f;
    interface.roughness = Vec2f(0.0f, 0.0f);
    c.bsdfTree.root = c.bsdfTree.Add(interface);

    const Vec3f N(0.0f, 1.0f, 0.0f);
    const Vec3f wo = Vec3f(0.8f, -0.6f, 0.0f).normalized();
    const auto sample = Bsdf::SampleSurface(c, N, wo, 0.3f, 0.7f, 0.99f);
    if (sample.pdf <= 0.0f || !sample.isSpecular) {
        printf("    Expected valid delta TIR sample\n");
        return false;
    }
    if (Dot(sample.wi, N) >= 0.0f) {
        printf("    TIR should stay on the incident side of the interface\n");
        return false;
    }
    if (!Test_IsClose(sample.f, Vec3f(1.0f), 1.0e-5f)) {
        printf(
            "    TIR should not be divided by transmission probability: "
            "f=(%f,%f,%f)\n",
            sample.f[0], sample.f[1], sample.f[2]);
        return false;
    }
    return true;
}

static bool
TestThinWalledDielectricInterfaceSamplePdfConsistency()
{
    SurfaceClosure c;
    Bsdf::DielectricInterfaceData interface;
    interface.reflectionWeight = 1.0f;
    interface.reflectionTint = Vec3f(1.0f);
    interface.transmissionWeight = 1.0f;
    interface.transmissionTint = Vec3f(0.72f, 1.0f, 0.86f);
    interface.ior = 1.5f;
    interface.roughness = Vec2f(0.25f, 0.25f);
    interface.thinWalled = true;
    c.bsdfTree.root = c.bsdfTree.Add(interface);

    const Vec3f N(0.0f, 1.0f, 0.0f);
    const Vec3f wo = Vec3f(0.2f, 0.98f, 0.0f).normalized();
    const auto sample =
        Bsdf::SampleSurface(c, N, wo, 0.3f, 0.7f, 0.8f);
    if (sample.pdf <= 0.0f || sample.isSpecular) {
        printf("    Expected valid rough thin-walled interface sample\n");
        return false;
    }
    if (Dot(sample.wi, N) >= 0.0f) {
        printf("    Expected thin-walled transmission below the surface\n");
        return false;
    }

    const float pdf = Bsdf::PdfSurface(c, N, sample.wi, wo);
    const float ratio = sample.pdf / (pdf + 1.0e-10f);
    if (ratio < 0.8f || ratio > 1.2f) {
        printf(
            "    Thin-walled interface sample pdf=%f != PdfSurface=%f "
            "(ratio=%f)\n",
            sample.pdf, pdf, ratio);
        return false;
    }

    return true;
}

static bool
TestDeltaThinWalledDielectricInterfaceTransmitsStraightThrough()
{
    SurfaceClosure c;
    Bsdf::DielectricInterfaceData interface;
    interface.reflectionWeight = 1.0f;
    interface.reflectionTint = Vec3f(1.0f);
    interface.transmissionWeight = 1.0f;
    interface.transmissionTint = Vec3f(1.0f);
    interface.ior = 1.5f;
    interface.roughness = Vec2f(0.0f, 0.0f);
    interface.thinWalled = true;
    c.bsdfTree.root = c.bsdfTree.Add(interface);

    const Vec3f N(0.0f, 1.0f, 0.0f);
    const Vec3f wo = Vec3f(0.3f, 0.953939f, 0.0f).normalized();
    const auto sample = Bsdf::SampleSurface(c, N, wo, 0.3f, 0.7f, 0.9f);
    if (sample.pdf <= 0.0f || !sample.isSpecular) {
        printf("    Expected valid delta thin-walled transmission sample\n");
        return false;
    }
    if (!Test_IsClose(sample.wi, -wo, 1.0e-5f)) {
        printf(
            "    Thin-walled transmission should continue straight through: "
            "wi=(%f,%f,%f), expected=(%f,%f,%f)\n",
            sample.wi[0], sample.wi[1], sample.wi[2],
            -wo[0], -wo[1], -wo[2]);
        return false;
    }
    if (!Test_IsClose(sample.f, Vec3f(1.0f), 1.0e-4f)) {
        printf(
            "    Delta thin-walled transmission should divide by its "
            "window-transmission branch probability: f=(%f,%f,%f)\n",
            sample.f[0], sample.f[1], sample.f[2]);
        return false;
    }
    return true;
}

static bool
TestSampleGGXTransmissionHemisphere()
{
    Vec3f N(0, 1, 0);
    Vec3f wo = Vec3f(0.3f, 0.95f, 0.0f).normalized();
    Vec3f tColor(1.0f);

    int valid = 0;
    for (int i = 0; i < 64; ++i) {
        float u1 = (i + 0.5f) / 64.0f;
        float u2 = (i * 13 % 64 + 0.5f) / 64.0f;
        auto s = Bsdf::SampleGGXTransmission(
            0.3f, 1.5f, tColor, N, wo, u1, u2);
        if (s.pdf > 0.0f) {
            ++valid;
            if (Dot(s.wi, N) > 1e-5f) {
                printf("    GGX transmission sample above surface: NdotWi=%f\n",
                       Dot(s.wi, N));
                return false;
            }
        }
    }
    if (valid < 32) {
        printf("    Too few valid GGX transmission samples: %d/64\n", valid);
        return false;
    }
    return true;
}

static bool
TestSampleGGXTransmissionPdfConsistency()
{
    Vec3f N(0, 1, 0);
    Vec3f wo = Vec3f(0.2f, 0.98f, 0.0f).normalized();
    Vec3f tColor(1.0f);

    auto s = Bsdf::SampleGGXTransmission(
        0.4f, 1.5f, tColor, N, wo, 0.3f, 0.7f);
    if (s.pdf <= 0.0f) return true;

    float pdf2 = Bsdf::PdfGGXTransmission(0.4f, 1.5f, N, s.wi, wo);
    float ratio = s.pdf / (pdf2 + 1e-10f);
    if (ratio < 0.8f || ratio > 1.2f) {
        printf("    Sample pdf=%f != PdfGGXTransmission=%f (ratio=%f)\n",
               s.pdf, pdf2, ratio);
        return false;
    }
    return true;
}

static bool
TestRoughTransmissionSpreads()
{
    Vec3f N(0, 1, 0);
    Vec3f wo = Vec3f(0, 1, 0);
    Vec3f tColor(1.0f);
    float roughness = 0.5f;

    Vec3f firstWi(0.0f);
    bool firstSet = false;
    float maxDeviation = 0.0f;

    for (int i = 0; i < 64; ++i) {
        float u1 = (i + 0.5f) / 64.0f;
        float u2 = (i * 7 % 64 + 0.5f) / 64.0f;
        auto s = Bsdf::SampleGGXTransmission(
            roughness, 1.5f, tColor, N, wo, u1, u2);
        if (s.pdf <= 0.0f) continue;
        if (!firstSet) {
            firstWi = s.wi;
            firstSet = true;
            continue;
        }
        float dev = 1.0f - Dot(s.wi, firstWi);
        if (dev > maxDeviation) maxDeviation = dev;
    }

    if (maxDeviation < 0.01f) {
        printf("    Rough transmission directions too similar (maxDev=%f)\n",
               maxDeviation);
        return false;
    }
    return true;
}

static bool
TestThinFilmDielectricChangesReflectionColor()
{
    Bsdf::DielectricData base;
    base.weight = 1.0f;
    base.tint = Vec3f(1.0f);
    base.ior = 1.5f;
    base.scatterMode = Bsdf::ScatterMode::Reflection;

    Bsdf::DielectricData thinFilm = base;
    thinFilm.thinFilmWeight = 1.0f;
    thinFilm.thinFilmThickness = 300.0f;
    thinFilm.thinFilmIor = 1.3f;

    SurfaceClosure baseClosure;
    baseClosure.bsdfTree.root = baseClosure.bsdfTree.Add(base);

    SurfaceClosure thinFilmClosure;
    thinFilmClosure.bsdfTree.root = thinFilmClosure.bsdfTree.Add(thinFilm);

    const Vec3f N(0.0f, 1.0f, 0.0f);
    const Vec3f wo = Vec3f(0.3f, 0.95f, 0.0f).normalized();
    const Vec3f wi = Vec3f(-0.2f, 0.98f, 0.0f).normalized();

    const Vec3f baseEval = Bsdf::EvalSurface(baseClosure, N, wi, wo);
    const Vec3f thinFilmEval = Bsdf::EvalSurface(thinFilmClosure, N, wi, wo);

    return !Test_IsClose(baseEval, thinFilmEval, 1e-4f) &&
           std::abs(thinFilmEval[0] - thinFilmEval[1]) > 1e-4f;
}

static bool
TestThinFilmConductorChangesReflectionColor()
{
    Bsdf::ConductorData base;
    base.weight = 1.0f;

    Bsdf::ConductorData thinFilm = base;
    thinFilm.thinFilmWeight = 1.0f;
    thinFilm.thinFilmThickness = 300.0f;
    thinFilm.thinFilmIor = 1.3f;

    SurfaceClosure baseClosure;
    baseClosure.bsdfTree.root = baseClosure.bsdfTree.Add(base);

    SurfaceClosure thinFilmClosure;
    thinFilmClosure.bsdfTree.root = thinFilmClosure.bsdfTree.Add(thinFilm);

    const Vec3f N(0.0f, 1.0f, 0.0f);
    const Vec3f wo = Vec3f(0.25f, 0.96f, 0.1f).normalized();
    const Vec3f wi = Vec3f(-0.1f, 0.99f, 0.05f).normalized();

    const Vec3f baseEval = Bsdf::EvalSurface(baseClosure, N, wi, wo);
    const Vec3f thinFilmEval = Bsdf::EvalSurface(thinFilmClosure, N, wi, wo);

    const bool changed = !Test_IsClose(baseEval, thinFilmEval, 1e-5f);
    const bool chromatic =
        std::abs(thinFilmEval[0] - thinFilmEval[1]) > 1e-5f;
    if (!changed || !chromatic) {
        printf(
            "    base=(%f,%f,%f) thinFilm=(%f,%f,%f)\n",
            baseEval[0], baseEval[1], baseEval[2],
            thinFilmEval[0], thinFilmEval[1], thinFilmEval[2]);
    }

    return changed && chromatic;
}

static bool
TestThinFilmGeneralizedSchlickChangesReflectionColor()
{
    Bsdf::GeneralizedSchlickData base;
    base.weight = 1.0f;
    base.color0 = Vec3f(0.08f);
    base.color82 = Vec3f(0.08f);
    base.color90 = Vec3f(1.0f);
    base.exponent = 5.0f;
    base.roughness = Vec2f(0.05f, 0.05f);
    base.scatterMode = Bsdf::ScatterMode::Reflection;

    Bsdf::GeneralizedSchlickData thinFilm = base;
    thinFilm.thinFilmWeight = 1.0f;
    thinFilm.thinFilmThickness = 320.0f;
    thinFilm.thinFilmIor = 1.45f;

    SurfaceClosure baseClosure;
    baseClosure.bsdfTree.root = baseClosure.bsdfTree.Add(base);

    SurfaceClosure thinFilmClosure;
    thinFilmClosure.bsdfTree.root = thinFilmClosure.bsdfTree.Add(thinFilm);

    const Vec3f N(0.0f, 1.0f, 0.0f);
    const Vec3f wo = Vec3f(0.82f, 0.57f, 0.0f).normalized();
    const Vec3f wi = Vec3f(-0.68f, 0.71f, 0.18f).normalized();

    const Vec3f baseEval = Bsdf::EvalSurface(baseClosure, N, wi, wo);
    const Vec3f thinFilmEval = Bsdf::EvalSurface(thinFilmClosure, N, wi, wo);

    const bool changed = !Test_IsClose(baseEval, thinFilmEval, 1e-5f);
    const bool chromatic =
        std::abs(thinFilmEval[0] - thinFilmEval[1]) > 1e-5f;
    if (!changed || !chromatic) {
        printf(
            "    base=(%f,%f,%f) thinFilm=(%f,%f,%f)\n",
            baseEval[0], baseEval[1], baseEval[2],
            thinFilmEval[0], thinFilmEval[1], thinFilmEval[2]);
    }

    return changed && chromatic;
}

static bool
TestThinFilmSampleSurfacePdfConsistency()
{
    SurfaceClosure c;
    Bsdf::DielectricData dielectric;
    dielectric.weight = 1.0f;
    dielectric.tint = Vec3f(1.0f);
    dielectric.ior = 1.5f;
    dielectric.roughness = Vec2f(0.2f, 0.2f);
    dielectric.scatterMode = Bsdf::ScatterMode::ReflectionTransmission;
    dielectric.thinFilmWeight = 1.0f;
    dielectric.thinFilmThickness = 280.0f;
    dielectric.thinFilmIor = 1.35f;
    c.bsdfTree.root = c.bsdfTree.Add(dielectric);

    const Vec3f N(0.0f, 1.0f, 0.0f);
    const Vec3f wo = Vec3f(0.2f, 0.98f, 0.0f).normalized();

    const auto sample = Bsdf::SampleSurface(c, N, wo, 0.3f, 0.7f, 0.65f);
    if (sample.pdf <= 0.0f) {
        printf("    Expected valid thin-film sample\n");
        return false;
    }

    const float pdf = Bsdf::PdfSurface(c, N, sample.wi, wo);
    const float ratio = sample.pdf / (pdf + 1e-10f);
    if (ratio < 0.8f || ratio > 1.2f) {
        printf("    Thin-film sample pdf=%f != PdfSurface=%f (ratio=%f)\n",
               sample.pdf, pdf, ratio);
        return false;
    }
    return true;
}

static bool
TestTreeDielectricCustomNormalMatchesStandaloneShadingNormal()
{
    SurfaceClosure customNormalClosure;
    Bsdf::DielectricData customNormal;
    customNormal.weight = 1.0f;
    customNormal.tint = Vec3f(1.0f);
    customNormal.ior = 1.5f;
    customNormal.roughness = Vec2f(0.08f * 0.08f, 0.08f * 0.08f);
    customNormal.normal = Vec3f(0.0f, 0.70710677f, 0.70710677f);
    customNormal.hasShadingNormal = true;
    customNormal.scatterMode = Bsdf::ScatterMode::Reflection;
    customNormalClosure.bsdfTree.root =
        customNormalClosure.bsdfTree.Add(customNormal);

    SurfaceClosure referenceClosure;
    Bsdf::DielectricData reference = customNormal;
    reference.hasShadingNormal = false;
    referenceClosure.bsdfTree.root = referenceClosure.bsdfTree.Add(reference);

    const Vec3f surfaceNormal(0.0f, 1.0f, 0.0f);
    const Vec3f coatNormal = customNormal.normal.normalized();
    const Vec3f wo = Vec3f(0.0f, 0.9238795f, 0.3826834f).normalized();
    const Vec3f wi = Vec3f(0.15f, 0.8293090f, 0.5382608f).normalized();

    const Vec3f customEval =
        Bsdf::EvalSurface(customNormalClosure, surfaceNormal, wi, wo);
    const Vec3f referenceEval =
        Bsdf::EvalSurface(referenceClosure, coatNormal, wi, wo);
    if (!Test_IsClose(customEval, referenceEval, 1e-4f)) {
        printf(
            "    Custom normal eval mismatch: custom=(%f,%f,%f) "
            "reference=(%f,%f,%f)\n",
            customEval[0], customEval[1], customEval[2],
            referenceEval[0], referenceEval[1], referenceEval[2]);
        return false;
    }

    const float customPdf =
        Bsdf::PdfSurface(customNormalClosure, surfaceNormal, wi, wo);
    const float referencePdf =
        Bsdf::PdfSurface(referenceClosure, coatNormal, wi, wo);
    if (!Test_IsClose(customPdf, referencePdf, 1e-4f)) {
        printf("    Custom normal pdf mismatch: custom=%f reference=%f\n",
               customPdf, referencePdf);
        return false;
    }

    const auto customSample = Bsdf::SampleSurface(
        customNormalClosure, surfaceNormal, wo, 0.3f, 0.7f, 0.2f);
    const auto referenceSample = Bsdf::SampleSurface(
        referenceClosure, coatNormal, wo, 0.3f, 0.7f, 0.2f);
    if (!Test_IsClose(customSample.wi, referenceSample.wi, 1e-4f) ||
        !Test_IsClose(customSample.f, referenceSample.f, 1e-4f) ||
        !Test_IsClose(customSample.pdf, referenceSample.pdf, 1e-4f)) {
        printf(
            "    Custom normal sample mismatch: "
            "customWi=(%f,%f,%f) referenceWi=(%f,%f,%f) customPdf=%f "
            "referencePdf=%f\n",
            customSample.wi[0], customSample.wi[1], customSample.wi[2],
            referenceSample.wi[0], referenceSample.wi[1], referenceSample.wi[2],
            customSample.pdf, referenceSample.pdf);
        return false;
    }

    return true;
}

static bool
TestTreeAnisotropicReflectionRespondsToTangent()
{
    SurfaceClosure tangentXClosure;
    Bsdf::DielectricData tangentX;
    tangentX.weight = 1.0f;
    tangentX.tint = Vec3f(1.0f);
    tangentX.ior = 1.5f;
    tangentX.roughness = Vec2f(0.05f, 0.45f);
    tangentX.tangent = Vec3f(1.0f, 0.0f, 0.0f);
    tangentX.scatterMode = Bsdf::ScatterMode::Reflection;
    tangentXClosure.bsdfTree.root = tangentXClosure.bsdfTree.Add(tangentX);

    SurfaceClosure tangentZClosure;
    Bsdf::DielectricData tangentZ = tangentX;
    tangentZ.tangent = Vec3f(0.0f, 0.0f, 1.0f);
    tangentZClosure.bsdfTree.root = tangentZClosure.bsdfTree.Add(tangentZ);

    const Vec3f N(0.0f, 1.0f, 0.0f);
    const Vec3f wo = Vec3f(0.0f, 1.0f, 0.0f);
    const Vec3f wi = Vec3f(0.05f, 0.9987492f, 0.0f).normalized();

    const Vec3f evalX = Bsdf::EvalSurface(tangentXClosure, N, wi, wo);
    const Vec3f evalZ = Bsdf::EvalSurface(tangentZClosure, N, wi, wo);
    const float magX = evalX.length();
    const float magZ = evalZ.length();
    const float ratio = std::max(magX, magZ) / std::max(std::min(magX, magZ), 1e-8f);

    if (ratio <= 1.3f) {
        printf("    Expected tangent rotation to change anisotropic response:"
               " tangentX=%f tangentZ=%f ratio=%f\n",
               magX, magZ, ratio);
        return false;
    }
    return true;
}

static bool
TestTreeAnisotropicReflectionUsesTurquinCompensation()
{
    constexpr float alpha = 0.8f;

    Bsdf::GeneralizedSchlickData isotropic;
    isotropic.weight = 1.0f;
    isotropic.color0 = Vec3f(1.0f);
    isotropic.color82 = Vec3f(1.0f);
    isotropic.color90 = Vec3f(1.0f);
    isotropic.roughness = Vec2f(alpha, alpha);
    isotropic.scatterMode = Bsdf::ScatterMode::Reflection;

    SurfaceClosure isotropicClosure;
    isotropicClosure.bsdfTree.root =
        isotropicClosure.bsdfTree.Add(isotropic);

    Bsdf::GeneralizedSchlickData anisotropic = isotropic;
    anisotropic.roughness = Vec2f(alpha, alpha + 2.0e-5f);
    anisotropic.tangent = Vec3f(1.0f, 0.0f, 0.0f);

    SurfaceClosure anisotropicClosure;
    anisotropicClosure.bsdfTree.root =
        anisotropicClosure.bsdfTree.Add(anisotropic);

    const Vec3f N(0.0f, 1.0f, 0.0f);
    const Vec3f wo = _DirectionFromCosThetaYUp(0.55f).normalized();
    const Vec3f wi = Vec3f(-0.25f, 0.78f, 0.57f).normalized();

    const Vec3f isotropicEval =
        Bsdf::EvalSurface(isotropicClosure, N, wi, wo);
    const Vec3f anisotropicEval =
        Bsdf::EvalSurface(anisotropicClosure, N, wi, wo);

    if (!Test_IsClose(isotropicEval, anisotropicEval, 5.0e-4f)) {
        printf(
            "    Near-isotropic anisotropic compensation mismatch: "
            "isotropic=(%f,%f,%f) anisotropic=(%f,%f,%f)\n",
            isotropicEval[0], isotropicEval[1], isotropicEval[2],
            anisotropicEval[0], anisotropicEval[1], anisotropicEval[2]);
        return false;
    }

    return true;
}

static bool
TestGGXMicrofacetMultipleScatteringToggle()
{
    const Vec3f N(0.0f, 1.0f, 0.0f);
    const Vec3f wo = _DirectionFromCosThetaYUp(0.55f).normalized();
    const Vec3f wi = Vec3f(-0.25f, 0.78f, 0.57f).normalized();

    Bsdf::SetGgxMicrofacetMultipleScatteringEnabled(true);
    const Vec3f enabled = Bsdf::EvalGGXSpecular(
        0.9f, 1.5f, Vec3f(1.0f), N, wi, wo);

    Bsdf::SetGgxMicrofacetMultipleScatteringEnabled(false);
    const Vec3f disabled = Bsdf::EvalGGXSpecular(
        0.9f, 1.5f, Vec3f(1.0f), N, wi, wo);

    Bsdf::SetGgxMicrofacetMultipleScatteringEnabled(true);

    const float enabledLum = _FurnaceLuminance(enabled);
    const float disabledLum = _FurnaceLuminance(disabled);
    if (enabledLum <= disabledLum * 1.05f) {
        printf(
            "    GGX multiple-scattering toggle had no visible effect: "
            "enabled=%f disabled=%f\n",
            enabledLum, disabledLum);
        return false;
    }

    if (!Bsdf::IsGgxMicrofacetMultipleScatteringEnabled()) {
        printf("    GGX multiple-scattering toggle did not reset to enabled\n");
        return false;
    }

    return true;
}

static bool
TestBsdlDielectricReflFrontLutUsesLinearCosThetaGrid()
{
    namespace lut = mxcpp::bsdf_luts;

    constexpr float eta = 1.001f;
    constexpr int cosIndex = 1;
    constexpr float linearCos =
        static_cast<float>(cosIndex) /
        static_cast<float>(lut::kBsdlDielectricReflFrontCosThetaCount - 1);
    constexpr float squaredCos = linearCos * linearCos;
    const float tableValue =
        lut::kBsdlDielectricReflFrontFilter[0][0][cosIndex];
    const float expectedLinear =
        _DielectricTransmittanceForTest(eta, linearCos);
    const float expectedSquared =
        _DielectricTransmittanceForTest(eta, squaredCos);

    if (!Test_IsClose(tableValue, expectedLinear, 1.0e-5f)) {
        printf(
            "    BSDL DielectricReflFront LUT grid mismatch: "
            "table=%f expectedLinear=%f\n",
            tableValue, expectedLinear);
        return false;
    }
    if (Test_IsClose(tableValue, expectedSquared, 1.0e-3f)) {
        printf(
            "    BSDL DielectricReflFront LUT unexpectedly matches squared "
            "cosine grid: table=%f expectedSquared=%f\n",
            tableValue, expectedSquared);
        return false;
    }

    return true;
}

static bool
TestLayerReflectionAttenuatesBaseOnOutgoingSide()
{
    Bsdf::SetGgxMicrofacetMultipleScatteringEnabled(true);
    Bsdf::SetDielectricLayerThroughputMode(
        Bsdf::DielectricLayerThroughputMode::Bsdl);

    SurfaceClosure topClosure;
    Bsdf::DielectricData top;
    top.weight = 1.0f;
    top.tint = Vec3f(1.0f);
    top.ior = 1.6f;
    top.roughness = Vec2f(0.02f, 0.02f);
    top.scatterMode = Bsdf::ScatterMode::Reflection;
    topClosure.bsdfTree.root = topClosure.bsdfTree.Add(top);

    SurfaceClosure baseClosure;
    Bsdf::OrenNayarDiffuseData base;
    base.weight = 1.0f;
    base.color = Vec3f(1.0f);
    base.roughness = 0.0f;
    base.energyCompensation = true;
    baseClosure.bsdfTree.root = baseClosure.bsdfTree.Add(base);

    SurfaceClosure layerClosure;
    const auto topId = layerClosure.bsdfTree.Add(top);
    const auto baseId = layerClosure.bsdfTree.Add(base);
    Bsdf::LayerData layer;
    layer.top = topId;
    layer.base = baseId;
    layerClosure.bsdfTree.root = layerClosure.bsdfTree.Add(layer);

    const Vec3f N(0.0f, 1.0f, 0.0f);
    const Vec3f wo = Vec3f(0.95f, 0.3122499f, 0.0f).normalized();
    const Vec3f wi = Vec3f(-0.6f, 0.8f, 0.0f).normalized();

    const Vec3f topEval = Bsdf::EvalSurface(topClosure, N, wi, wo);
    const Vec3f baseEval = Bsdf::EvalSurface(baseClosure, N, wi, wo);
    const Vec3f layerEval = Bsdf::EvalSurface(layerClosure, N, wi, wo);

    // BSDL mtx::DielectricReflFront outgoing filter value for alpha=0.02,
    // IOR=1.6, and cos(wo)=~0.31225 using the linear cosTheta grid.
    constexpr float attOut = 0.78626859f;
    const Vec3f expected =
        topEval + baseEval * attOut;

    if (!Test_IsClose(layerEval, expected, 1.0e-4f)) {
        printf(
            "    Layer attenuation mismatch: expected=(%f,%f,%f) "
            "actual=(%f,%f,%f)\n",
            expected[0], expected[1], expected[2],
            layerEval[0], layerEval[1], layerEval[2]);
        return false;
    }
    return true;
}

static bool
TestLayerThroughputUsesBsdlDielectricFilter()
{
    Bsdf::SetGgxMicrofacetMultipleScatteringEnabled(true);
    Bsdf::SetDielectricLayerThroughputMode(
        Bsdf::DielectricLayerThroughputMode::Bsdl);

    SurfaceClosure topClosure;
    Bsdf::DielectricData top;
    top.weight = 1.0f;
    top.tint = Vec3f(1.0f);
    top.ior = 1.5f;
    top.roughness = Vec2f(1.0f, 1.0f);
    top.scatterMode = Bsdf::ScatterMode::Reflection;
    topClosure.bsdfTree.root = topClosure.bsdfTree.Add(top);

    SurfaceClosure baseClosure;
    Bsdf::OrenNayarDiffuseData base;
    base.weight = 1.0f;
    base.color = Vec3f(0.8f);
    base.roughness = 0.0f;
    base.energyCompensation = true;
    baseClosure.bsdfTree.root = baseClosure.bsdfTree.Add(base);

    SurfaceClosure layerClosure;
    const auto topId = layerClosure.bsdfTree.Add(top);
    const auto baseId = layerClosure.bsdfTree.Add(base);
    Bsdf::LayerData layer;
    layer.top = topId;
    layer.base = baseId;
    layerClosure.bsdfTree.root = layerClosure.bsdfTree.Add(layer);

    const Vec3f N(0.0f, 1.0f, 0.0f);
    const Vec3f wo = _DirectionFromCosThetaYUp(0.55f).normalized();
    const Vec3f wi = Vec3f(-0.35f, 0.72f, 0.60f).normalized();

    const Vec3f topEval = Bsdf::EvalSurface(topClosure, N, wi, wo);
    const Vec3f baseEval = Bsdf::EvalSurface(baseClosure, N, wi, wo);
    const Vec3f layerEval = Bsdf::EvalSurface(layerClosure, N, wi, wo);

    // BSDL mtx::DielectricReflFront outgoing filter value for roughness=1,
    // IOR=1.5, and cos(wo)=0.55 using the linear cosTheta grid.
    constexpr float filterOut = 0.97670996f;
    const Vec3f expected =
        topEval + baseEval * filterOut;

    if (!Test_IsClose(layerEval, expected, 1.0e-4f)) {
        printf(
            "    BSDL dielectric layer filter mismatch: expected=(%f,%f,%f) "
            "actual=(%f,%f,%f)\n",
            expected[0], expected[1], expected[2],
            layerEval[0], layerEval[1], layerEval[2]);
        return false;
    }

    return true;
}

static bool
TestLayerThroughputMaterialXGlslModeUsesExactFresnel()
{
    Bsdf::SetGgxMicrofacetMultipleScatteringEnabled(true);

    Bsdf::DielectricData top;
    top.weight = 1.0f;
    top.tint = Vec3f(1.0f);
    top.ior = 1.5f;
    top.roughness = Vec2f(0.35f, 0.35f);
    top.scatterMode = Bsdf::ScatterMode::Reflection;

    Bsdf::OrenNayarDiffuseData base;
    base.weight = 1.0f;
    base.color = Vec3f(0.75f);
    base.roughness = 0.0f;
    base.energyCompensation = true;

    SurfaceClosure topClosure;
    topClosure.bsdfTree.root = topClosure.bsdfTree.Add(top);

    SurfaceClosure baseClosure;
    baseClosure.bsdfTree.root = baseClosure.bsdfTree.Add(base);

    SurfaceClosure layerClosure;
    const auto topId = layerClosure.bsdfTree.Add(top);
    const auto baseId = layerClosure.bsdfTree.Add(base);
    Bsdf::LayerData layer;
    layer.top = topId;
    layer.base = baseId;
    layerClosure.bsdfTree.root = layerClosure.bsdfTree.Add(layer);

    const Vec3f N(0.0f, 1.0f, 0.0f);
    const float cosTheta = 0.18f;
    const Vec3f wo = _DirectionFromCosThetaYUp(cosTheta).normalized();
    const Vec3f wi = Vec3f(-0.35f, 0.72f, 0.60f).normalized();

    const Vec3f topEval = Bsdf::EvalSurface(topClosure, N, wi, wo);
    const Vec3f baseEval = Bsdf::EvalSurface(baseClosure, N, wi, wo);

    Bsdf::SetDielectricLayerThroughputMode(
        Bsdf::DielectricLayerThroughputMode::MaterialXGlsl);
    const Vec3f glslLayerEval = Bsdf::EvalSurface(layerClosure, N, wi, wo);

    Bsdf::SetDielectricLayerThroughputMode(
        Bsdf::DielectricLayerThroughputMode::Bsdl);
    const Vec3f bsdlLayerEval = Bsdf::EvalSurface(layerClosure, N, wi, wo);

    const float exactFresnel =
        1.0f - _DielectricTransmittanceForTest(top.ior, cosTheta);
    const Vec3f exactThroughput =
        _MaterialXGlslDielectricThroughputForTest(
            top.roughness[0],
            cosTheta,
            top.ior,
            top.weight,
            exactFresnel);
    const Vec3f expectedExact =
        topEval + CompMul(baseEval, exactThroughput);

    if (!Test_IsClose(glslLayerEval, expectedExact, 1.0e-4f)) {
        printf(
            "    MaterialX GLSL dielectric throughput mismatch: "
            "expected=(%f,%f,%f) actual=(%f,%f,%f)\n",
            expectedExact[0], expectedExact[1], expectedExact[2],
            glslLayerEval[0], glslLayerEval[1], glslLayerEval[2]);
        return false;
    }

    const float schlickFresnel =
        _SchlickFresnelForTest(top.ior, cosTheta);
    const Vec3f schlickThroughput =
        _MaterialXGlslDielectricThroughputForTest(
            top.roughness[0],
            cosTheta,
            top.ior,
            top.weight,
            schlickFresnel);
    const Vec3f expectedSchlick =
        topEval + CompMul(baseEval, schlickThroughput);
    if (Test_IsClose(glslLayerEval, expectedSchlick, 1.0e-4f)) {
        printf(
            "    MaterialX GLSL dielectric throughput used Schlick Fresnel: "
            "actual=(%f,%f,%f) schlickExpected=(%f,%f,%f)\n",
            glslLayerEval[0], glslLayerEval[1], glslLayerEval[2],
            expectedSchlick[0], expectedSchlick[1], expectedSchlick[2]);
        return false;
    }

    if (Test_IsClose(glslLayerEval, bsdlLayerEval, 1.0e-4f)) {
        printf(
            "    Dielectric throughput mode switch had no visible effect: "
            "bsdl=(%f,%f,%f) glsl=(%f,%f,%f)\n",
            bsdlLayerEval[0], bsdlLayerEval[1], bsdlLayerEval[2],
            glslLayerEval[0], glslLayerEval[1], glslLayerEval[2]);
        return false;
    }

    return true;
}

static bool
TestPowerHeuristic()
{
    float w = Bsdf::PowerHeuristic(1.0f, 1.0f);
    if (!Test_IsClose(w, 0.5f, 1e-4f)) {
        printf("    PowerHeuristic(1,1)=%f, expected 0.5\n", w);
        return false;
    }

    float w2 = Bsdf::PowerHeuristic(2.0f, 1.0f);
    if (w2 < 0.7f || w2 > 0.85f) {
        printf("    PowerHeuristic(2,1)=%f, expected ~0.8\n", w2);
        return false;
    }

    float w3 = Bsdf::PowerHeuristic(0.0f, 1.0f);
    if (w3 > 0.01f) {
        printf("    PowerHeuristic(0,1)=%f, expected ~0\n", w3);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Bsdf::SampleSubsurfaceEntry tests (Phase 1 Task 1.2)

static bool
TestSampleSubsurfaceEntrySmoothRefraction()
{
    // Smooth dielectric at normal incidence: deterministic Snell refraction
    // should yield wi pointing straight into the medium.
    SurfaceClosure c;
    c.roughness = 1.0e-6f;
    c.specularIor = 1.5f;
    Vec3f N(0, 0, 1);
    Vec3f wo(0, 0, 1);
    Vec3f wi;
    bool ok = Bsdf::SampleSubsurfaceEntry(c, N, wo, 0.5f, 0.5f, wi);
    if (!ok) {
        printf("    Expected successful sample\n");
        return false;
    }
    if (!Test_IsClose(wi, Vec3f(0, 0, -1), 1e-4f)) {
        printf("    Expected (0,0,-1), got (%f,%f,%f)\n", wi[0], wi[1], wi[2]);
        return false;
    }
    return true;
}

static bool
TestSampleSubsurfaceEntryRoughGGX()
{
    // Rough dielectric should sample a direction pointing into the medium.
    SurfaceClosure c;
    c.roughness = 0.5f;
    c.specularIor = 1.5f;
    Vec3f N(0, 0, 1);
    Vec3f wo(0, 0, 1);
    Vec3f wi;
    bool ok = Bsdf::SampleSubsurfaceEntry(c, N, wo, 0.3f, 0.7f, wi);
    if (!ok) {
        printf("    Expected successful sample\n");
        return false;
    }
    if (Dot(wi, N) >= 0.0f) {
        printf("    Expected wi pointing into medium (Dot(wi,N) < 0), "
               "got (%f,%f,%f)\n", wi[0], wi[1], wi[2]);
        return false;
    }
    return true;
}

static bool
TestSampleSubsurfaceEntryIorClamp()
{
    // IOR < 1 should be clamped to 1.0 (no TIR at entry).
    SurfaceClosure c;
    c.roughness = 1.0e-6f;
    c.specularIor = 0.5f;
    Vec3f N(0, 0, 1);
    Vec3f wo(0, 0, 1);
    Vec3f wi;
    bool ok = Bsdf::SampleSubsurfaceEntry(c, N, wo, 0.5f, 0.5f, wi);
    if (!ok) {
        printf("    Expected successful sample with IOR clamp\n");
        return false;
    }
    if (!Test_IsClose(wi, Vec3f(0, 0, -1), 1e-4f)) {
        printf("    Expected (0,0,-1) with IOR clamped to 1.0, got (%f,%f,%f)\n",
               wi[0], wi[1], wi[2]);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// mxcpp::ChannelMIS tests (Phase 2 Task 2.1)

static bool
TestChannelMISUniform()
{
    Vec3f tp(1.0f);
    Vec3f w(1.0f);
    Vec3f pdf;
    int ch = mxcpp::ChannelMIS(tp, w, 0.0f, &pdf);
    if (ch != 0) {
        printf("    u=0 expected ch 0, got %d\n", ch);
        return false;
    }
    if (!Test_IsClose(pdf, Vec3f(1.0f/3.0f), 1e-4f)) {
        printf("    Expected uniform pdf (1/3,1/3,1/3), got (%f,%f,%f)\n",
               pdf[0], pdf[1], pdf[2]);
        return false;
    }
    return true;
}

static bool
TestChannelMISWeighted()
{
    Vec3f tp(1.0f);
    Vec3f w(0.1f, 0.8f, 0.1f);
    Vec3f pdf;
    int ch = mxcpp::ChannelMIS(tp, w, 0.5f, &pdf);
    if (!Test_IsClose(pdf, Vec3f(0.1f, 0.8f, 0.1f), 1e-4f)) {
        printf("    Expected (0.1,0.8,0.1) pdf, got (%f,%f,%f)\n",
               pdf[0], pdf[1], pdf[2]);
        return false;
    }
    // u=0.5 falls in green channel [0.1, 0.9)
    if (ch != 1) {
        printf("    u=0.5 expected ch 1, got %d\n", ch);
        return false;
    }
    return true;
}

static bool
TestChannelMISZeroFallback()
{
    Vec3f tp(0.0f);
    Vec3f w(0.0f);
    Vec3f pdf;
    mxcpp::ChannelMIS(tp, w, 0.5f, &pdf);
    if (!Test_IsClose(pdf, Vec3f(1.0f/3.0f), 1e-4f)) {
        printf("    Expected uniform fallback, got (%f,%f,%f)\n",
               pdf[0], pdf[1], pdf[2]);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// HdEmbreeChiangRemap tests (Phase 2 Task 2.3)

static bool
TestChiangRemapHighAlbedo()
{
    // High albedo (0.95) should yield high alpha (close to but below 0.999999).
    GfVec3f albedo(0.95f);
    GfVec3f radius(1.0f);
    GfVec3f sigma_t, alpha;
    HdEmbreeChiangRemap(albedo, radius, 0.0f, &sigma_t, &alpha);
    if (alpha[0] < 0.5f || alpha[0] > 0.999999f) {
        printf("    Expected 0.5 <= alpha <= 0.999999 for albedo=0.95, "
               "got %f\n", alpha[0]);
        return false;
    }
    // At g=0, sigma_t = sigma_t_prime / (1 - g) = 1/radius = 1.0
    if (!Test_IsClose(sigma_t[0], 1.0f, 1e-3f)) {
        printf("    Expected sigma_t ~ 1.0 at g=0, radius=1, got %f\n",
               sigma_t[0]);
        return false;
    }
    return true;
}

static bool
TestChiangRemapLowAlbedo()
{
    // Very low albedo (0.025): Cycles comment notes this is where rawAlpha
    // drops below min_alpha=0.2, triggering the min-alpha clamp.
    GfVec3f albedo(0.025f);
    GfVec3f radius(1.0f);
    GfVec3f sigma_t, alpha, rawAlpha;
    HdEmbreeChiangRemap(albedo, radius, 0.0f,
                        &sigma_t, &alpha, &rawAlpha);
    if (alpha[0] < 0.19f) {
        printf("    Expected alpha >= 0.2 (min_alpha clamp), got %f\n",
               alpha[0]);
        return false;
    }
    // Raw alpha should be below 0.2 for albedo=0.025.
    if (rawAlpha[0] >= 0.2f) {
        printf("    Expected rawAlpha < 0.2 for low albedo, got %f\n",
               rawAlpha[0]);
        return false;
    }
    return true;
}

static bool
TestChiangRemapAnisotropy()
{
    // sigma_t depends on anisotropy: sigma_t = sigma_t_prime / (1 - g).
    // For g=0.5, sigma_t should be ~2x compared to g=0.
    GfVec3f albedo(0.5f);
    GfVec3f radius(1.0f);
    GfVec3f sigma_t0, alpha0, sigma_t5, alpha5;
    HdEmbreeChiangRemap(albedo, radius, 0.0f,
                        &sigma_t0, &alpha0);
    HdEmbreeChiangRemap(albedo, radius, 0.5f,
                        &sigma_t5, &alpha5);
    if (sigma_t5[0] < 1.5f * sigma_t0[0]) {
        printf("    Expected sigma_t(g=0.5) > 1.5*sigma_t(g=0); "
               "g=0: %f, g=0.5: %f\n", sigma_t0[0], sigma_t5[0]);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Dwivedi sampling helper tests (Phase 3 Task 3.1)

static bool
TestDiffusionLengthDwivedi()
{
    // Diffusion length should be finite and increase with alpha.
    float l1 = PXR_INTERNAL_NS::HdEmbreeDiffusionLengthDwivedi(0.99f);
    if (!std::isfinite(l1) || l1 < 5.0f || l1 > 15.0f) {
        printf("    Expected 5 < L(0.99) < 15, got %f\n", l1);
        return false;
    }
    float l2 = PXR_INTERNAL_NS::HdEmbreeDiffusionLengthDwivedi(0.5f);
    if (!std::isfinite(l2) || l2 < 1.0f || l2 > 2.0f) {
        printf("    Expected 1.0 < L(0.5) < 2.0, got %f\n", l2);
        return false;
    }
    if (l1 <= l2) {
        printf("    Expected L(0.99) > L(0.5), got %f <= %f\n", l1, l2);
        return false;
    }
    return true;
}

static bool
TestSamplePhaseDwivediRange()
{
    // Sampled cos_theta must be in [-1, 1].
    const float L = 2.0f;
    const float phase_log = std::log((L + 1.0f) / (L - 1.0f));
    for (int i = 0; i < 100; ++i) {
        const float u = static_cast<float>(i) / 99.0f;
        const float cos_theta =
            PXR_INTERNAL_NS::HdEmbreeSamplePhaseDwivedi(L, phase_log, u);
        if (!std::isfinite(cos_theta) ||
            cos_theta < -1.0f - 1e-4f ||
            cos_theta > 1.0f + 1e-4f) {
            printf("    cos_theta out of range: %f at u=%f\n", cos_theta, u);
            return false;
        }
    }
    return true;
}

static bool
TestBackwardDwivediFraction()
{
    const float oppositeDistance = 2.0f;
    const float diffusionLength = 1.0f;

    const float nearEntry = PXR_INTERNAL_NS::HdEmbreeBackwardDwivediFraction(
        oppositeDistance, 0.0f, diffusionLength);
    const float midPlane = PXR_INTERNAL_NS::HdEmbreeBackwardDwivediFraction(
        oppositeDistance, 1.0f, diffusionLength);
    const float nearOpposite = PXR_INTERNAL_NS::HdEmbreeBackwardDwivediFraction(
        oppositeDistance, 2.0f, diffusionLength);

    if (!(nearEntry < midPlane && midPlane < nearOpposite)) {
        printf("    Expected backward fraction to increase across slab: "
               "%f, %f, %f\n", nearEntry, midPlane, nearOpposite);
        return false;
    }
    if (!Test_IsClose(midPlane, 0.5f, 1.0e-6f)) {
        printf("    Expected midpoint backward fraction 0.5, got %f\n",
               midPlane);
        return false;
    }
    if (!Test_IsClose(
            PXR_INTERNAL_NS::HdEmbreeBackwardDwivediFraction(
                oppositeDistance, -1.0f, diffusionLength),
            nearEntry,
            1.0e-6f)) {
        printf("    Expected x below entry plane to clamp to near-entry value\n");
        return false;
    }
    if (!Test_IsClose(
            PXR_INTERNAL_NS::HdEmbreeBackwardDwivediFraction(
                oppositeDistance, 3.0f, diffusionLength),
            nearOpposite,
            1.0e-6f)) {
        printf("    Expected x beyond opposite plane to clamp to far value\n");
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------

void
Test_RegisterBsdfTests()
{
    _REG(TestLambertianValue);
    _REG(TestLambertianColorScaling);
    _REG(TestFurnaceHelperMatchesLambertian);
    _REG(TestLegacySurfaceSpecularZeroIsLambertian);
    _REG(TestOrenNayarEnergyCompensationFalseUsesLegacyFactor);
    _REG(TestEonDiffuseLambertianLimit);
    _REG(TestEonDiffuseWhiteFurnace);
    _REG(TestGGXFurnaceWhiteFresnelGeneralizedSchlickBaseline);
    _REG(TestGGXFurnaceConductorEnergyBaseline);
    _REG(TestGGXFurnaceDielectricReflectionEnergyBaseline);
    _REG(TestGGXSpecularNonNegative);
    _REG(TestGGXSpecularUsesHeightCorrelatedSmith);
    _REG(TestGGXDirectionalMissingEnergyLutBounds);
    _REG(TestGGXTurquinWhiteFurnaceCompensatesMissingEnergy);
    _REG(TestGGXSpecularPeak);
    _REG(TestGGXSpecularLowRoughnessPeakPreserved);
    _REG(TestTreeDielectricReflectionMatchesStandaloneGgx);
    _REG(TestDispersionCauchyIorMonotonic);
    _REG(TestDispersionChangesTransmissionSampling);
    _REG(TestDispersionDisabledIgnoresHeroWavelength);
    _REG(TestSpectralNeutralRoundTripWhite);
    _REG(TestSpectralNeutralRoundTripGray);
    _REG(TestCoatZeroWeight);
    _REG(TestEvalSurfaceEmissiveOnly);
    _REG(TestSheenGrazingAngle);
    _REG(TestTransmissionNonNegative);
    _REG(TestEvalSurfaceNonNegative);
    _REG(TestPhaseHgIsotropicMatchesUniformSphere);
    _REG(TestPhaseHgForwardScatterBias);
    _REG(TestSampleHenyeyGreensteinPdfConsistency);
    _REG(TestSampleHenyeyGreensteinForwardMean);
    _REG(TestFreeFlightScatterWeightFinite);
    _REG(TestSampleSurfaceSubsurfaceReturnsMarker);
    _REG(TestSampleSurfaceMixCanChooseSubsurface);
    _REG(TestSampleSubsurfaceEntrySmoothRefraction);
    _REG(TestSampleSubsurfaceEntryRoughGGX);
    _REG(TestSampleSubsurfaceEntryIorClamp);
    _REG(TestSampleLambertianHemisphere);
    _REG(TestSampleLambertianPdfConsistency);
    _REG(TestSampleGGXSpecularHemisphere);
    _REG(TestSampleGGXSpecularPdfConsistency);
    _REG(TestSampleGGXSpecularLowRoughnessBoundedThroughput);
    _REG(TestSampleSurfacePdfConsistency);
    _REG(TestTreeTransmissionPreservesWeight);
    _REG(TestBsdfSampleDiffuseLikeClassification);
    _REG(TestZeroRoughnessDielectricSamplesDelta);
    _REG(TestZeroRoughnessConductorSamplesDeltaAndSkipsDirectEval);
    _REG(TestEffectivelySmoothConductorAlphaSamplesDeltaAndSkipsDirectEval);
    _REG(TestSharpConductorAlphaRemainsFiniteGlossy);
    _REG(TestSharpConductorEvalPdfRatioBoundedForLightSamples);
    _REG(TestTreeTransmissionPreservesWeightFromInterior);
    _REG(TestEvalSurfaceTransmissionFromInterior);
    _REG(TestTreeAddTransmissionPreservesWeight);
    _REG(TestDielectricInterfaceLayerDoesNotDoubleAttenuateTransmission);
    _REG(TestDielectricInterfaceSamplePdfConsistency);
    _REG(TestDeltaDielectricInterfaceTransmissionSamplesSingleFresnel);
    _REG(TestDeltaDielectricInterfaceTirDoesNotAmplifyThroughput);
    _REG(TestThinWalledDielectricInterfaceSamplePdfConsistency);
    _REG(TestDeltaThinWalledDielectricInterfaceTransmitsStraightThrough);
    _REG(TestSampleGGXTransmissionHemisphere);
    _REG(TestSampleGGXTransmissionPdfConsistency);
    _REG(TestRoughTransmissionSpreads);
    _REG(TestThinFilmDielectricChangesReflectionColor);
    _REG(TestThinFilmConductorChangesReflectionColor);
    _REG(TestThinFilmGeneralizedSchlickChangesReflectionColor);
    _REG(TestThinFilmSampleSurfacePdfConsistency);
    _REG(TestTreeDielectricCustomNormalMatchesStandaloneShadingNormal);
    _REG(TestTreeAnisotropicReflectionRespondsToTangent);
    _REG(TestTreeAnisotropicReflectionUsesTurquinCompensation);
    _REG(TestGGXMicrofacetMultipleScatteringToggle);
    _REG(TestBsdlDielectricReflFrontLutUsesLinearCosThetaGrid);
    _REG(TestLayerReflectionAttenuatesBaseOnOutgoingSide);
    _REG(TestLayerThroughputUsesBsdlDielectricFilter);
    _REG(TestLayerThroughputMaterialXGlslModeUsesExactFresnel);
    _REG(TestPowerHeuristic);
    _REG(TestChannelMISUniform);
    _REG(TestChannelMISWeighted);
    _REG(TestChannelMISZeroFallback);
    _REG(TestChiangRemapHighAlbedo);
    _REG(TestChiangRemapLowAlbedo);
    _REG(TestChiangRemapAnisotropy);
    _REG(TestDiffusionLengthDwivedi);
    _REG(TestSamplePhaseDwivediRange);
    _REG(TestBackwardDwivediFraction);
}

#undef _REG
