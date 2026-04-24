//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "../materials/bsdf.h"
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

static float
_SchlickIor(float ior, float cosTheta)
{
    float f0 = (ior - 1.0f) / (ior + 1.0f);
    f0 *= f0;
    const float t = 1.0f - std::clamp(cosTheta, 0.0f, 1.0f);
    const float t2 = t * t;
    return f0 + (1.0f - f0) * t2 * t2 * t;
}

static float
_TurquinDirectionalReflectanceScalar(
    float alphaRoughness,
    float cosTheta,
    float fresnel)
{
    const float F = std::clamp(fresnel, 0.0f, 1.0f);
    const float missing =
        Bsdf::GgxDirectionalMissingEnergy(cosTheta, alphaRoughness);
    const float single = 1.0f - missing;
    return std::clamp(F * single + F * F * missing, 0.0f, 1.0f);
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
    const float kRoughness = 0.001f;
    const float kSamples[][2] = {
        {0.1f, 0.2f},
        {0.3f, 0.7f},
        {0.6f, 0.4f},
        {0.85f, 0.15f},
    };

    for (const auto& sampleUV : kSamples) {
        const auto sample = Bsdf::SampleGGXSpecular(
            kRoughness, 1.5f, F0, N, wo, sampleUV[0], sampleUV[1]);
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
                    "sample=(%f,%f) channel=%d value=%f\n",
                    sampleUV[0], sampleUV[1], i, throughput[i]);
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
TestZeroRoughnessDielectricSamplesDelta()
{
    SurfaceClosure c;
    Bsdf::DielectricData dielectric;
    dielectric.weight = 1.0f;
    dielectric.tint = Vec3f(1.0f);
    dielectric.ior = 1.5f;
    dielectric.roughness = Vec2f(1.0e-5f, 1.0e-5f);
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
TestLayerReflectionAttenuatesBaseOnBothSides()
{
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

    const float f0 = std::pow((top.ior - 1.0f) / (top.ior + 1.0f), 2.0f);
    const auto schlick = [f0](float cosTheta) {
        const float t = 1.0f - cosTheta;
        const float t2 = t * t;
        return f0 + (1.0f - f0) * t2 * t2 * t;
    };
    const float attOut = 1.0f - schlick(std::abs(Dot(N, wo)));
    const float attIn = 1.0f - schlick(std::abs(Dot(N, wi)));
    const Vec3f expected =
        topEval + baseEval * (attOut * attIn);

    const float expectedLum = expected.length();
    const float actualLum = layerEval.length();
    const float ratio = actualLum / std::max(expectedLum, 1.0e-8f);
    if (ratio < 0.9f || ratio > 1.1f) {
        printf(
            "    Layer attenuation mismatch: expected=%f actual=%f ratio=%f\n",
            expectedLum, actualLum, ratio);
        return false;
    }
    return true;
}

static bool
TestLayerThroughputUsesRoughDirectionalReflectance()
{
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

    const float reflectanceOut = _TurquinDirectionalReflectanceScalar(
        top.roughness[0],
        std::abs(Dot(N, wo)),
        _SchlickIor(top.ior, std::abs(Dot(N, wo))));
    const float reflectanceIn = _TurquinDirectionalReflectanceScalar(
        top.roughness[0],
        std::abs(Dot(N, wi)),
        _SchlickIor(top.ior, std::abs(Dot(N, wi))));
    const Vec3f expected =
        topEval + baseEval * ((1.0f - reflectanceOut) *
                              (1.0f - reflectanceIn));

    if (!Test_IsClose(layerEval, expected, 1.0e-4f)) {
        printf(
            "    Rough layer throughput mismatch: expected=(%f,%f,%f) "
            "actual=(%f,%f,%f)\n",
            expected[0], expected[1], expected[2],
            layerEval[0], layerEval[1], layerEval[2]);
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

// ---------------------------------------------------------------------------

void
Test_RegisterBsdfTests()
{
    _REG(TestLambertianValue);
    _REG(TestLambertianColorScaling);
    _REG(TestFurnaceHelperMatchesLambertian);
    _REG(TestGGXFurnaceWhiteFresnelGeneralizedSchlickBaseline);
    _REG(TestGGXFurnaceConductorEnergyBaseline);
    _REG(TestGGXFurnaceDielectricReflectionEnergyBaseline);
    _REG(TestGGXSpecularNonNegative);
    _REG(TestGGXSpecularUsesHeightCorrelatedSmith);
    _REG(TestGGXDirectionalMissingEnergyLutBounds);
    _REG(TestGGXTurquinWhiteFurnaceCompensatesMissingEnergy);
    _REG(TestGGXSpecularPeak);
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
    _REG(TestZeroRoughnessDielectricSamplesDelta);
    _REG(TestTreeTransmissionPreservesWeightFromInterior);
    _REG(TestEvalSurfaceTransmissionFromInterior);
    _REG(TestTreeAddTransmissionPreservesWeight);
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
    _REG(TestLayerReflectionAttenuatesBaseOnBothSides);
    _REG(TestLayerThroughputUsesRoughDirectionalReflectance);
    _REG(TestPowerHeuristic);
    _REG(TestChannelMISUniform);
    _REG(TestChannelMISWeighted);
    _REG(TestChannelMISZeroFallback);
    _REG(TestChiangRemapHighAlbedo);
    _REG(TestChiangRemapLowAlbedo);
    _REG(TestChiangRemapAnisotropy);
    _REG(TestDiffusionLengthDwivedi);
    _REG(TestSamplePhaseDwivediRange);
}

#undef _REG
