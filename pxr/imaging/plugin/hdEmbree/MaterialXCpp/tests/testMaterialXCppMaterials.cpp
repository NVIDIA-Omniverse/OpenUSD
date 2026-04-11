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
#include "../materials/bsdf.h"

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

static bool
TestStandardSurfaceLayersSpecularOverTransmissionMix()
{
    ParamMap params;
    params["transmission"] = Value(0.6f);
    params["coat"] = Value(0.0f);
    params["metalness"] = Value(0.0f);

    const SurfaceClosure c = EvalStandardSurface(params);
    const auto* root = c.bsdfTree.Get(c.bsdfTree.root);
    const auto* layer = root ? std::get_if<Bsdf::LayerData>(&root->data) : nullptr;
    if (!layer) {
        printf("    Expected Standard Surface root layer for specular stack\n");
        return false;
    }

    const auto* base = c.bsdfTree.Get(layer->base);
    const auto* mix = base ? std::get_if<Bsdf::MixData>(&base->data) : nullptr;
    if (!mix) {
        printf("    Expected transmission mix under Standard Surface specular layer\n");
        return false;
    }

    const auto* transmission = c.bsdfTree.Get(mix->fg);
    const auto* substrate = c.bsdfTree.Get(mix->bg);
    if (!transmission || !substrate) {
        printf("    Missing Standard Surface transmission mix children\n");
        return false;
    }

    return std::holds_alternative<Bsdf::DielectricData>(transmission->data) &&
           std::holds_alternative<Bsdf::OrenNayarDiffuseData>(substrate->data);
}

static bool
TestStandardSurfaceThinWalledUsesUnitIorTransmission()
{
    ParamMap params;
    params["transmission"] = Value(1.0f);
    params["thin_walled"] = Value(true);
    params["specular_IOR"] = Value(1.0f);
    params["coat"] = Value(0.0f);
    params["metalness"] = Value(0.0f);

    const SurfaceClosure c = EvalStandardSurface(params);
    const auto* transmission = FindNodeIf<Bsdf::DielectricData>(
        c.bsdfTree,
        [](const Bsdf::DielectricData& data) {
            return data.scatterMode == Bsdf::ScatterMode::Transmission;
        });

    if (!transmission) {
        printf("    Expected Standard Surface thin_walled to build dielectric transmission\n");
        return false;
    }
    if (!Test_IsClose(transmission->ior, 1.0f, 1e-4f)) {
        printf("    Expected Standard Surface thin_walled transmission IOR to be 1.0\n");
        return false;
    }
    const auto* reflection = FindNodeIf<Bsdf::DielectricData>(
        c.bsdfTree,
        [](const Bsdf::DielectricData& data) {
            return data.scatterMode == Bsdf::ScatterMode::Reflection;
        });
    if (!reflection || !Test_IsClose(reflection->ior, 1.0f, 1e-4f)) {
        printf("    Expected Standard Surface thin_walled reflection IOR to be 1.0\n");
        return false;
    }

    ParamMap openPbrParams;
    openPbrParams["base_weight"] = Value(0.0f);
    openPbrParams["base_color"] = Value(Vec3f(1.0f));
    openPbrParams["base_metalness"] = Value(0.0f);
    openPbrParams["specular_weight"] = Value(1.0f);
    openPbrParams["specular_color"] = Value(Vec3f(1.0f));
    openPbrParams["specular_roughness"] = Value(0.0f);
    openPbrParams["specular_ior"] = Value(1.0f);
    openPbrParams["transmission_weight"] = Value(1.0f);
    openPbrParams["transmission_color"] = Value(Vec3f(1.0f));
    openPbrParams["geometry_thin_walled"] = Value(true);

    const SurfaceClosure openPbrClosure = EvalOpenPbr(openPbrParams);
    const Vec3f N(0.0f, 1.0f, 0.0f);
    const Vec3f wo = Vec3f(0.0f, 1.0f, 0.0f);
    const Vec3f wi = Vec3f(0.0f, -1.0f, 0.0f);
    const Vec3f standardEval = Bsdf::EvalSurface(c, N, wi, wo);
    const Vec3f openPbrEval = Bsdf::EvalSurface(openPbrClosure, N, wi, wo);
    if (!Test_IsClose(standardEval, openPbrEval, 1e-4f)) {
        printf(
            "    standard=(%f,%f,%f) openpbr=(%f,%f,%f)\n",
            standardEval[0], standardEval[1], standardEval[2],
            openPbrEval[0], openPbrEval[1], openPbrEval[2]);
        return false;
    }

    return true;
}

static bool
TestOpenPbrThinWalledUsesUnitIorTransmission()
{
    ParamMap params;
    params["base_weight"] = Value(0.0f);
    params["base_color"] = Value(Vec3f(1.0f));
    params["base_metalness"] = Value(0.0f);
    params["specular_weight"] = Value(1.0f);
    params["specular_color"] = Value(Vec3f(1.0f));
    params["specular_roughness"] = Value(0.0f);
    params["specular_ior"] = Value(1.0f);
    params["transmission_weight"] = Value(1.0f);
    params["transmission_color"] = Value(Vec3f(1.0f));
    params["geometry_thin_walled"] = Value(true);

    const SurfaceClosure c = EvalOpenPbr(params);
    const auto* transmission = FindNodeIf<Bsdf::DielectricData>(
        c.bsdfTree,
        [](const Bsdf::DielectricData& data) {
            return data.scatterMode == Bsdf::ScatterMode::Transmission;
        });

    if (!transmission) {
        printf("    Expected OpenPBR thin_walled to build dielectric transmission\n");
        return false;
    }

    return Test_IsClose(transmission->ior, 1.0f, 1e-4f);
}

static bool
TestStandardSurfaceSpecularRotationUsesCanonicalName()
{
    ParamMap params;
    params["specular_anisotropy"] = Value(0.7f);
    params["specular_rotation"] = Value(0.25f);
    params["normal"] = Value(Vec3f(0.0f, 0.0f, 1.0f));
    params["tangent"] = Value(Vec3f(1.0f, 0.0f, 0.0f));
    params["coat"] = Value(0.0f);

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

    return Test_IsClose(dielectric->tangent, Vec3f(0.0f, 1.0f, 0.0f), 1e-4f);
}

static bool
TestStandardSurfaceMetalThinFilmChangesReflectionColor()
{
    ParamMap standardParams;
    standardParams["base"] = Value(1.0f);
    standardParams["base_color"] = Value(Vec3f(0.8f, 0.8f, 0.8f));
    standardParams["metalness"] = Value(1.0f);
    standardParams["specular"] = Value(1.0f);
    standardParams["specular_color"] = Value(Vec3f(1.0f));
    standardParams["specular_roughness"] = Value(0.3f);
    standardParams["specular_IOR"] = Value(1.5f);
    standardParams["thin_film_thickness"] = Value(250.0f);
    standardParams["thin_film_IOR"] = Value(1.6f);
    standardParams["coat"] = Value(0.0f);

    ParamMap openPbrParams;
    openPbrParams["base_weight"] = Value(1.0f);
    openPbrParams["base_color"] = Value(Vec3f(0.8f, 0.8f, 0.8f));
    openPbrParams["base_metalness"] = Value(1.0f);
    openPbrParams["specular_weight"] = Value(1.0f);
    openPbrParams["specular_color"] = Value(Vec3f(1.0f));
    openPbrParams["specular_roughness"] = Value(0.3f);
    openPbrParams["specular_ior"] = Value(1.5f);
    openPbrParams["thin_film_weight"] = Value(1.0f);
    openPbrParams["thin_film_thickness"] = Value(0.25f);
    openPbrParams["thin_film_ior"] = Value(1.6f);

    const SurfaceClosure standardClosure = EvalStandardSurface(standardParams);
    const SurfaceClosure openPbrClosure = EvalOpenPbr(openPbrParams);

    const Vec3f N(0.0f, 1.0f, 0.0f);
    const Vec3f wo = Vec3f(0.25f, 0.96f, 0.1f).normalized();
    const Vec3f wi = Vec3f(-0.1f, 0.99f, 0.05f).normalized();

    const Vec3f standardEval = Bsdf::EvalSurface(standardClosure, N, wi, wo);
    const Vec3f openPbrEval = Bsdf::EvalSurface(openPbrClosure, N, wi, wo);

    const bool chromatic =
        std::abs(standardEval[0] - standardEval[1]) > 1e-5f;
    const bool matchesOpenPbr =
        Test_IsClose(standardEval, openPbrEval, 5e-3f);
    if (!chromatic || !matchesOpenPbr) {
        printf(
            "    standard=(%f,%f,%f) openpbr=(%f,%f,%f)\n",
            standardEval[0], standardEval[1], standardEval[2],
            openPbrEval[0], openPbrEval[1], openPbrEval[2]);
    }

    return chromatic && matchesOpenPbr;
}

static bool
TestStandardSurfaceThinFilmUsesNanometerUnits()
{
    ParamMap nanometerParams;
    nanometerParams["base"] = Value(0.0f);
    nanometerParams["base_color"] = Value(Vec3f(1.0f));
    nanometerParams["metalness"] = Value(0.0f);
    nanometerParams["specular"] = Value(1.0f);
    nanometerParams["specular_color"] = Value(Vec3f(1.0f));
    nanometerParams["specular_roughness"] = Value(0.02f);
    nanometerParams["specular_IOR"] = Value(2.5f);
    nanometerParams["thin_film_thickness"] = Value(550.0f);
    nanometerParams["thin_film_IOR"] = Value(1.5f);

    ParamMap subNanometerParams;
    subNanometerParams["base"] = Value(0.0f);
    subNanometerParams["base_color"] = Value(Vec3f(1.0f));
    subNanometerParams["metalness"] = Value(0.0f);
    subNanometerParams["specular"] = Value(1.0f);
    subNanometerParams["specular_color"] = Value(Vec3f(1.0f));
    subNanometerParams["specular_roughness"] = Value(0.02f);
    subNanometerParams["specular_IOR"] = Value(2.5f);
    subNanometerParams["thin_film_thickness"] = Value(0.55f);
    subNanometerParams["thin_film_IOR"] = Value(1.5f);

    const SurfaceClosure nanometerClosure = EvalStandardSurface(nanometerParams);
    const SurfaceClosure subNanometerClosure =
        EvalStandardSurface(subNanometerParams);

    const Vec3f N(0.0f, 1.0f, 0.0f);
    const Vec3f wo = Vec3f(0.25f, 0.96f, 0.1f).normalized();
    const Vec3f wi = Vec3f(-0.1f, 0.99f, 0.05f).normalized();

    const Vec3f nanometerEval =
        Bsdf::EvalSurface(nanometerClosure, N, wi, wo);
    const Vec3f subNanometerEval =
        Bsdf::EvalSurface(subNanometerClosure, N, wi, wo);

    const bool nanometerChromatic =
        std::abs(nanometerEval[0] - nanometerEval[1]) > 1e-6f ||
        std::abs(nanometerEval[1] - nanometerEval[2]) > 1e-6f;
    const bool differsFromSubNanometer = !Test_IsClose(
        nanometerEval, subNanometerEval, 1e-6f);

    if (!nanometerChromatic || !differsFromSubNanometer) {
        printf(
            "    550nm=(%.8f,%.8f,%.8f) 0.55nm=(%.8f,%.8f,%.8f)\n",
            nanometerEval[0], nanometerEval[1], nanometerEval[2],
            subNanometerEval[0], subNanometerEval[1], subNanometerEval[2]);
        }

    return nanometerChromatic && differsFromSubNanometer;
}

static bool
TestStandardSurfaceCoatAffectRoughnessMatchesMaterialXGraph()
{
    ParamMap params;
    params["specular_roughness"] = Value(0.2f);
    params["transmission"] = Value(1.0f);
    params["transmission_extra_roughness"] = Value(0.1f);
    params["coat"] = Value(1.0f);
    params["coat_roughness"] = Value(0.6f);
    params["coat_affect_roughness"] = Value(1.0f);

    const SurfaceClosure c = EvalStandardSurface(params);
    const auto* reflection = FindNodeIf<Bsdf::DielectricData>(
        c.bsdfTree,
        [](const Bsdf::DielectricData& data) {
            return data.scatterMode == Bsdf::ScatterMode::Reflection;
        });
    const auto* transmission = FindNodeIf<Bsdf::DielectricData>(
        c.bsdfTree,
        [](const Bsdf::DielectricData& data) {
            return data.scatterMode == Bsdf::ScatterMode::Transmission;
        });

    if (!reflection || !transmission) {
        printf("    Failed to find Standard Surface dielectric nodes\n");
        return false;
    }

    const float affectedSpecular = 0.2f * 0.4f + 0.6f;
    const float affectedTransmission = 0.3f * 0.4f + 0.6f;
    const float expectedSpecularAlpha = affectedSpecular * affectedSpecular;
    const float expectedTransmissionAlpha =
        affectedTransmission * affectedTransmission;

    return Test_IsClose(reflection->roughness[0], expectedSpecularAlpha, 1e-4f) &&
           Test_IsClose(reflection->roughness[1], expectedSpecularAlpha, 1e-4f) &&
           Test_IsClose(transmission->roughness[0], expectedTransmissionAlpha, 1e-4f) &&
           Test_IsClose(transmission->roughness[1], expectedTransmissionAlpha, 1e-4f);
}

static bool
TestStandardSurfaceCoatColorSemanticsMatchMaterialX()
{
    const Vec3f baseColor(0.25f, 0.5f, 0.75f);
    const Vec3f coatColor(0.2f, 0.4f, 0.8f);
    const Vec3f emissionColor(0.5f, 0.25f, 0.125f);

    ParamMap params;
    params["base_color"] = Value(baseColor);
    params["specular"] = Value(0.0f);
    params["coat"] = Value(1.0f);
    params["coat_color"] = Value(coatColor);
    params["coat_affect_color"] = Value(0.5f);
    params["emission"] = Value(2.0f);
    params["emission_color"] = Value(emissionColor);

    const SurfaceClosure c = EvalStandardSurface(params);
    const auto* diffuse = FindNodeIf<Bsdf::OrenNayarDiffuseData>(
        c.bsdfTree,
        [](const Bsdf::OrenNayarDiffuseData&) {
            return true;
        });
    const auto* attenuation = FindNodeIf<Bsdf::MultiplyData>(
        c.bsdfTree,
        [&](const Bsdf::MultiplyData& data) {
            return Test_IsClose(data.weight, coatColor, 1e-4f);
        });
    const auto* coat = FindNodeIf<Bsdf::DielectricData>(
        c.bsdfTree,
        [](const Bsdf::DielectricData& data) {
            return data.scatterMode == Bsdf::ScatterMode::Reflection;
        });

    if (!diffuse || !attenuation || !coat) {
        printf("    Failed to find Standard Surface coat semantic nodes\n");
        return false;
    }

    const Vec3f expectedDiffuse(
        std::pow(baseColor[0], 1.5f),
        std::pow(baseColor[1], 1.5f),
        std::pow(baseColor[2], 1.5f));
    const Vec3f expectedEmission(
        emissionColor[0] * 2.0f * coatColor[0],
        emissionColor[1] * 2.0f * coatColor[1],
        emissionColor[2] * 2.0f * coatColor[2]);

    return Test_IsClose(diffuse->color, expectedDiffuse, 1e-4f) &&
           Test_IsClose(c.emissiveColor, expectedEmission, 1e-4f) &&
           Test_IsClose(coat->tint, Vec3f(1.0f), 1e-4f);
}

static bool
TestStandardSurfaceCoatNormalAndRotationReachBsdf()
{
    constexpr float roughness = 0.45f;
    constexpr float anisotropy = 0.8f;

    ParamMap params;
    params["specular"] = Value(0.0f);
    params["coat"] = Value(1.0f);
    params["coat_roughness"] = Value(roughness);
    params["coat_anisotropy"] = Value(anisotropy);
    params["coat_rotation"] = Value(0.25f);
    params["tangent"] = Value(Vec3f(1.0f, 0.0f, 0.0f));
    params["coat_normal"] = Value(Vec3f(0.0f, 1.0f, 0.0f));

    const SurfaceClosure c = EvalStandardSurface(params);
    const auto* coat = FindNodeIf<Bsdf::DielectricData>(
        c.bsdfTree,
        [](const Bsdf::DielectricData& data) {
            return data.scatterMode == Bsdf::ScatterMode::Reflection;
        });

    if (!coat) {
        printf("    Failed to find Standard Surface coat dielectric node\n");
        return false;
    }

    const float roughnessSquared = std::clamp(
        roughness * roughness,
        1.0e-5f,
        1.0f);
    const float aspect = std::sqrt(1.0f - std::clamp(anisotropy, 0.0f, 0.98f));
    const Vec2f expected(
        std::min(roughnessSquared / aspect, 1.0f),
        roughnessSquared * aspect);

    return coat->hasShadingNormal &&
           Test_IsClose(coat->normal, Vec3f(0.0f, 1.0f, 0.0f), 1e-4f) &&
           Test_IsClose(coat->tangent, Vec3f(0.0f, 0.0f, -1.0f), 1e-4f) &&
           Test_IsClose(coat->roughness[0], expected[0], 1e-4f) &&
           Test_IsClose(coat->roughness[1], expected[1], 1e-4f);
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
TestOpenPbrBuildsLayeredDielectricBase()
{
    ParamMap params;
    params["base_metalness"] = Value(0.0f);
    params["transmission_weight"] = Value(0.0f);
    params["coat_weight"] = Value(0.0f);
    params["fuzz_weight"] = Value(0.0f);

    const SurfaceClosure c = EvalOpenPbr(params);
    const auto* root = c.bsdfTree.Get(c.bsdfTree.root);
    if (!root) {
        printf("    Missing OpenPBR root node\n");
        return false;
    }

    const auto* layer = std::get_if<Bsdf::LayerData>(&root->data);
    if (!layer) {
        printf("    Expected layered dielectric base at the root\n");
        return false;
    }

    const auto* top = c.bsdfTree.Get(layer->top);
    const auto* base = c.bsdfTree.Get(layer->base);
    if (!top || !base) {
        printf("    Missing layered OpenPBR child nodes\n");
        return false;
    }

    const auto* dielectric = std::get_if<Bsdf::DielectricData>(&top->data);
    if (!dielectric ||
        dielectric->scatterMode != Bsdf::ScatterMode::Reflection) {
        printf("    Expected dielectric reflection layer on top of base\n");
        return false;
    }

    if (!std::holds_alternative<Bsdf::OrenNayarDiffuseData>(base->data)) {
        printf("    Expected diffuse base under dielectric reflection\n");
        return false;
    }

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
TestOpenPbrLayersReflectionOverTransmissionMix()
{
    ParamMap params;
    params["transmission_weight"] = Value(0.7f);
    params["geometry_thin_walled"] = Value(false);
    params["coat_weight"] = Value(0.0f);
    params["fuzz_weight"] = Value(0.0f);

    const SurfaceClosure c = EvalOpenPbr(params);
    const auto* root = c.bsdfTree.Get(c.bsdfTree.root);
    const auto* layer =
        root ? std::get_if<Bsdf::LayerData>(&root->data) : nullptr;
    if (!layer) {
        printf("    Expected layered OpenPBR root with transmission\n");
        return false;
    }

    const auto* base = c.bsdfTree.Get(layer->base);
    const auto* mix = base ? std::get_if<Bsdf::MixData>(&base->data) : nullptr;
    if (!mix) {
        printf("    Expected transmission mixed into dielectric substrate\n");
        return false;
    }

    const auto* bg = c.bsdfTree.Get(mix->bg);
    const auto* fg = c.bsdfTree.Get(mix->fg);
    if (!bg || !fg) {
        printf("    Missing transmission mix children\n");
        return false;
    }

    if (!std::holds_alternative<Bsdf::OrenNayarDiffuseData>(bg->data)) {
        printf("    Expected diffuse node as transmission background\n");
        return false;
    }

    const auto* transmission = std::get_if<Bsdf::DielectricData>(&fg->data);
    if (!transmission ||
        transmission->scatterMode != Bsdf::ScatterMode::Transmission) {
        printf("    Expected transmission dielectric in substrate mix\n");
        return false;
    }

    return true;
}

static bool
TestOpenPbrCoatDarkeningReachesBaseSubstrate()
{
    ParamMap params;
    params["base_color"] = Value(Vec3f(0.8f, 0.4f, 0.2f));
    params["base_metalness"] = Value(0.0f);
    params["specular_weight"] = Value(1.0f);
    params["subsurface_weight"] = Value(0.0f);
    params["coat_weight"] = Value(1.0f);
    params["coat_darkening"] = Value(1.0f);
    params["coat_ior"] = Value(1.6f);
    params["fuzz_weight"] = Value(0.0f);

    const SurfaceClosure c = EvalOpenPbr(params);
    const auto* multiply = FindNodeIf<Bsdf::MultiplyData>(
        c.bsdfTree,
        [](const Bsdf::MultiplyData& data) {
            return std::abs(data.weight[0] - 1.0f) > 1.0e-6f ||
                   std::abs(data.weight[1] - 1.0f) > 1.0e-6f ||
                   std::abs(data.weight[2] - 1.0f) > 1.0e-6f;
        });

    if (!multiply) {
        printf("    Expected non-identity substrate multiply for coat darkening\n");
        return false;
    }

    const float coatF0 = std::pow((1.6f - 1.0f) / (1.6f + 1.0f), 2.0f);
    const float kCoat = 1.0f - (1.0f - coatF0) / (1.6f * 1.6f);
    const Vec3f eBase(0.8f, 0.4f, 0.2f);
    const Vec3f expected(
        (1.0f - kCoat) / (1.0f - eBase[0] * kCoat),
        (1.0f - kCoat) / (1.0f - eBase[1] * kCoat),
        (1.0f - kCoat) / (1.0f - eBase[2] * kCoat));

    return Test_IsClose(multiply->weight, expected, 1e-4f);
}

static bool
TestOpenPbrCoatColorAttenuatesSubstrate()
{
    const Vec3f expectedCoatColor(0.25f, 0.5f, 0.75f);

    ParamMap params;
    params["coat_weight"] = Value(1.0f);
    params["coat_darkening"] = Value(0.0f);
    params["coat_color"] = Value(expectedCoatColor);
    params["fuzz_weight"] = Value(0.0f);

    const SurfaceClosure c = EvalOpenPbr(params);
    const auto* attenuation = FindNodeIf<Bsdf::MultiplyData>(
        c.bsdfTree,
        [&](const Bsdf::MultiplyData& data) {
            return Test_IsClose(data.weight, expectedCoatColor, 1e-4f);
        });
    if (!attenuation) {
        printf("    Expected coat_color attenuation multiply on substrate\n");
        return false;
    }

    const auto* coat = FindNodeIf<Bsdf::DielectricData>(
        c.bsdfTree,
        [](const Bsdf::DielectricData& data) {
            return data.scatterMode == Bsdf::ScatterMode::Reflection &&
                   std::abs(data.ior - 1.6f) < 1e-4f;
        });
    if (!coat) {
        printf("    Expected coat dielectric node\n");
        return false;
    }

    return Test_IsClose(coat->tint, Vec3f(1.0f), 1e-4f);
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

static bool
TestOpenPbrBaseDiffuseRoughnessUsesCanonicalName()
{
    ParamMap params;
    params["base_diffuse_roughness"] = Value(0.65f);
    params["base_roughness"] = Value(0.1f);

    const SurfaceClosure c = EvalOpenPbr(params);
    const auto* diffuse = FindNodeIf<Bsdf::OrenNayarDiffuseData>(
        c.bsdfTree,
        [](const Bsdf::OrenNayarDiffuseData&) {
            return true;
        });

    if (!diffuse) {
        printf("    Failed to find OpenPBR diffuse node\n");
        return false;
    }

    return Test_IsClose(diffuse->roughness, 0.65f, 1e-4f);
}

static bool
TestOpenPbrSpecularRoughnessAnisotropyUsesCanonicalName()
{
    constexpr float roughness = 0.45f;
    constexpr float anisotropy = 0.75f;

    ParamMap params;
    params["specular_roughness"] = Value(roughness);
    params["specular_roughness_anisotropy"] = Value(anisotropy);
    params["specular_anisotropy"] = Value(0.0f);

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

    const float clampedRoughness = std::clamp(roughness, 0.001f, 1.0f);
    const float clampedAnisotropy = std::clamp(anisotropy, 0.0f, 1.0f);
    const float alphaRoughness = clampedRoughness * clampedRoughness;
    const float oneMinusAnisotropy = 1.0f - clampedAnisotropy;
    const float alphaX = alphaRoughness * std::sqrt(
        2.0f /
        std::max(oneMinusAnisotropy * oneMinusAnisotropy + 1.0f, 1.0e-6f));
    const float alphaY = oneMinusAnisotropy * alphaX;
    const Vec2f expected(
        std::clamp(alphaX, 1.0e-5f, 1.0f),
        std::clamp(alphaY, 1.0e-5f, 1.0f));

    return Test_IsClose(dielectric->roughness[0], expected[0], 1e-4f) &&
           Test_IsClose(dielectric->roughness[1], expected[1], 1e-4f);
}

static bool
TestOpenPbrGeometryInputsUseCanonicalNames()
{
    const Vec3f expectedNormal(0.25f, 0.5f, 0.75f);
    const Vec3f expectedTangent(0.0f, 1.0f, 0.0f);

    ParamMap params;
    params["geometry_normal"] = Value(expectedNormal);
    params["normal"] = Value(Vec3f(0.0f, 0.0f, 1.0f));
    params["geometry_tangent"] = Value(expectedTangent);
    params["tangent"] = Value(Vec3f(1.0f, 0.0f, 0.0f));

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

    return Test_IsClose(c.normal, expectedNormal, 1e-4f) &&
           Test_IsClose(dielectric->tangent, expectedTangent, 1e-4f);
}

static bool
TestOpenPbrGeometryCoatNormalUsesCanonicalName()
{
    const Vec3f expectedCoatNormal =
        Vec3f(0.0f, 0.70710677f, 0.70710677f);

    ParamMap params;
    params["specular_weight"] = Value(0.0f);
    params["coat_weight"] = Value(1.0f);
    params["geometry_normal"] = Value(Vec3f(0.0f, 0.0f, 1.0f));
    params["coat_normal"] = Value(Vec3f(1.0f, 0.0f, 0.0f));
    params["geometry_coat_normal"] = Value(expectedCoatNormal);

    const SurfaceClosure c = EvalOpenPbr(params);
    const auto* coat = FindNodeIf<Bsdf::DielectricData>(
        c.bsdfTree,
        [](const Bsdf::DielectricData& data) {
            return data.scatterMode == Bsdf::ScatterMode::Reflection;
        });

    if (!coat) {
        printf("    Failed to find OpenPBR coat dielectric node\n");
        return false;
    }

    return coat->hasShadingNormal &&
           Test_IsClose(coat->normal, expectedCoatNormal, 1e-4f);
}

static bool
TestOpenPbrCoatRoughnessAnisotropyUsesCanonicalName()
{
    constexpr float roughness = 0.45f;
    constexpr float anisotropy = 0.75f;

    ParamMap params;
    params["specular_weight"] = Value(0.0f);
    params["coat_weight"] = Value(1.0f);
    params["coat_roughness"] = Value(roughness);
    params["coat_roughness_anisotropy"] = Value(anisotropy);

    const SurfaceClosure c = EvalOpenPbr(params);
    const auto* coat = FindNodeIf<Bsdf::DielectricData>(
        c.bsdfTree,
        [](const Bsdf::DielectricData& data) {
            return data.scatterMode == Bsdf::ScatterMode::Reflection &&
                   std::abs(data.ior - 1.6f) < 1e-4f;
        });

    if (!coat) {
        printf("    Failed to find OpenPBR coat dielectric node\n");
        return false;
    }

    const float clampedRoughness = std::clamp(roughness, 0.001f, 1.0f);
    const float clampedAnisotropy = std::clamp(anisotropy, 0.0f, 1.0f);
    const float alphaRoughness = clampedRoughness * clampedRoughness;
    const float oneMinusAnisotropy = 1.0f - clampedAnisotropy;
    const float alphaX = alphaRoughness * std::sqrt(
        2.0f /
        std::max(oneMinusAnisotropy * oneMinusAnisotropy + 1.0f, 1.0e-6f));
    const float alphaY = oneMinusAnisotropy * alphaX;
    const Vec2f expected(
        std::clamp(alphaX, 1.0e-5f, 1.0f),
        std::clamp(alphaY, 1.0e-5f, 1.0f));

    return Test_IsClose(coat->roughness[0], expected[0], 1e-4f) &&
           Test_IsClose(coat->roughness[1], expected[1], 1e-4f);
}

static bool
TestOpenPbrGeometryCoatTangentUsesCanonicalName()
{
    const Vec3f expectedCoatTangent(0.0f, 1.0f, 0.0f);

    ParamMap params;
    params["specular_weight"] = Value(0.0f);
    params["coat_weight"] = Value(1.0f);
    params["geometry_tangent"] = Value(Vec3f(1.0f, 0.0f, 0.0f));
    params["geometry_coat_tangent"] = Value(expectedCoatTangent);

    const SurfaceClosure c = EvalOpenPbr(params);
    const auto* coat = FindNodeIf<Bsdf::DielectricData>(
        c.bsdfTree,
        [](const Bsdf::DielectricData& data) {
            return data.scatterMode == Bsdf::ScatterMode::Reflection;
        });

    if (!coat) {
        printf("    Failed to find OpenPBR coat dielectric node\n");
        return false;
    }

    return Test_IsClose(coat->tangent, expectedCoatTangent, 1e-4f);
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
    if (!Test_IsClose(c.sheenRoughness, 0.3f)) return false;
    if (!c.HasBsdfTree()) return false;
    return true;
}

static bool
TestDisneyPrincipledSpecularTintReachesDielectricLayer()
{
    const Vec3f baseColor(0.2f, 0.4f, 0.7f);

    ParamMap params;
    params["baseColor"] = Value(baseColor);
    params["specular"] = Value(1.0f);
    params["specularTint"] = Value(1.0f);
    params["clearcoat"] = Value(0.0f);

    const SurfaceClosure c = EvalDisneyPrincipled(params);
    const auto* dielectric = FindNodeIf<Bsdf::GeneralizedSchlickData>(
        c.bsdfTree,
        [](const Bsdf::GeneralizedSchlickData& data) {
            return data.scatterMode == Bsdf::ScatterMode::Reflection &&
                   data.weight < 0.5f;
        });

    if (!dielectric) {
        printf("    Failed to find Disney Principled dielectric layer\n");
        return false;
    }

    return Test_IsClose(c.specularColor, baseColor, 1e-4f) &&
           Test_IsClose(dielectric->weight, 0.08f, 1e-4f) &&
           Test_IsClose(dielectric->color0, baseColor, 1e-4f) &&
           Test_IsClose(dielectric->color82, baseColor, 1e-4f) &&
           Test_IsClose(dielectric->color90, Vec3f(1.0f), 1e-4f);
}

static bool
TestDisneyPrincipledAnisotropyUsesMaterialXFormula()
{
    constexpr float roughness = 0.45f;
    constexpr float anisotropy = 0.75f;

    ParamMap params;
    params["roughness"] = Value(roughness);
    params["anisotropic"] = Value(anisotropy);
    params["specular"] = Value(1.0f);
    params["clearcoat"] = Value(0.0f);

    const SurfaceClosure c = EvalDisneyPrincipled(params);
    const auto* dielectric = FindNodeIf<Bsdf::GeneralizedSchlickData>(
        c.bsdfTree,
        [](const Bsdf::GeneralizedSchlickData& data) {
            return data.scatterMode == Bsdf::ScatterMode::Reflection &&
                   data.weight < 0.5f;
        });

    if (!dielectric) {
        printf("    Failed to find Disney Principled anisotropic dielectric layer\n");
        return false;
    }

    const float roughnessSqr =
        std::clamp(roughness * roughness, 1.0e-5f, 1.0f);
    const float clampedAnisotropy = std::clamp(anisotropy, 0.0f, 0.98f);
    const float aspect = std::sqrt(1.0f - clampedAnisotropy);
    const Vec2f expected(
        std::min(roughnessSqr / std::max(aspect, 1.0e-5f), 1.0f),
        roughnessSqr * aspect);

    return Test_IsClose(dielectric->roughness[0], expected[0], 1e-4f) &&
           Test_IsClose(dielectric->roughness[1], expected[1], 1e-4f);
}

static bool
TestDisneyPrincipledSheenUsesMaterialXDefaults()
{
    const Vec3f baseColor(0.82f, 0.28f, 0.14f);

    ParamMap params;
    params["baseColor"] = Value(baseColor);
    params["sheen"] = Value(0.7f);
    params["sheenTint"] = Value(1.0f);

    const SurfaceClosure c = EvalDisneyPrincipled(params);
    const auto* sheen = FindNodeIf<Bsdf::SheenData>(
        c.bsdfTree,
        [](const Bsdf::SheenData&) {
            return true;
        });

    if (!sheen) {
        printf("    Failed to find Disney Principled sheen layer\n");
        return false;
    }

    return Test_IsClose(c.sheenColor, baseColor, 1e-4f) &&
           Test_IsClose(c.sheenRoughness, 0.3f, 1e-4f) &&
           Test_IsClose(sheen->weight, 0.7f, 1e-4f) &&
           Test_IsClose(sheen->color, baseColor, 1e-4f) &&
           Test_IsClose(sheen->roughness, 0.3f, 1e-4f) &&
           sheen->mode == Bsdf::SheenMode::ContyKulla;
}

static bool
TestDisneyPrincipledTransmissionUsesSharpDielectric()
{
    const Vec3f baseColor(0.7f, 0.92f, 0.98f);

    ParamMap params;
    params["baseColor"] = Value(baseColor);
    params["specTrans"] = Value(0.65f);
    params["ior"] = Value(1.8f);

    const SurfaceClosure c = EvalDisneyPrincipled(params);
    const auto* transmission = FindNodeIf<Bsdf::DielectricData>(
        c.bsdfTree,
        [](const Bsdf::DielectricData& data) {
            return data.scatterMode == Bsdf::ScatterMode::Transmission;
        });

    if (!transmission) {
        printf("    Failed to find Disney Principled transmission layer\n");
        return false;
    }

    return Test_IsClose(c.transmission, 0.65f, 1e-4f) &&
           Test_IsClose(transmission->tint, baseColor, 1e-4f) &&
           Test_IsClose(transmission->ior, 1.8f, 1e-4f) &&
           Test_IsClose(transmission->roughness[0], 1.0e-5f, 1e-7f) &&
           Test_IsClose(transmission->roughness[1], 1.0e-5f, 1e-7f);
}

static bool
TestDisneyPrincipledClearcoatGlossControlsCoatRoughness()
{
    ParamMap params;
    params["baseColor"] = Value(Vec3f(0.08f, 0.22f, 0.75f));
    params["specular"] = Value(0.0f);
    params["clearcoat"] = Value(1.0f);
    params["clearcoatGloss"] = Value(0.2f);

    const SurfaceClosure c = EvalDisneyPrincipled(params);
    const auto* coat = FindNodeIf<Bsdf::GeneralizedSchlickData>(
        c.bsdfTree,
        [](const Bsdf::GeneralizedSchlickData& data) {
            return data.scatterMode == Bsdf::ScatterMode::Reflection &&
                   Test_IsClose(data.color0, Vec3f(1.0f), 1e-4f) &&
                   Test_IsClose(data.weight, 0.04f, 1e-4f);
        });

    if (!coat) {
        printf("    Failed to find Disney Principled clearcoat layer\n");
        return false;
    }

    const float expectedRoughness = 0.8f * 0.8f;
    return Test_IsClose(c.coat, 0.04f, 1e-4f) &&
           Test_IsClose(c.coatRoughness, 0.8f, 1e-4f) &&
           Test_IsClose(coat->roughness[0], expectedRoughness, 1e-4f) &&
           Test_IsClose(coat->roughness[1], expectedRoughness, 1e-4f);
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

static bool
TestGltfPbrClearcoatNormalReachesBsdf()
{
    const Vec3f clearcoatNormal(0.0f, 0.70710677f, 0.70710677f);

    ParamMap params;
    params["metallic"] = Value(0.0f);
    params["clearcoat"] = Value(1.0f);
    params["clearcoat_roughness"] = Value(0.15f);
    params["clearcoat_normal"] = Value(clearcoatNormal);
    params["tangent"] = Value(Vec3f(1.0f, 0.0f, 0.0f));

    const SurfaceClosure c = EvalGltfPbr(params);
    const auto* coat = FindNodeIf<Bsdf::DielectricData>(
        c.bsdfTree,
        [](const Bsdf::DielectricData& data) {
            return data.scatterMode == Bsdf::ScatterMode::Reflection &&
                   Test_IsClose(data.ior, 1.5f, 1e-4f) &&
                   Test_IsClose(data.weight, 1.0f, 1e-4f);
        });

    if (!coat) {
        printf("    Failed to find glTF clearcoat dielectric node\n");
        return false;
    }

    const float expectedRoughness = 0.15f * 0.15f;
    return coat->hasShadingNormal &&
           Test_IsClose(coat->normal, clearcoatNormal, 1e-4f) &&
           Test_IsClose(coat->tangent, Vec3f(1.0f, 0.0f, 0.0f), 1e-4f) &&
           Test_IsClose(coat->roughness[0], expectedRoughness, 1e-4f) &&
           Test_IsClose(coat->roughness[1], expectedRoughness, 1e-4f);
}

static bool
TestGltfPbrAnisotropyRotationRotatesTangent()
{
    constexpr float kQuarterTurnRadians = 1.57079632679f;

    ParamMap params;
    params["metallic"] = Value(0.0f);
    params["roughness"] = Value(0.4f);
    params["anisotropy_strength"] = Value(0.8f);
    params["anisotropy_rotation"] = Value(kQuarterTurnRadians);
    params["tangent"] = Value(Vec3f(1.0f, 0.0f, 0.0f));
    params["normal"] = Value(Vec3f(0.0f, 0.0f, 1.0f));

    const SurfaceClosure c = EvalGltfPbr(params);
    const auto* reflective = FindNodeIf<Bsdf::GeneralizedSchlickData>(
        c.bsdfTree,
        [](const Bsdf::GeneralizedSchlickData& data) {
            return data.scatterMode == Bsdf::ScatterMode::Reflection &&
                   data.thinFilmWeight <= 0.0f;
        });

    if (!reflective) {
        printf("    Failed to find glTF anisotropic reflection node\n");
        return false;
    }

    const float roughnessSquared = std::clamp(0.4f * 0.4f, 1.0e-5f, 1.0f);
    const float strengthSquared = 0.8f * 0.8f;
    const float alphaX = std::clamp(
        roughnessSquared * (1.0f - strengthSquared) + strengthSquared,
        1.0e-5f,
        1.0f);
    const Vec2f expected(alphaX, roughnessSquared);

    return Test_IsClose(reflective->tangent, Vec3f(0.0f, -1.0f, 0.0f), 1e-4f) &&
           Test_IsClose(reflective->roughness[0], expected[0], 1e-4f) &&
           Test_IsClose(reflective->roughness[1], expected[1], 1e-4f);
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

static bool
TestUsdPreviewSurfaceIgnoresOcclusion()
{
    ParamMap params;
    params["diffuseColor"] = Value(Vec3f(0.2f, 0.4f, 0.8f));
    params["occlusion"] = Value(0.0f);

    const SurfaceClosure c = EvalUsdPreviewSurface(params);
    return Test_IsClose(c.baseColor, Vec3f(0.2f, 0.4f, 0.8f), 1e-4f);
}

// ---------------------------------------------------------------------------

void
Test_RegisterMaterialTests()
{
    _REG(TestStandardSurfaceDefaults);
    _REG(TestStandardSurfaceMetallic);
    _REG(TestStandardSurfaceCustomParams);
    _REG(TestStandardSurfaceThinFilmParametersReachBsdf);
    _REG(TestStandardSurfaceLayersSpecularOverTransmissionMix);
    _REG(TestStandardSurfaceThinWalledUsesUnitIorTransmission);
    _REG(TestStandardSurfaceSpecularRotationUsesCanonicalName);
    _REG(TestStandardSurfaceMetalThinFilmChangesReflectionColor);
    _REG(TestStandardSurfaceThinFilmUsesNanometerUnits);
    _REG(TestStandardSurfaceCoatAffectRoughnessMatchesMaterialXGraph);
    _REG(TestStandardSurfaceCoatColorSemanticsMatchMaterialX);
    _REG(TestStandardSurfaceCoatNormalAndRotationReachBsdf);
    _REG(TestOpenPbrDefaults);
    _REG(TestOpenPbrBuildsLayeredDielectricBase);
    _REG(TestOpenPbrTransmission);
    _REG(TestOpenPbrLayersReflectionOverTransmissionMix);
    _REG(TestOpenPbrThinWalledUsesUnitIorTransmission);
    _REG(TestOpenPbrCoatDarkeningReachesBaseSubstrate);
    _REG(TestOpenPbrCoatColorAttenuatesSubstrate);
    _REG(TestOpenPbrThinFilmParametersReachBsdf);
    _REG(TestOpenPbrBaseDiffuseRoughnessUsesCanonicalName);
    _REG(TestOpenPbrSpecularRoughnessAnisotropyUsesCanonicalName);
    _REG(TestOpenPbrGeometryInputsUseCanonicalNames);
    _REG(TestOpenPbrGeometryCoatNormalUsesCanonicalName);
    _REG(TestOpenPbrCoatRoughnessAnisotropyUsesCanonicalName);
    _REG(TestOpenPbrGeometryCoatTangentUsesCanonicalName);
    _REG(TestDisneyPrincipledDefaults);
    _REG(TestDisneyPrincipledSpecularTintReachesDielectricLayer);
    _REG(TestDisneyPrincipledAnisotropyUsesMaterialXFormula);
    _REG(TestDisneyPrincipledSheenUsesMaterialXDefaults);
    _REG(TestDisneyPrincipledTransmissionUsesSharpDielectric);
    _REG(TestDisneyPrincipledClearcoatGlossControlsCoatRoughness);
    _REG(TestGltfPbrDefaults);
    _REG(TestGltfPbrAlphaMask);
    _REG(TestGltfPbrIridescenceParametersReachBsdf);
    _REG(TestGltfPbrClearcoatNormalReachesBsdf);
    _REG(TestGltfPbrAnisotropyRotationRotatesTangent);
    _REG(TestUsdPreviewSurfaceDefaults);
    _REG(TestUsdPreviewSurfaceMetallicWorkflow);
    _REG(TestUsdPreviewSurfaceSpecularWorkflow);
    _REG(TestUsdPreviewSurfaceOpacityThreshold);
    _REG(TestUsdPreviewSurfaceTransparentModeKeepsLightingResponse);
    _REG(TestUsdPreviewSurfacePresenceModeCutsLightingResponse);
    _REG(TestUsdPreviewSurfaceMaterialXOpacityModeInteger);
    _REG(TestUsdPreviewSurfaceIgnoresOcclusion);
}

#undef _REG
