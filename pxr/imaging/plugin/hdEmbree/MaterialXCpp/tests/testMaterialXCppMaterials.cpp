//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/pxr.h"
#include "pxr/imaging/plugin/hdEmbree/MaterialXCpp/materials/standardSurface.h"
#include "pxr/imaging/plugin/hdEmbree/MaterialXCpp/materials/openPbr.h"
#include "pxr/imaging/plugin/hdEmbree/MaterialXCpp/materials/usdPreviewSurface.h"

#include <cmath>
#include <cstdio>
#include <functional>

PXR_NAMESPACE_USING_DIRECTIVE

void Test_Register(const char* name, std::function<bool()> fn);
bool Test_IsClose(float a, float b, float eps = 1e-5f);
bool Test_IsClose(const GfVec3f& a, const GfVec3f& b, float eps = 1e-5f);

#define _REG(name) Test_Register("Materials." #name, &name)

// ---------------------------------------------------------------------------
// Standard Surface
// ---------------------------------------------------------------------------

static bool
TestStandardSurfaceDefaults()
{
    ParamMap params;
    SurfaceClosure c = EvalStandardSurface(params);

    // Default base=1.0, base_color=(0.8), so baseColor should be (0.8).
    if (!Test_IsClose(c.baseColor, GfVec3f(0.8f), 1e-4f)) {
        printf("    baseColor: (%f,%f,%f)\n", c.baseColor[0], c.baseColor[1], c.baseColor[2]);
        return false;
    }
    if (!Test_IsClose(c.roughness, 0.2f)) {
        printf("    roughness: %f (expected 0.2)\n", c.roughness);
        return false;
    }
    if (!Test_IsClose(c.metallic, 0.0f)) return false;
    if (!Test_IsClose(c.specularIor, 1.5f)) return false;
    if (!Test_IsClose(c.coat, 0.0f)) return false;
    if (!Test_IsClose(c.sheen, 0.0f)) return false;
    if (!Test_IsClose(c.transmission, 0.0f)) return false;

    return true;
}

static bool
TestStandardSurfaceMetallic()
{
    ParamMap params;
    params[TfToken("metalness")] = VtValue(1.0f);
    SurfaceClosure c = EvalStandardSurface(params);
    return Test_IsClose(c.metallic, 1.0f);
}

static bool
TestStandardSurfaceCustomParams()
{
    ParamMap params;
    params[TfToken("base")] = VtValue(0.5f);
    params[TfToken("base_color")] = VtValue(GfVec3f(1, 0, 0));
    params[TfToken("specular_roughness")] = VtValue(0.8f);
    params[TfToken("emission")] = VtValue(2.0f);
    params[TfToken("emission_color")] = VtValue(GfVec3f(0, 1, 0));

    SurfaceClosure c = EvalStandardSurface(params);

    if (!Test_IsClose(c.baseColor, GfVec3f(0.5f, 0.0f, 0.0f), 1e-4f)) {
        printf("    baseColor: (%f,%f,%f)\n", c.baseColor[0], c.baseColor[1], c.baseColor[2]);
        return false;
    }
    if (!Test_IsClose(c.roughness, 0.8f)) return false;
    if (!Test_IsClose(c.emissiveColor, GfVec3f(0, 2, 0), 1e-4f)) return false;

    return true;
}

// ---------------------------------------------------------------------------
// OpenPBR
// ---------------------------------------------------------------------------

static bool
TestOpenPbrDefaults()
{
    ParamMap params;
    SurfaceClosure c = EvalOpenPbr(params);

    if (!Test_IsClose(c.baseColor, GfVec3f(0.8f), 1e-4f)) return false;
    if (!Test_IsClose(c.roughness, 0.3f)) return false;
    if (!Test_IsClose(c.metallic, 0.0f)) return false;
    if (!Test_IsClose(c.specularIor, 1.5f)) return false;
    if (!Test_IsClose(c.transmission, 0.0f)) return false;
    if (!Test_IsClose(c.coat, 0.0f)) return false;
    if (!Test_IsClose(c.sheen, 0.0f)) return false;

    return true;
}

static bool
TestOpenPbrTransmission()
{
    ParamMap params;
    params[TfToken("transmission_weight")] = VtValue(0.7f);
    params[TfToken("transmission_color")] = VtValue(GfVec3f(0.8f, 0.9f, 1.0f));
    SurfaceClosure c = EvalOpenPbr(params);

    if (!Test_IsClose(c.transmission, 0.7f)) return false;
    if (!Test_IsClose(c.transmissionColor, GfVec3f(0.8f, 0.9f, 1.0f), 1e-4f))
        return false;
    return true;
}

// ---------------------------------------------------------------------------
// UsdPreviewSurface
// ---------------------------------------------------------------------------

static bool
TestUsdPreviewSurfaceDefaults()
{
    ParamMap params;
    SurfaceClosure c = EvalUsdPreviewSurface(params);

    if (!Test_IsClose(c.baseColor, GfVec3f(0.18f), 1e-4f)) {
        printf("    baseColor: (%f,%f,%f)\n", c.baseColor[0], c.baseColor[1], c.baseColor[2]);
        return false;
    }
    if (!Test_IsClose(c.roughness, 0.5f)) return false;
    if (!Test_IsClose(c.metallic, 0.0f)) return false;
    if (!Test_IsClose(c.opacity, 1.0f)) return false;
    if (!Test_IsClose(c.coat, 0.0f)) return false;
    if (!Test_IsClose(c.transmission, 0.0f)) return false;
    return true;
}

static bool
TestUsdPreviewSurfaceMetallicWorkflow()
{
    ParamMap params;
    params[TfToken("useSpecularWorkflow")] = VtValue(0);
    params[TfToken("metallic")] = VtValue(1.0f);
    params[TfToken("diffuseColor")] = VtValue(GfVec3f(1, 0, 0));

    SurfaceClosure c = EvalUsdPreviewSurface(params);
    if (!Test_IsClose(c.metallic, 1.0f)) return false;
    if (!Test_IsClose(c.baseColor, GfVec3f(1, 0, 0), 1e-4f)) return false;
    return true;
}

static bool
TestUsdPreviewSurfaceSpecularWorkflow()
{
    ParamMap params;
    params[TfToken("useSpecularWorkflow")] = VtValue(1);
    params[TfToken("specularColor")] = VtValue(GfVec3f(0.5f, 0.5f, 0.5f));

    SurfaceClosure c = EvalUsdPreviewSurface(params);
    if (!Test_IsClose(c.metallic, 0.0f)) return false;
    if (!Test_IsClose(c.specularColor, GfVec3f(0.5f), 1e-4f)) return false;
    return true;
}

static bool
TestUsdPreviewSurfaceOpacityThreshold()
{
    ParamMap params;
    params[TfToken("opacity")] = VtValue(0.3f);
    params[TfToken("opacityThreshold")] = VtValue(0.5f);

    SurfaceClosure c = EvalUsdPreviewSurface(params);
    // opacity(0.3) < threshold(0.5) → opacity should be cutout to 0.
    return Test_IsClose(c.opacity, 0.0f);
}

// ---------------------------------------------------------------------------

void
Test_RegisterMaterialTests()
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
