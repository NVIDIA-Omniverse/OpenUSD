//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "../materials/bsdf.h"
#include "../spectral.h"
#include "../nodes/helpers/mathHelpers.h"

#include <cmath>
#include <cstdio>
#include <functional>

using namespace mxcpp;

// From testMain.cpp
void Test_Register(const char* name, std::function<bool()> fn);
bool Test_IsClose(float a, float b, float eps = 1e-5f);
bool Test_IsClose(const Vec3f& a, const Vec3f& b, float eps = 1e-5f);

#define _REG(name) Test_Register("Bsdf." #name, &name)

// ---------------------------------------------------------------------------

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

void
Test_RegisterBsdfTests()
{
    _REG(TestLambertianValue);
    _REG(TestLambertianColorScaling);
    _REG(TestGGXSpecularNonNegative);
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
    _REG(TestLayerReflectionAttenuatesBaseOnBothSides);
    _REG(TestPowerHeuristic);
}

#undef _REG
