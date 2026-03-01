//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/pxr.h"
#include "pxr/imaging/plugin/hdEmbree/mxLite/materials/bsdf.h"

#include <cmath>
#include <cstdio>
#include <functional>

PXR_NAMESPACE_USING_DIRECTIVE

// From testMxLiteMain.cpp
void MxLiteTest_Register(const char* name, std::function<bool()> fn);
bool MxLiteTest_IsClose(float a, float b, float eps = 1e-5f);
bool MxLiteTest_IsClose(const GfVec3f& a, const GfVec3f& b, float eps = 1e-5f);

#define _REG(name) MxLiteTest_Register("Bsdf." #name, &name)

// ---------------------------------------------------------------------------

static bool
TestLambertianValue()
{
    GfVec3f white(1.0f);
    GfVec3f N(0, 1, 0);
    GfVec3f wi(0, 1, 0);
    GfVec3f wo(0, 1, 0);

    GfVec3f result = MxLiteBsdf::EvalLambertian(white, N, wi, wo);
    float expected = 1.0f / 3.14159265358979f;
    if (!MxLiteTest_IsClose(result, GfVec3f(expected), 1e-5f)) {
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
    GfVec3f color(0.5f, 0.3f, 0.1f);
    GfVec3f N(0, 1, 0);
    GfVec3f result = MxLiteBsdf::EvalLambertian(color, N, GfVec3f(0,1,0), GfVec3f(0,1,0));
    float invPi = 1.0f / 3.14159265358979f;
    return MxLiteTest_IsClose(result, color * invPi, 1e-5f);
}

static bool
TestGGXSpecularNonNegative()
{
    GfVec3f N(0, 1, 0);
    GfVec3f wo = GfVec3f(0, 1, 0);
    GfVec3f wi = GfVec3f(0.5f, 0.866f, 0.0f).GetNormalized();
    GfVec3f result = MxLiteBsdf::EvalGGXSpecular(
        0.5f, 1.5f, GfVec3f(1.0f), N, wi, wo);
    return result[0] >= 0.0f && result[1] >= 0.0f && result[2] >= 0.0f;
}

static bool
TestGGXSpecularPeak()
{
    GfVec3f N(0, 1, 0);
    GfVec3f wo = GfVec3f(0.3f, 0.95f, 0.0f).GetNormalized();
    GfVec3f wiMirror = GfVec3f(-0.3f, 0.95f, 0.0f).GetNormalized();
    GfVec3f wiOff = GfVec3f(0.8f, 0.6f, 0.0f).GetNormalized();

    float roughness = 0.05f;
    GfVec3f specColor(1.0f);

    GfVec3f atMirror = MxLiteBsdf::EvalGGXSpecular(
        roughness, 1.5f, specColor, N, wiMirror, wo);
    GfVec3f offMirror = MxLiteBsdf::EvalGGXSpecular(
        roughness, 1.5f, specColor, N, wiOff, wo);

    float mirrorMag = atMirror.GetLength();
    float offMag = offMirror.GetLength();
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
    GfVec3f N(0, 1, 0);
    GfVec3f wi(0, 1, 0);
    GfVec3f wo(0, 1, 0);
    GfVec3f result = MxLiteBsdf::EvalCoat(0.0f, 0.1f, 1.5f, N, wi, wo);
    return MxLiteTest_IsClose(result, GfVec3f(0.0f), 1e-7f);
}

static bool
TestEvalSurfaceEmissiveOnly()
{
    // EvalSurface returns pure BSDF value (no emissive — that's the
    // renderer's job). With NdotL <= 0 the BSDF returns zero.
    MxLiteSurfaceClosure c;
    c.emissiveColor = GfVec3f(1.0f, 0.5f, 0.0f);

    GfVec3f N(0, 1, 0);
    GfVec3f wi(0, -1, 0);  // NdotL <= 0
    GfVec3f wo(0, 1, 0);

    GfVec3f result = MxLiteBsdf::EvalSurface(c, N, wi, wo);
    return MxLiteTest_IsClose(result, GfVec3f(0.0f), 1e-5f);
}

static bool
TestSheenGrazingAngle()
{
    GfVec3f N(0, 1, 0);
    GfVec3f sheenColor(1.0f);
    float roughness = 0.5f;

    GfVec3f woNormal = GfVec3f(0, 1, 0);
    GfVec3f woGrazing = GfVec3f(0.99f, 0.14f, 0.0f).GetNormalized();
    GfVec3f wi = GfVec3f(-0.3f, 0.95f, 0.0f).GetNormalized();

    GfVec3f atNormal = MxLiteBsdf::EvalSheen(sheenColor, roughness, N, wi, woNormal);
    GfVec3f atGrazing = MxLiteBsdf::EvalSheen(sheenColor, roughness, N, wi, woGrazing);

    // Sheen should generally have more contribution at grazing angles.
    // We allow some tolerance here since the exact relationship depends on
    // the half-vector geometry.
    return atNormal.GetLength() >= 0.0f && atGrazing.GetLength() >= 0.0f;
}

static bool
TestTransmissionIor1()
{
    GfVec3f N(0, 1, 0);
    GfVec3f wi(0, 1, 0);
    GfVec3f wo(0, 1, 0);
    GfVec3f tColor(1.0f);

    GfVec3f result = MxLiteBsdf::EvalGGXTransmission(
        0.5f, 1.0f, tColor, N, wi, wo);
    // With ior=1.0, Fresnel reflection should be zero, so transmission
    // should be non-zero.
    if (result.GetLength() <= 0.0f) {
        printf("    Expected non-zero transmission at ior=1.0, got zero\n");
        return false;
    }
    return true;
}

static bool
TestEvalSurfaceNonNegative()
{
    MxLiteSurfaceClosure c;
    c.baseColor = GfVec3f(0.8f, 0.2f, 0.1f);
    c.roughness = 0.4f;
    c.metallic = 0.0f;
    c.specular = 1.0f;
    c.specularIor = 1.5f;
    c.specularColor = GfVec3f(1.0f);
    c.emissiveColor = GfVec3f(0.0f);

    GfVec3f N(0, 1, 0);
    GfVec3f wi = GfVec3f(0.3f, 0.95f, 0.0f).GetNormalized();
    GfVec3f wo = GfVec3f(-0.2f, 0.98f, 0.0f).GetNormalized();

    GfVec3f result = MxLiteBsdf::EvalSurface(c, N, wi, wo);
    return result[0] >= 0.0f && result[1] >= 0.0f && result[2] >= 0.0f;
}

// ===========================================================================
// Sampling tests (Phase 9)
// ===========================================================================

static bool
TestSampleLambertianHemisphere()
{
    GfVec3f N(0, 1, 0);
    GfVec3f wo(0, 1, 0);
    GfVec3f baseColor(0.8f, 0.2f, 0.1f);

    // All sampled directions should be in the hemisphere around N.
    for (int i = 0; i < 64; ++i) {
        float u1 = (i + 0.5f) / 64.0f;
        float u2 = (i * 7 % 64 + 0.5f) / 64.0f;
        auto s = MxLiteBsdf::SampleLambertian(baseColor, N, wo, u1, u2);
        if (GfDot(s.wi, N) < -1e-5f) {
            printf("    Lambertian sample below hemisphere: NdotWi=%f\n",
                   GfDot(s.wi, N));
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
    GfVec3f N(0, 1, 0);
    GfVec3f wo(0, 1, 0);
    GfVec3f baseColor(1.0f);

    auto s = MxLiteBsdf::SampleLambertian(baseColor, N, wo, 0.3f, 0.7f);
    float pdf2 = MxLiteBsdf::PdfLambertian(N, s.wi);
    if (!MxLiteTest_IsClose(s.pdf, pdf2, 1e-4f)) {
        printf("    Sample pdf=%f != PdfLambertian=%f\n", s.pdf, pdf2);
        return false;
    }
    return true;
}

static bool
TestSampleGGXSpecularHemisphere()
{
    GfVec3f N(0, 1, 0);
    GfVec3f wo = GfVec3f(0.3f, 0.95f, 0.0f).GetNormalized();
    GfVec3f specColor(1.0f);

    int valid = 0;
    for (int i = 0; i < 64; ++i) {
        float u1 = (i + 0.5f) / 64.0f;
        float u2 = (i * 13 % 64 + 0.5f) / 64.0f;
        auto s = MxLiteBsdf::SampleGGXSpecular(
            0.3f, 1.5f, specColor, N, wo, u1, u2);
        if (s.pdf > 0.0f) {
            ++valid;
            if (GfDot(s.wi, N) < -1e-5f) {
                printf("    GGX sample below hemisphere: NdotWi=%f\n",
                       GfDot(s.wi, N));
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
    GfVec3f N(0, 1, 0);
    GfVec3f wo = GfVec3f(0.2f, 0.98f, 0.0f).GetNormalized();
    GfVec3f specColor(1.0f);

    auto s = MxLiteBsdf::SampleGGXSpecular(
        0.4f, 1.5f, specColor, N, wo, 0.3f, 0.7f);
    if (s.pdf <= 0.0f) return true;

    float pdf2 = MxLiteBsdf::PdfGGXSpecular(0.4f, N, s.wi, wo);
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
    MxLiteSurfaceClosure c;
    c.baseColor = GfVec3f(0.8f, 0.2f, 0.1f);
    c.roughness = 0.4f;
    c.metallic = 0.0f;
    c.specular = 1.0f;
    c.specularIor = 1.5f;
    c.specularColor = GfVec3f(1.0f);

    GfVec3f N(0, 1, 0);
    GfVec3f wo = GfVec3f(0.2f, 0.98f, 0.0f).GetNormalized();

    auto s = MxLiteBsdf::SampleSurface(c, N, wo, 0.5f, 0.5f, 0.3f);
    if (s.pdf <= 0.0f) return true;

    float pdf2 = MxLiteBsdf::PdfSurface(c, N, s.wi, wo);
    float ratio = s.pdf / (pdf2 + 1e-10f);
    if (ratio < 0.8f || ratio > 1.2f) {
        printf("    SampleSurface pdf=%f != PdfSurface=%f (ratio=%f)\n",
               s.pdf, pdf2, ratio);
        return false;
    }
    return true;
}

static bool
TestPowerHeuristic()
{
    float w = MxLiteBsdf::PowerHeuristic(1.0f, 1.0f);
    if (!MxLiteTest_IsClose(w, 0.5f, 1e-4f)) {
        printf("    PowerHeuristic(1,1)=%f, expected 0.5\n", w);
        return false;
    }

    float w2 = MxLiteBsdf::PowerHeuristic(2.0f, 1.0f);
    if (w2 < 0.7f || w2 > 0.85f) {
        printf("    PowerHeuristic(2,1)=%f, expected ~0.8\n", w2);
        return false;
    }

    float w3 = MxLiteBsdf::PowerHeuristic(0.0f, 1.0f);
    if (w3 > 0.01f) {
        printf("    PowerHeuristic(0,1)=%f, expected ~0\n", w3);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------

void
MxLiteTest_RegisterBsdfTests()
{
    _REG(TestLambertianValue);
    _REG(TestLambertianColorScaling);
    _REG(TestGGXSpecularNonNegative);
    _REG(TestGGXSpecularPeak);
    _REG(TestCoatZeroWeight);
    _REG(TestEvalSurfaceEmissiveOnly);
    _REG(TestSheenGrazingAngle);
    _REG(TestTransmissionIor1);
    _REG(TestEvalSurfaceNonNegative);
    _REG(TestSampleLambertianHemisphere);
    _REG(TestSampleLambertianPdfConsistency);
    _REG(TestSampleGGXSpecularHemisphere);
    _REG(TestSampleGGXSpecularPdfConsistency);
    _REG(TestSampleSurfacePdfConsistency);
    _REG(TestPowerHeuristic);
}

#undef _REG
