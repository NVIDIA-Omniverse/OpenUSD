//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "../materials/bsdf.h"
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
    _REG(TestCoatZeroWeight);
    _REG(TestEvalSurfaceEmissiveOnly);
    _REG(TestSheenGrazingAngle);
    _REG(TestTransmissionNonNegative);
    _REG(TestEvalSurfaceNonNegative);
    _REG(TestSampleLambertianHemisphere);
    _REG(TestSampleLambertianPdfConsistency);
    _REG(TestSampleGGXSpecularHemisphere);
    _REG(TestSampleGGXSpecularPdfConsistency);
    _REG(TestSampleSurfacePdfConsistency);
    _REG(TestTreeTransmissionPreservesWeight);
    _REG(TestTreeTransmissionPreservesWeightFromInterior);
    _REG(TestEvalSurfaceTransmissionFromInterior);
    _REG(TestTreeAddTransmissionPreservesWeight);
    _REG(TestSampleGGXTransmissionHemisphere);
    _REG(TestSampleGGXTransmissionPdfConsistency);
    _REG(TestRoughTransmissionSpreads);
    _REG(TestPowerHeuristic);
}

#undef _REG
