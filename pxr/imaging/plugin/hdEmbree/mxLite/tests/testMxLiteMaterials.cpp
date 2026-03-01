//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/pxr.h"
#include "pxr/imaging/plugin/hdEmbree/mxLite/materials/standardSurface.h"
#include "pxr/imaging/plugin/hdEmbree/mxLite/materials/openPbr.h"
#include "pxr/imaging/plugin/hdEmbree/mxLite/materials/usdPreviewSurface.h"

#include <cmath>
#include <cstdio>
#include <functional>

PXR_NAMESPACE_USING_DIRECTIVE

void MxLiteTest_Register(const char* name, std::function<bool()> fn);
bool MxLiteTest_IsClose(float a, float b, float eps = 1e-5f);
bool MxLiteTest_IsClose(const GfVec3f& a, const GfVec3f& b, float eps = 1e-5f);

#define _REG(name) MxLiteTest_Register("Materials." #name, &name)

// ---------------------------------------------------------------------------
// Standard Surface
// ---------------------------------------------------------------------------

static bool
TestStandardSurfaceDefaults()
{
    MxLiteParamMap params;
    MxLiteSurfaceClosure c = MxLiteEvalStandardSurface(params);

    // Default base=1.0, base_color=(0.8), so baseColor should be (0.8).
    if (!MxLiteTest_IsClose(c.baseColor, GfVec3f(0.8f), 1e-4f)) {
        printf("    baseColor: (%f,%f,%f)\n", c.baseColor[0], c.baseColor[1], c.baseColor[2]);
        return false;
    }
    if (!MxLiteTest_IsClose(c.roughness, 0.2f)) {
        printf("    roughness: %f (expected 0.2)\n", c.roughness);
        return false;
    }
    if (!MxLiteTest_IsClose(c.metallic, 0.0f)) return false;
    if (!MxLiteTest_IsClose(c.specularIor, 1.5f)) return false;
    if (!MxLiteTest_IsClose(c.coat, 0.0f)) return false;
    if (!MxLiteTest_IsClose(c.sheen, 0.0f)) return false;
    if (!MxLiteTest_IsClose(c.transmission, 0.0f)) return false;

    return true;
}

static bool
TestStandardSurfaceMetallic()
{
    MxLiteParamMap params;
    params[TfToken("metalness")] = VtValue(1.0f);
    MxLiteSurfaceClosure c = MxLiteEvalStandardSurface(params);
    return MxLiteTest_IsClose(c.metallic, 1.0f);
}

static bool
TestStandardSurfaceCustomParams()
{
    MxLiteParamMap params;
    params[TfToken("base")] = VtValue(0.5f);
    params[TfToken("base_color")] = VtValue(GfVec3f(1, 0, 0));
    params[TfToken("specular_roughness")] = VtValue(0.8f);
    params[TfToken("emission")] = VtValue(2.0f);
    params[TfToken("emission_color")] = VtValue(GfVec3f(0, 1, 0));

    MxLiteSurfaceClosure c = MxLiteEvalStandardSurface(params);

    if (!MxLiteTest_IsClose(c.baseColor, GfVec3f(0.5f, 0.0f, 0.0f), 1e-4f)) {
        printf("    baseColor: (%f,%f,%f)\n", c.baseColor[0], c.baseColor[1], c.baseColor[2]);
        return false;
    }
    if (!MxLiteTest_IsClose(c.roughness, 0.8f)) return false;
    if (!MxLiteTest_IsClose(c.emissiveColor, GfVec3f(0, 2, 0), 1e-4f)) return false;

    return true;
}

// ---------------------------------------------------------------------------
// OpenPBR
// ---------------------------------------------------------------------------

static bool
TestOpenPbrDefaults()
{
    MxLiteParamMap params;
    MxLiteSurfaceClosure c = MxLiteEvalOpenPbr(params);

    if (!MxLiteTest_IsClose(c.baseColor, GfVec3f(0.8f), 1e-4f)) return false;
    if (!MxLiteTest_IsClose(c.roughness, 0.3f)) return false;
    if (!MxLiteTest_IsClose(c.metallic, 0.0f)) return false;
    if (!MxLiteTest_IsClose(c.specularIor, 1.5f)) return false;
    if (!MxLiteTest_IsClose(c.transmission, 0.0f)) return false;
    if (!MxLiteTest_IsClose(c.coat, 0.0f)) return false;
    if (!MxLiteTest_IsClose(c.sheen, 0.0f)) return false;

    return true;
}

static bool
TestOpenPbrTransmission()
{
    MxLiteParamMap params;
    params[TfToken("transmission_weight")] = VtValue(0.7f);
    params[TfToken("transmission_color")] = VtValue(GfVec3f(0.8f, 0.9f, 1.0f));
    MxLiteSurfaceClosure c = MxLiteEvalOpenPbr(params);

    if (!MxLiteTest_IsClose(c.transmission, 0.7f)) return false;
    if (!MxLiteTest_IsClose(c.transmissionColor, GfVec3f(0.8f, 0.9f, 1.0f), 1e-4f))
        return false;
    return true;
}

// ---------------------------------------------------------------------------
// UsdPreviewSurface
// ---------------------------------------------------------------------------

static bool
TestUsdPreviewSurfaceDefaults()
{
    MxLiteParamMap params;
    MxLiteSurfaceClosure c = MxLiteEvalUsdPreviewSurface(params);

    if (!MxLiteTest_IsClose(c.baseColor, GfVec3f(0.18f), 1e-4f)) {
        printf("    baseColor: (%f,%f,%f)\n", c.baseColor[0], c.baseColor[1], c.baseColor[2]);
        return false;
    }
    if (!MxLiteTest_IsClose(c.roughness, 0.5f)) return false;
    if (!MxLiteTest_IsClose(c.metallic, 0.0f)) return false;
    if (!MxLiteTest_IsClose(c.opacity, 1.0f)) return false;
    if (!MxLiteTest_IsClose(c.coat, 0.0f)) return false;
    if (!MxLiteTest_IsClose(c.transmission, 0.0f)) return false;
    return true;
}

static bool
TestUsdPreviewSurfaceMetallicWorkflow()
{
    MxLiteParamMap params;
    params[TfToken("useSpecularWorkflow")] = VtValue(0);
    params[TfToken("metallic")] = VtValue(1.0f);
    params[TfToken("diffuseColor")] = VtValue(GfVec3f(1, 0, 0));

    MxLiteSurfaceClosure c = MxLiteEvalUsdPreviewSurface(params);
    if (!MxLiteTest_IsClose(c.metallic, 1.0f)) return false;
    if (!MxLiteTest_IsClose(c.baseColor, GfVec3f(1, 0, 0), 1e-4f)) return false;
    return true;
}

static bool
TestUsdPreviewSurfaceSpecularWorkflow()
{
    MxLiteParamMap params;
    params[TfToken("useSpecularWorkflow")] = VtValue(1);
    params[TfToken("specularColor")] = VtValue(GfVec3f(0.5f, 0.5f, 0.5f));

    MxLiteSurfaceClosure c = MxLiteEvalUsdPreviewSurface(params);
    if (!MxLiteTest_IsClose(c.metallic, 0.0f)) return false;
    if (!MxLiteTest_IsClose(c.specularColor, GfVec3f(0.5f), 1e-4f)) return false;
    return true;
}

static bool
TestUsdPreviewSurfaceOpacityThreshold()
{
    MxLiteParamMap params;
    params[TfToken("opacity")] = VtValue(0.3f);
    params[TfToken("opacityThreshold")] = VtValue(0.5f);

    MxLiteSurfaceClosure c = MxLiteEvalUsdPreviewSurface(params);
    // opacity(0.3) < threshold(0.5) → opacity should be cutout to 0.
    return MxLiteTest_IsClose(c.opacity, 0.0f);
}

// ---------------------------------------------------------------------------

void
MxLiteTest_RegisterMaterialTests()
{
    _REG(TestStandardSurfaceDefaults);
    _REG(TestStandardSurfaceMetallic);
    _REG(TestStandardSurfaceCustomParams);
    _REG(TestOpenPbrDefaults);
    _REG(TestOpenPbrTransmission);
    _REG(TestUsdPreviewSurfaceDefaults);
    _REG(TestUsdPreviewSurfaceMetallicWorkflow);
    _REG(TestUsdPreviewSurfaceSpecularWorkflow);
    _REG(TestUsdPreviewSurfaceOpacityThreshold);
}

#undef _REG
