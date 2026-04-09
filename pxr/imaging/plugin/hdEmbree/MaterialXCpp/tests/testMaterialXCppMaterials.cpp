//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "../materials/standardSurface.h"
#include "../materials/openPbr.h"
#include "../materials/disneyPrincipled.h"
#include "../materials/gltfPbr.h"
#include "../materials/usdPreviewSurface.h"

#include <cmath>
#include <cstdio>
#include <functional>
#include <variant>

using namespace mxcpp;

void Test_Register(const char* name, std::function<bool()> fn);
bool Test_IsClose(float a, float b, float eps = 1e-5f);
bool Test_IsClose(const Vec3f& a, const Vec3f& b, float eps = 1e-5f);

#define _REG(name) Test_Register("Materials." #name, &name)

template <typename T, typename Predicate>
static const T*
FindNodeIf(const mxcpp::Bsdf::ClosureTree& tree, Predicate predicate)
{
    for (const auto& node : tree.nodes) {
        if (const auto* data = std::get_if<T>(&node.data)) {
            if (predicate(*data)) {
                return data;
            }
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Standard Surface
// ---------------------------------------------------------------------------

static bool
TestStandardSurfaceDefaults()
{
    ParamMap params;
    SurfaceClosure c = EvalStandardSurface(params);

    // Default base=1.0, base_color=(0.8), so baseColor should be (0.8).
    if (!Test_IsClose(c.baseColor, Vec3f(0.8f), 1e-4f)) {
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
    if (!c.HasBsdfTree()) return false;

    return true;
}

static bool
TestStandardSurfaceMetallic()
{
    ParamMap params;
    params["metalness"] = Value(1.0f);
    SurfaceClosure c = EvalStandardSurface(params);
    return Test_IsClose(c.metallic, 1.0f);
}

static bool
TestStandardSurfaceCustomParams()
{
    ParamMap params;
    params["base"] = Value(0.5f);
    params["base_color"] = Value(Vec3f(1, 0, 0));
    params["specular_roughness"] = Value(0.8f);
    params["emission"] = Value(2.0f);
    params["emission_color"] = Value(Vec3f(0, 1, 0));

    SurfaceClosure c = EvalStandardSurface(params);

    if (!Test_IsClose(c.baseColor, Vec3f(0.5f, 0.0f, 0.0f), 1e-4f)) {
        printf("    baseColor: (%f,%f,%f)\n", c.baseColor[0], c.baseColor[1], c.baseColor[2]);
        return false;
    }
    if (!Test_IsClose(c.roughness, 0.8f)) return false;
    if (!Test_IsClose(c.emissiveColor, Vec3f(0, 2, 0), 1e-4f)) return false;

    return true;
}

static bool
TestStandardSurfaceThinFilmParametersReachBsdf()
{
    ParamMap params;
    params["thin_film_thickness"] = Value(350.0f);
    params["thin_film_IOR"] = Value(1.8f);

    const SurfaceClosure c = EvalStandardSurface(params);
    const auto* dielectric = FindNodeIf<Bsdf::DielectricData>(
        c.bsdfTree,
        [](const Bsdf::DielectricData& data) {
            return data.scatterMode == Bsdf::ScatterMode::Reflection;
        });

    if (!dielectric) {
        printf("    Failed to find Standard Surface reflection dielectric node\n");
        return false;
    }

    return Test_IsClose(dielectric->thinFilmWeight, 1.0f) &&
           Test_IsClose(dielectric->thinFilmThickness, 350.0f, 1e-4f) &&
           Test_IsClose(dielectric->thinFilmIor, 1.8f, 1e-4f);
}

// ---------------------------------------------------------------------------
// OpenPBR
// ---------------------------------------------------------------------------

static bool
TestOpenPbrDefaults()
{
    ParamMap params;
    SurfaceClosure c = EvalOpenPbr(params);

    if (!Test_IsClose(c.baseColor, Vec3f(0.8f), 1e-4f)) return false;
    if (!Test_IsClose(c.roughness, 0.3f)) return false;
    if (!Test_IsClose(c.metallic, 0.0f)) return false;
    if (!Test_IsClose(c.specularIor, 1.5f)) return false;
    if (!Test_IsClose(c.transmission, 0.0f)) return false;
    if (!Test_IsClose(c.coat, 0.0f)) return false;
    if (!Test_IsClose(c.sheen, 0.0f)) return false;
    if (!c.HasBsdfTree()) return false;

    return true;
}

static bool
TestOpenPbrTransmission()
{
    ParamMap params;
    params["transmission_weight"] = Value(0.7f);
    params["transmission_color"] = Value(Vec3f(0.8f, 0.9f, 1.0f));
    SurfaceClosure c = EvalOpenPbr(params);

    if (!Test_IsClose(c.transmission, 0.7f)) return false;
    if (!Test_IsClose(c.transmissionColor, Vec3f(0.8f, 0.9f, 1.0f), 1e-4f))
        return false;
    return true;
}

static bool
TestOpenPbrThinFilmParametersReachBsdf()
{
    ParamMap params;
    params["thin_film_weight"] = Value(0.25f);
    params["thin_film_thickness"] = Value(0.4f);
    params["thin_film_ior"] = Value(1.7f);

    const SurfaceClosure c = EvalOpenPbr(params);
    const auto* dielectric = FindNodeIf<Bsdf::DielectricData>(
        c.bsdfTree,
        [](const Bsdf::DielectricData& data) {
            return data.scatterMode == Bsdf::ScatterMode::Reflection;
        });

    if (!dielectric) {
        printf("    Failed to find OpenPBR reflection dielectric node\n");
        return false;
    }

    return Test_IsClose(dielectric->thinFilmWeight, 0.25f) &&
           Test_IsClose(dielectric->thinFilmThickness, 400.0f, 1e-4f) &&
           Test_IsClose(dielectric->thinFilmIor, 1.7f, 1e-4f);
}

// ---------------------------------------------------------------------------
// Disney Principled
// ---------------------------------------------------------------------------

static bool
TestDisneyPrincipledDefaults()
{
    ParamMap params;
    const SurfaceClosure c = EvalDisneyPrincipled(params);

    if (!Test_IsClose(c.baseColor, Vec3f(0.16f), 1e-4f)) return false;
    if (!Test_IsClose(c.roughness, 0.5f)) return false;
    if (!Test_IsClose(c.metallic, 0.0f)) return false;
    if (!Test_IsClose(c.transmission, 0.0f)) return false;
    if (!c.HasBsdfTree()) return false;
    return true;
}

// ---------------------------------------------------------------------------
// glTF PBR
// ---------------------------------------------------------------------------

static bool
TestGltfPbrDefaults()
{
    ParamMap params;
    const SurfaceClosure c = EvalGltfPbr(params);

    if (!Test_IsClose(c.baseColor, Vec3f(1.0f), 1e-4f)) return false;
    if (!Test_IsClose(c.roughness, 1.0f)) return false;
    if (!Test_IsClose(c.metallic, 1.0f)) return false;
    if (!Test_IsClose(c.opacity, 1.0f)) return false;
    if (!c.HasBsdfTree()) return false;
    return true;
}

static bool
TestGltfPbrAlphaMask()
{
    ParamMap params;
    params["alpha"] = Value(0.25f);
    params["alpha_mode"] = Value(1);
    params["alpha_cutoff"] = Value(0.5f);

    const SurfaceClosure c = EvalGltfPbr(params);
    return Test_IsClose(c.opacity, 0.0f) &&
           Test_IsClose(c.presence, 0.0f);
}

static bool
TestGltfPbrIridescenceParametersReachBsdf()
{
    ParamMap params;
    params["iridescence"] = Value(0.6f);
    params["iridescence_ior"] = Value(1.45f);
    params["iridescence_thickness"] = Value(220.0f);

    const SurfaceClosure c = EvalGltfPbr(params);
    const auto* reflective = FindNodeIf<Bsdf::GeneralizedSchlickData>(
        c.bsdfTree,
        [](const Bsdf::GeneralizedSchlickData& data) {
            return data.scatterMode == Bsdf::ScatterMode::Reflection &&
                   data.thinFilmWeight > 0.0f;
        });

    if (!reflective) {
        printf("    Failed to find glTF iridescent reflection node\n");
        return false;
    }

    return Test_IsClose(reflective->thinFilmWeight, 0.6f) &&
           Test_IsClose(reflective->thinFilmThickness, 220.0f, 1e-4f) &&
           Test_IsClose(reflective->thinFilmIor, 1.45f, 1e-4f);
}

// ---------------------------------------------------------------------------
// UsdPreviewSurface
// ---------------------------------------------------------------------------

static bool
TestUsdPreviewSurfaceDefaults()
{
    ParamMap params;
    SurfaceClosure c = EvalUsdPreviewSurface(params);

    if (!Test_IsClose(c.baseColor, Vec3f(0.18f), 1e-4f)) {
        printf("    baseColor: (%f,%f,%f)\n", c.baseColor[0], c.baseColor[1], c.baseColor[2]);
        return false;
    }
    if (!Test_IsClose(c.roughness, 0.5f)) return false;
    if (!Test_IsClose(c.metallic, 0.0f)) return false;
    if (!Test_IsClose(c.opacity, 1.0f)) return false;
    if (!Test_IsClose(c.coat, 0.0f)) return false;
    if (!Test_IsClose(c.transmission, 0.0f)) return false;
    if (!c.HasBsdfTree()) return false;
    return true;
}

static bool
TestUsdPreviewSurfaceMetallicWorkflow()
{
    ParamMap params;
    params["useSpecularWorkflow"] = Value(0);
    params["metallic"] = Value(1.0f);
    params["diffuseColor"] = Value(Vec3f(1, 0, 0));

    SurfaceClosure c = EvalUsdPreviewSurface(params);
    if (!Test_IsClose(c.metallic, 1.0f)) return false;
    if (!Test_IsClose(c.baseColor, Vec3f(1, 0, 0), 1e-4f)) return false;
    return true;
}

static bool
TestUsdPreviewSurfaceSpecularWorkflow()
{
    ParamMap params;
    params["useSpecularWorkflow"] = Value(1);
    params["specularColor"] = Value(Vec3f(0.5f, 0.5f, 0.5f));

    SurfaceClosure c = EvalUsdPreviewSurface(params);
    if (!Test_IsClose(c.metallic, 0.0f)) return false;
    if (!Test_IsClose(c.specularColor, Vec3f(0.5f), 1e-4f)) return false;
    return true;
}

static bool
TestUsdPreviewSurfaceOpacityThreshold()
{
    ParamMap params;
    params["opacity"] = Value(0.3f);
    params["opacityThreshold"] = Value(0.5f);

    SurfaceClosure c = EvalUsdPreviewSurface(params);
    // opacity(0.3) < threshold(0.5) → opacity should be cutout to 0.
    return Test_IsClose(c.opacity, 0.0f) &&
           Test_IsClose(c.presence, 0.0f) &&
           Test_IsClose(c.transmission, 0.0f);
}

static bool
TestUsdPreviewSurfaceTransparentModeKeepsLightingResponse()
{
    ParamMap params;
    params["opacity"] = Value(0.0f);
    params["opacityMode"] = Value(std::string("transparent"));

    const SurfaceClosure c = EvalUsdPreviewSurface(params);
    return Test_IsClose(c.opacity, 0.0f) &&
           Test_IsClose(c.presence, 1.0f) &&
           Test_IsClose(c.transmission, 1.0f);
}

static bool
TestUsdPreviewSurfacePresenceModeCutsLightingResponse()
{
    ParamMap params;
    params["opacity"] = Value(0.25f);
    params["opacityMode"] = Value(std::string("presence"));

    const SurfaceClosure c = EvalUsdPreviewSurface(params);
    return Test_IsClose(c.opacity, 0.25f) &&
           Test_IsClose(c.presence, 0.25f) &&
           Test_IsClose(c.transmission, 0.0f);
}

static bool
TestUsdPreviewSurfaceMaterialXOpacityModeInteger()
{
    ParamMap params;
    params["opacity"] = Value(0.0f);
    params["opacityMode"] = Value(1);

    const SurfaceClosure c = EvalUsdPreviewSurface(params);
    return Test_IsClose(c.opacity, 0.0f) &&
           Test_IsClose(c.presence, 0.0f) &&
           Test_IsClose(c.transmission, 0.0f);
}

// ---------------------------------------------------------------------------

void
Test_RegisterMaterialTests()
{
    _REG(TestStandardSurfaceDefaults);
    _REG(TestStandardSurfaceMetallic);
    _REG(TestStandardSurfaceCustomParams);
    _REG(TestStandardSurfaceThinFilmParametersReachBsdf);
    _REG(TestOpenPbrDefaults);
    _REG(TestOpenPbrTransmission);
    _REG(TestOpenPbrThinFilmParametersReachBsdf);
    _REG(TestDisneyPrincipledDefaults);
    _REG(TestGltfPbrDefaults);
    _REG(TestGltfPbrAlphaMask);
    _REG(TestGltfPbrIridescenceParametersReachBsdf);
    _REG(TestUsdPreviewSurfaceDefaults);
    _REG(TestUsdPreviewSurfaceMetallicWorkflow);
    _REG(TestUsdPreviewSurfaceSpecularWorkflow);
    _REG(TestUsdPreviewSurfaceOpacityThreshold);
    _REG(TestUsdPreviewSurfaceTransparentModeKeepsLightingResponse);
    _REG(TestUsdPreviewSurfacePresenceModeCutsLightingResponse);
    _REG(TestUsdPreviewSurfaceMaterialXOpacityModeInteger);
}

#undef _REG
