//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include <renderer/materials/MaterialXCpp/graph.h>
#include <renderer/materials/mxcppAdapter.h>

#include "pxr/base/gf/color.h"
#include "pxr/base/gf/colorSpace.h"
#include "pxr/base/gf/vec2f.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/gf/vec4f.h"
#include "pxr/base/vt/value.h"
#include "pxr/usd/sdf/assetPath.h"

#include <cmath>
#include <cstdio>
#include <functional>

PXR_NAMESPACE_USING_DIRECTIVE

using namespace mxcpp;

void Test_Register(const char* name, std::function<bool()> fn);

#define _REG(name) Test_Register("Adapter." #name, &name)

static bool
_IsClose(const Vec3f& a, const GfVec3f& b, float tolerance = 1.0e-6f)
{
    return std::abs(a[0] - b[0]) <= tolerance &&
           std::abs(a[1] - b[1]) <= tolerance &&
           std::abs(a[2] - b[2]) <= tolerance;
}

static bool
TestConvertNativeUsdNodesToCanonicalMxcppNodes()
{
    HdMaterialNetwork2 network;

    const SdfPath primvarPath("/Material/Primvar");
    HdMaterialNode2 primvarNode;
    primvarNode.nodeTypeId = TfToken("UsdPrimvarReader_float2");
    primvarNode.parameters[TfToken("varname")] = VtValue(std::string("st"));
    primvarNode.parameters[TfToken("fallback")] =
        VtValue(GfVec2f(0.25f, 0.75f));
    network.nodes[primvarPath] = primvarNode;

    const SdfPath transformPath("/Material/Transform");
    HdMaterialNode2 transformNode;
    transformNode.nodeTypeId = TfToken("UsdTransform2d");
    transformNode.parameters[TfToken("rotation")] = VtValue(90.0f);
    transformNode.parameters[TfToken("scale")] = VtValue(GfVec2f(2.0f, 3.0f));
    transformNode.parameters[TfToken("translation")] =
        VtValue(GfVec2f(4.0f, 5.0f));

    HdMaterialConnection2 transformInputConn;
    transformInputConn.upstreamNode = primvarPath;
    transformInputConn.upstreamOutputName = TfToken("result");
    transformNode.inputConnections[TfToken("in")].push_back(transformInputConn);
    network.nodes[transformPath] = transformNode;

    MaterialGraph graph = ty::ConvertHdNetworkToMxcppGraph(network);

    const auto primvarIt = graph.nodes.find(primvarPath.GetString());
    if (primvarIt == graph.nodes.end()) {
        std::printf("    Missing converted primvar node\n");
        return false;
    }
    if (primvarIt->second.nodeTypeId != "ND_geompropvalue_vector2") {
        std::printf("    primvar nodeTypeId: %s\n",
                    primvarIt->second.nodeTypeId.c_str());
        return false;
    }
    if (primvarIt->second.parameters.count("varname") != 0 ||
        primvarIt->second.parameters.count("fallback") != 0) {
        std::printf("    Primvar parameters were not renamed\n");
        return false;
    }
    const Value* geomprop = &primvarIt->second.parameters.at("geomprop");
    const Value* fallback = &primvarIt->second.parameters.at("default");
    if (!ValueHolds<std::string>(*geomprop) ||
        ValueGet<std::string>(*geomprop) != "st") {
        std::printf("    geomprop rename failed\n");
        return false;
    }
    if (!ValueHolds<Vec2f>(*fallback) ||
        ValueGet<Vec2f>(*fallback) != Vec2f(0.25f, 0.75f)) {
        std::printf("    fallback rename failed\n");
        return false;
    }

    const auto transformIt = graph.nodes.find(transformPath.GetString());
    if (transformIt == graph.nodes.end()) {
        std::printf("    Missing converted transform node\n");
        return false;
    }
    if (transformIt->second.nodeTypeId != "ND_place2d_vector2") {
        std::printf("    transform nodeTypeId: %s\n",
                    transformIt->second.nodeTypeId.c_str());
        return false;
    }
    if (transformIt->second.parameters.count("rotation") != 0 ||
        transformIt->second.parameters.count("translation") != 0) {
        std::printf("    Transform parameters were not renamed\n");
        return false;
    }
    if (transformIt->second.parameters.count("rotate") == 0 ||
        transformIt->second.parameters.count("offset") == 0) {
        std::printf("    Transform canonical parameters missing\n");
        return false;
    }

    const auto connIt = transformIt->second.inputConnections.find("texcoord");
    if (connIt == transformIt->second.inputConnections.end() ||
        connIt->second.empty()) {
        std::printf("    Missing texcoord connection\n");
        return false;
    }
    if (connIt->second.front().upstreamOutputName != "out") {
        std::printf("    upstream output rename failed: %s\n",
                    connIt->second.front().upstreamOutputName.c_str());
        return false;
    }

    return true;
}

static bool
TestConvertAssetPathUsesResolvedPathThenAuthoredFallback()
{
    HdMaterialNetwork2 network;

    const SdfPath texturePath("/Material/Texture");
    HdMaterialNode2 textureNode;
    textureNode.nodeTypeId = TfToken("UsdUVTexture");
    textureNode.parameters[TfToken("resolvedFile")] = VtValue(
        SdfAssetPath(
            "textures/color.<UDIM>.exr",
            "/show/textures/color.<UDIM>.exr"));
    textureNode.parameters[TfToken("authoredFile")] = VtValue(
        SdfAssetPath("textures/fallback.<UDIM>.exr"));
    network.nodes[texturePath] = textureNode;

    const MaterialGraph graph = ty::ConvertHdNetworkToMxcppGraph(network);
    const auto nodeIt = graph.nodes.find(texturePath.GetString());
    if (nodeIt == graph.nodes.end()) {
        std::printf("    Missing converted texture node\n");
        return false;
    }

    const Value& resolved = nodeIt->second.parameters.at("resolvedFile");
    const Value& authored = nodeIt->second.parameters.at("authoredFile");
    return ValueHolds<std::string>(resolved) &&
        ValueGet<std::string>(resolved) ==
            "/show/textures/color.<UDIM>.exr" &&
        ValueHolds<std::string>(authored) &&
        ValueGet<std::string>(authored) ==
            "textures/fallback.<UDIM>.exr";
}

static bool
TestConvertAuthoredSolidColorsToRenderSpace()
{
    HdMaterialNetwork2 network;

    const GfVec3f authoredAp1(
        0.505387187f, 0.216290325f, 0.086754665f);
    const GfVec4f authoredSrgb(
        0.313304096f, 0.601243377f, 0.896243751f, 0.37f);

    const SdfPath surfacePath("/Material/Surface");
    HdMaterialNode2 surfaceNode;
    surfaceNode.nodeTypeId = TfToken("UsdPreviewSurface");
    surfaceNode.parameters[TfToken("diffuseColor")] =
        VtValue(authoredAp1);
    surfaceNode.parameters[TfToken("colorSpace:diffuseColor")] =
        VtValue(GfColorSpaceNames->LinearAP1);
    surfaceNode.parameters[TfToken("testColor4")] =
        VtValue(authoredSrgb);
    surfaceNode.parameters[TfToken("colorSpace:testColor4")] =
        VtValue(GfColorSpaceNames->SRGBRec709);
    network.nodes[surfacePath] = surfaceNode;

    const MaterialGraph graph = ty::ConvertHdNetworkToMxcppGraph(
        network, ty::RenderColorSpace::LinearRec709);
    const auto nodeIt = graph.nodes.find(surfacePath.GetString());
    if (nodeIt == graph.nodes.end()) {
        std::printf("    Missing converted solid-color node\n");
        return false;
    }

    const auto diffuseIt = nodeIt->second.parameters.find("diffuseColor");
    const auto color4It = nodeIt->second.parameters.find("testColor4");
    const auto diffuseSpaceIt =
        nodeIt->second.parameters.find("colorSpace:diffuseColor");
    if (diffuseIt == nodeIt->second.parameters.end() ||
        color4It == nodeIt->second.parameters.end() ||
        diffuseSpaceIt == nodeIt->second.parameters.end() ||
        !ValueHolds<Vec3f>(diffuseIt->second) ||
        !ValueHolds<Vec4f>(color4It->second) ||
        !ValueHolds<std::string>(diffuseSpaceIt->second)) {
        std::printf("    Converted solid-color parameters are malformed\n");
        return false;
    }

    const GfVec3f expectedAp1ToRec709 =
        GfColorSpace(GfColorSpaceNames->LinearRec709)
            .Convert(
                GfColorSpace(GfColorSpaceNames->LinearAP1),
                authoredAp1)
            .GetRGB();
    const GfVec3f expectedSrgbToRec709 =
        GfColorSpace(GfColorSpaceNames->LinearRec709)
            .Convert(
                GfColorSpace(GfColorSpaceNames->SRGBRec709),
                GfVec3f(
                    authoredSrgb[0],
                    authoredSrgb[1],
                    authoredSrgb[2]))
            .GetRGB();

    const Vec3f& convertedColor3 = ValueGet<Vec3f>(diffuseIt->second);
    const Vec4f& convertedColor4 = ValueGet<Vec4f>(color4It->second);
    if (!_IsClose(convertedColor3, expectedAp1ToRec709) ||
        !_IsClose(
            Vec3f(
                convertedColor4[0],
                convertedColor4[1],
                convertedColor4[2]),
            expectedSrgbToRec709) ||
        std::abs(convertedColor4[3] - authoredSrgb[3]) > 1.0e-6f) {
        std::printf("    Authored solid colors were not converted correctly\n");
        return false;
    }

    if (ValueGet<std::string>(diffuseSpaceIt->second) !=
        GfColorSpaceNames->LinearAP1.GetString()) {
        std::printf("    Solid input color-space metadata was not preserved\n");
        return false;
    }

    const MaterialGraph dataGraph = ty::ConvertHdNetworkToMxcppGraph(
        network, ty::RenderColorSpace::Data);
    const auto dataNodeIt = dataGraph.nodes.find(surfacePath.GetString());
    if (dataNodeIt == dataGraph.nodes.end()) {
        return false;
    }
    const Value& dataValue =
        dataNodeIt->second.parameters.at("diffuseColor");
    return ValueHolds<Vec3f>(dataValue) &&
           _IsClose(ValueGet<Vec3f>(dataValue), authoredAp1);
}

static bool
TestConnectedColorInputSkipsConstantColorTransform()
{
    HdMaterialNetwork2 network;

    const SdfPath upstreamPath("/Material/Upstream");
    HdMaterialNode2 upstreamNode;
    upstreamNode.nodeTypeId = TfToken("ND_constant_color3");
    upstreamNode.parameters[TfToken("value")] =
        VtValue(GfVec3f(0.1f, 0.2f, 0.3f));
    network.nodes[upstreamPath] = upstreamNode;

    const GfVec3f authoredAp1(
        0.505387187f, 0.216290325f, 0.086754665f);
    const SdfPath surfacePath("/Material/Surface");
    HdMaterialNode2 surfaceNode;
    surfaceNode.nodeTypeId = TfToken("UsdPreviewSurface");
    surfaceNode.parameters[TfToken("diffuseColor")] =
        VtValue(authoredAp1);
    surfaceNode.parameters[TfToken("colorSpace:diffuseColor")] =
        VtValue(GfColorSpaceNames->LinearAP1);
    surfaceNode.inputConnections[TfToken("diffuseColor")].push_back(
        HdMaterialConnection2{upstreamPath, TfToken("out")});
    network.nodes[surfacePath] = surfaceNode;

    const MaterialGraph graph = ty::ConvertHdNetworkToMxcppGraph(
        network, ty::RenderColorSpace::LinearRec709);
    const Value& fallback =
        graph.nodes.at(surfacePath.GetString())
            .parameters.at("diffuseColor");
    return ValueHolds<Vec3f>(fallback) &&
           _IsClose(ValueGet<Vec3f>(fallback), authoredAp1);
}

static bool
TestConvertMaterialXUsdPrimvarReaderStringToCanonicalNode()
{
    HdMaterialNetwork2 network;

    const SdfPath primvarPath("/Material/Primvar");
    HdMaterialNode2 primvarNode;
    primvarNode.nodeTypeId = TfToken("ND_UsdPrimvarReader_string");
    primvarNode.parameters[TfToken("varname")] =
        VtValue(std::string("assetName"));
    primvarNode.parameters[TfToken("fallback")] =
        VtValue(std::string("fallback.usd"));
    network.nodes[primvarPath] = primvarNode;

    MaterialGraph graph = ty::ConvertHdNetworkToMxcppGraph(network);

    const auto primvarIt = graph.nodes.find(primvarPath.GetString());
    if (primvarIt == graph.nodes.end()) {
        std::printf("    Missing converted MaterialX primvar node\n");
        return false;
    }
    if (primvarIt->second.nodeTypeId != "ND_geompropvalueuniform_string") {
        std::printf("    nodeTypeId: %s\n",
                    primvarIt->second.nodeTypeId.c_str());
        return false;
    }
    if (primvarIt->second.parameters.count("geomprop") == 0 ||
        primvarIt->second.parameters.count("default") == 0) {
        std::printf("    Missing canonical uniform parameters\n");
        return false;
    }
    if (primvarIt->second.parameters.count("varname") != 0 ||
        primvarIt->second.parameters.count("fallback") != 0) {
        std::printf("    Original MaterialX parameter names remain\n");
        return false;
    }

    return true;
}

static bool
TestConvertAndCompileSurfaceAndDisplacementTerminals()
{
    HdMaterialNetwork2 network;

    const SdfPath surfacePath("/Material/Surface");
    HdMaterialNode2 surfaceNode;
    surfaceNode.nodeTypeId = TfToken("UsdPreviewSurface");
    surfaceNode.parameters[TfToken("diffuseColor")] =
        VtValue(GfVec3f(0.2f, 0.3f, 0.4f));
    network.nodes[surfacePath] = surfaceNode;

    const SdfPath displacementPath("/Material/Displacement");
    HdMaterialNode2 displacementNode;
    displacementNode.nodeTypeId = TfToken("ND_displacement_float");
    displacementNode.parameters[TfToken("displacement")] = VtValue(0.25f);
    displacementNode.parameters[TfToken("scale")] = VtValue(2.0f);
    network.nodes[displacementPath] = displacementNode;

    network.terminals[TfToken("surface")] =
        HdMaterialConnection2{surfacePath, TfToken("out")};
    network.terminals[TfToken("displacement")] =
        HdMaterialConnection2{displacementPath, TfToken("out")};

    MaterialGraph graph = ty::ConvertHdNetworkToMxcppGraph(network);
    if (graph.terminals.count("surface") != 1 ||
        graph.terminals.count("displacement") != 1) {
        std::printf("    Surface or displacement terminal was lost\n");
        return false;
    }

    CompileResult surfaceResult = EvalGraph::Compile(graph, "surface");

    std::unique_ptr<EvalGraph>& surface = surfaceResult.graph;
    CompileResult displacementResult = EvalGraph::Compile(graph, "displacement");
    std::unique_ptr<EvalGraph>& displacement = displacementResult.graph;
    float value = 0.0f;
    ShadingContext context;
    return surface && surface->IsValid() &&
        displacement && displacement->IsValid() &&
        displacement->EvaluateDisplacement(context, &value) &&
        std::abs(value - 0.5f) < 1.0e-6f;
}

void
Test_RegisterAdapterTests()
{
    _REG(TestConvertNativeUsdNodesToCanonicalMxcppNodes);
    _REG(TestConvertAssetPathUsesResolvedPathThenAuthoredFallback);
    _REG(TestConvertAuthoredSolidColorsToRenderSpace);
    _REG(TestConnectedColorInputSkipsConstantColorTransform);
    _REG(TestConvertMaterialXUsdPrimvarReaderStringToCanonicalNode);
    _REG(TestConvertAndCompileSurfaceAndDisplacementTerminals);
}

#undef _REG
