//
// Adapter: converts pxr types into pxr-independent mxcpp types.
//
#include "pxr/imaging/plugin/hdEmbree/mxcppAdapter.h"
#include "pxr/imaging/plugin/hdEmbree/MaterialXCpp/mxcpp_math.h"

#include "pxr/base/vt/value.h"
#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/gf/matrix4f.h"
#include "pxr/base/gf/vec2f.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/gf/vec3d.h"
#include "pxr/base/gf/vec4f.h"
#include "pxr/usd/sdf/assetPath.h"

PXR_NAMESPACE_OPEN_SCOPE

namespace {

bool
_IsMaterialXUsdPrimvarReader(const std::string& nodeTypeId)
{
    return nodeTypeId == "ND_UsdPrimvarReader_integer" ||
           nodeTypeId == "ND_UsdPrimvarReader_boolean" ||
           nodeTypeId == "ND_UsdPrimvarReader_string" ||
           nodeTypeId == "ND_UsdPrimvarReader_filename" ||
           nodeTypeId == "ND_UsdPrimvarReader_float" ||
           nodeTypeId == "ND_UsdPrimvarReader_vector2" ||
           nodeTypeId == "ND_UsdPrimvarReader_vector3" ||
           nodeTypeId == "ND_UsdPrimvarReader_vector4";
}

bool
_IsNativeUsdPrimvarReader(const std::string& nodeTypeId)
{
    return nodeTypeId == "UsdPrimvarReader_float" ||
           nodeTypeId == "UsdPrimvarReader_float2" ||
           nodeTypeId == "UsdPrimvarReader_float3" ||
           nodeTypeId == "UsdPrimvarReader_float4" ||
           nodeTypeId == "UsdPrimvarReader_int" ||
           nodeTypeId == "UsdPrimvarReader_string" ||
           nodeTypeId == "UsdPrimvarReader_normal" ||
           nodeTypeId == "UsdPrimvarReader_point" ||
           nodeTypeId == "UsdPrimvarReader_vector" ||
           nodeTypeId == "UsdPrimvarReader_matrix";
}

bool
_IsUsdTransform2d(const std::string& nodeTypeId)
{
    return nodeTypeId == "UsdTransform2d" ||
           nodeTypeId == "ND_UsdTransform2d";
}

std::string
_CanonicalNodeTypeId(const std::string& nodeTypeId)
{
    if (nodeTypeId == "ND_UsdPrimvarReader_integer" ||
        nodeTypeId == "UsdPrimvarReader_int") {
        return "ND_geompropvalue_integer";
    }
    if (nodeTypeId == "ND_UsdPrimvarReader_boolean") {
        return "ND_geompropvalue_boolean";
    }
    if (nodeTypeId == "ND_UsdPrimvarReader_string" ||
        nodeTypeId == "UsdPrimvarReader_string") {
        return "ND_geompropvalueuniform_string";
    }
    if (nodeTypeId == "ND_UsdPrimvarReader_filename") {
        return "ND_geompropvalueuniform_filename";
    }
    if (nodeTypeId == "ND_UsdPrimvarReader_float" ||
        nodeTypeId == "UsdPrimvarReader_float") {
        return "ND_geompropvalue_float";
    }
    if (nodeTypeId == "ND_UsdPrimvarReader_vector2" ||
        nodeTypeId == "UsdPrimvarReader_float2") {
        return "ND_geompropvalue_vector2";
    }
    if (nodeTypeId == "ND_UsdPrimvarReader_vector3" ||
        nodeTypeId == "UsdPrimvarReader_float3" ||
        nodeTypeId == "UsdPrimvarReader_normal" ||
        nodeTypeId == "UsdPrimvarReader_point" ||
        nodeTypeId == "UsdPrimvarReader_vector") {
        return "ND_geompropvalue_vector3";
    }
    if (nodeTypeId == "ND_UsdPrimvarReader_vector4" ||
        nodeTypeId == "UsdPrimvarReader_float4") {
        return "ND_geompropvalue_vector4";
    }
    if (nodeTypeId == "UsdPrimvarReader_matrix") {
        return "ND_geompropvalue_matrix44";
    }
    if (_IsUsdTransform2d(nodeTypeId)) {
        return "ND_place2d_vector2";
    }
    return nodeTypeId;
}

std::string
_CanonicalInputName(
    const std::string& nodeTypeId,
    const std::string& inputName)
{
    if (_IsMaterialXUsdPrimvarReader(nodeTypeId) ||
        _IsNativeUsdPrimvarReader(nodeTypeId)) {
        if (inputName == "varname") {
            return "geomprop";
        }
        if (inputName == "fallback") {
            return "default";
        }
    }

    if (_IsUsdTransform2d(nodeTypeId)) {
        if (inputName == "in") {
            return "texcoord";
        }
        if (inputName == "rotation") {
            return "rotate";
        }
        if (inputName == "translation") {
            return "offset";
        }
    }

    return inputName;
}

std::string
_CanonicalOutputName(
    const std::string& nodeTypeId,
    const std::string& outputName)
{
    if (_IsNativeUsdPrimvarReader(nodeTypeId) &&
        outputName == "result") {
        return "out";
    }

    if (nodeTypeId == "UsdTransform2d" &&
        outputName == "result") {
        return "out";
    }

    return outputName;
}

// Convert a VtValue to an mxcpp::Value (std::any), handling type coercion.
mxcpp::Value
_ConvertValue(const VtValue& v)
{
    if (v.IsEmpty()) return mxcpp::Value();

    // Scalars
    if (v.IsHolding<float>())
        return mxcpp::Value(v.UncheckedGet<float>());
    if (v.IsHolding<double>())
        return mxcpp::Value(
            static_cast<float>(v.UncheckedGet<double>()));
    if (v.IsHolding<int>())
        return mxcpp::Value(v.UncheckedGet<int>());
    if (v.IsHolding<bool>())
        return mxcpp::Value(v.UncheckedGet<bool>());

    // Vectors
    if (v.IsHolding<GfVec2f>()) {
        auto gf = v.UncheckedGet<GfVec2f>();
        return mxcpp::Value(mxcpp::Vec2f(gf[0], gf[1]));
    }
    if (v.IsHolding<GfVec3f>()) {
        auto gf = v.UncheckedGet<GfVec3f>();
        return mxcpp::Value(mxcpp::Vec3f(gf[0], gf[1], gf[2]));
    }
    if (v.IsHolding<GfVec3d>()) {
        auto gf = v.UncheckedGet<GfVec3d>();
        return mxcpp::Value(mxcpp::Vec3f(
            static_cast<float>(gf[0]),
            static_cast<float>(gf[1]),
            static_cast<float>(gf[2])));
    }
    if (v.IsHolding<GfVec4f>()) {
        auto gf = v.UncheckedGet<GfVec4f>();
        return mxcpp::Value(
            mxcpp::Vec4f(gf[0], gf[1], gf[2], gf[3]));
    }
    if (v.IsHolding<GfMatrix4f>()) {
        auto gf = v.UncheckedGet<GfMatrix4f>();
        mxcpp::Mat4f result;
        for (int row = 0; row < 4; ++row) {
            for (int col = 0; col < 4; ++col) {
                result[row][col] = gf[row][col];
            }
        }
        return mxcpp::Value(result);
    }
    if (v.IsHolding<GfMatrix4d>()) {
        auto gf = v.UncheckedGet<GfMatrix4d>();
        mxcpp::Mat4f result;
        for (int row = 0; row < 4; ++row) {
            for (int col = 0; col < 4; ++col) {
                result[row][col] = static_cast<float>(gf[row][col]);
            }
        }
        return mxcpp::Value(result);
    }

    // Strings / asset paths
    if (v.IsHolding<std::string>())
        return mxcpp::Value(v.UncheckedGet<std::string>());
    if (v.IsHolding<TfToken>())
        return mxcpp::Value(v.UncheckedGet<TfToken>().GetString());
    if (v.IsHolding<SdfAssetPath>())
        return mxcpp::Value(
            v.UncheckedGet<SdfAssetPath>().GetResolvedPath());

    // Unsupported type — return empty.
    return mxcpp::Value();
}

} // anonymous namespace

mxcpp::MaterialGraph
ConvertHdNetworkToMxcppGraph(const HdMaterialNetwork2& network)
{
    mxcpp::MaterialGraph graph;

    // Convert nodes.
    for (const auto& [path, hdNode] : network.nodes) {
        mxcpp::GraphNode node;
        const std::string originalNodeTypeId = hdNode.nodeTypeId.GetString();
        node.nodeTypeId = _CanonicalNodeTypeId(originalNodeTypeId);

        // Parameters (VtValue → mxcpp::Value with coercion).
        for (const auto& [key, val] : hdNode.parameters) {
            node.parameters[_CanonicalInputName(
                originalNodeTypeId, key.GetString())] = _ConvertValue(val);
        }

        // Input connections.
        for (const auto& [inputName, connections] :
             hdNode.inputConnections) {
            const std::string canonicalInputName =
                _CanonicalInputName(
                    originalNodeTypeId, inputName.GetString());
            auto& conns =
                node.inputConnections[canonicalInputName];
            for (const auto& conn : connections) {
                mxcpp::GraphConnection gc;
                gc.upstreamNode = conn.upstreamNode.GetString();

                std::string upstreamNodeTypeId;
                auto upstreamIt = network.nodes.find(conn.upstreamNode);
                if (upstreamIt != network.nodes.end()) {
                    upstreamNodeTypeId =
                        upstreamIt->second.nodeTypeId.GetString();
                }

                gc.upstreamOutputName = _CanonicalOutputName(
                    upstreamNodeTypeId,
                    conn.upstreamOutputName.GetString());
                conns.push_back(std::move(gc));
            }
        }

        graph.nodes[path.GetString()] = std::move(node);
    }

    // Convert terminals.
    for (const auto& [termName, conn] : network.terminals) {
        mxcpp::GraphConnection gc;
        gc.upstreamNode = conn.upstreamNode.GetString();

        std::string upstreamNodeTypeId;
        auto upstreamIt = network.nodes.find(conn.upstreamNode);
        if (upstreamIt != network.nodes.end()) {
            upstreamNodeTypeId = upstreamIt->second.nodeTypeId.GetString();
        }

        gc.upstreamOutputName = _CanonicalOutputName(
            upstreamNodeTypeId, conn.upstreamOutputName.GetString());
        graph.terminals[termName.GetString()] = gc;
    }

    return graph;
}

PXR_NAMESPACE_CLOSE_SCOPE
