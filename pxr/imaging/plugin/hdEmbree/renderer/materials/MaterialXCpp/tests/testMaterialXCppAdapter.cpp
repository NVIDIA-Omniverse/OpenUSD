//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/base/gf/vec2f.h"
#include "pxr/base/vt/value.h"

#include "../../mxcppAdapter.h"

#include <cstdio>
#include <functional>

PXR_NAMESPACE_USING_DIRECTIVE

using namespace mxcpp;

void Test_Register(const char* name, std::function<bool()> fn);

#define _REG(name) Test_Register("Adapter." #name, &name)

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

    MaterialGraph graph = ConvertHdNetworkToMxcppGraph(network);

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

    MaterialGraph graph = ConvertHdNetworkToMxcppGraph(network);

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

void
Test_RegisterAdapterTests()
{
    _REG(TestConvertNativeUsdNodesToCanonicalMxcppNodes);
    _REG(TestConvertMaterialXUsdPrimvarReaderStringToCanonicalNode);
}

#undef _REG
