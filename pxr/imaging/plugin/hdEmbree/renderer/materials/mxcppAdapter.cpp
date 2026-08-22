//
// Adapter: converts pxr types into pxr-independent mxcpp types.
//
#include "mxcppAdapter.h"

#include <renderer/materials/MaterialXCpp/mathTypes.h>

#include "pxr/base/gf/matrix3d.h"
#include "pxr/base/gf/matrix3f.h"
#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/gf/matrix4f.h"
#include "pxr/base/gf/vec2f.h"
#include "pxr/base/gf/vec3d.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/gf/vec4f.h"
#include "pxr/base/tf/diagnostic.h"
#include "pxr/base/vt/value.h"
#include "pxr/usd/sdf/assetPath.h"

#include <vector>

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
_ConvertValue(const VtValue& value)
{
    if (value.IsEmpty()) return mxcpp::Value();

    // Scalars
    if (value.IsHolding<float>())
        return mxcpp::Value(value.UncheckedGet<float>());
    if (value.IsHolding<double>())
        return mxcpp::Value(
            static_cast<float>(value.UncheckedGet<double>()));
    if (value.IsHolding<int>())
        return mxcpp::Value(value.UncheckedGet<int>());
    if (value.IsHolding<bool>())
        return mxcpp::Value(value.UncheckedGet<bool>());

    // Vectors
    if (value.IsHolding<GfVec2f>()) {
        GfVec2f gf = value.UncheckedGet<GfVec2f>();
        return mxcpp::Value(mxcpp::Vec2f(gf[0], gf[1]));
    }
    if (value.IsHolding<GfVec3f>()) {
        GfVec3f gf = value.UncheckedGet<GfVec3f>();
        return mxcpp::Value(mxcpp::Vec3f(gf[0], gf[1], gf[2]));
    }
    if (value.IsHolding<GfVec3d>()) {
        GfVec3d gf = value.UncheckedGet<GfVec3d>();
        return mxcpp::Value(mxcpp::Vec3f(
            static_cast<float>(gf[0]),
            static_cast<float>(gf[1]),
            static_cast<float>(gf[2])));
    }
    if (value.IsHolding<GfVec4f>()) {
        GfVec4f gf = value.UncheckedGet<GfVec4f>();
        return mxcpp::Value(
            mxcpp::Vec4f(gf[0], gf[1], gf[2], gf[3]));
    }
    if (value.IsHolding<GfMatrix3f>()) {
        GfMatrix3f const gf = value.UncheckedGet<GfMatrix3f>();
        mxcpp::Mat3f result;
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 3; ++col) {
                result[row][col] = gf[row][col];
            }
        }
        return mxcpp::Value(result);
    }
    if (value.IsHolding<GfMatrix3d>()) {
        GfMatrix3d const gf = value.UncheckedGet<GfMatrix3d>();
        mxcpp::Mat3f result;
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 3; ++col) {
                result[row][col] = static_cast<float>(gf[row][col]);
            }
        }
        return mxcpp::Value(result);
    }
    if (value.IsHolding<GfMatrix4f>()) {
        GfMatrix4f gf = value.UncheckedGet<GfMatrix4f>();
        mxcpp::Mat4f result;
        for (int row = 0; row < 4; ++row) {
            for (int col = 0; col < 4; ++col) {
                result[row][col] = gf[row][col];
            }
        }
        return mxcpp::Value(result);
    }
    if (value.IsHolding<GfMatrix4d>()) {
        GfMatrix4d gf = value.UncheckedGet<GfMatrix4d>();
        mxcpp::Mat4f result;
        for (int row = 0; row < 4; ++row) {
            for (int col = 0; col < 4; ++col) {
                result[row][col] = static_cast<float>(gf[row][col]);
            }
        }
        return mxcpp::Value(result);
    }

    // Strings / asset paths
    if (value.IsHolding<std::string>())
        return mxcpp::Value(value.UncheckedGet<std::string>());
    if (value.IsHolding<TfToken>())
        return mxcpp::Value(value.UncheckedGet<TfToken>().GetString());
    if (value.IsHolding<SdfAssetPath>()) {
        const SdfAssetPath& assetPath =
            value.UncheckedGet<SdfAssetPath>();
        std::string path = assetPath.GetResolvedPath();
        if (path.empty()) {
            path = assetPath.GetAssetPath();
        }
        return mxcpp::Value(path);
    }

    // Unsupported type — return empty.
    return mxcpp::Value();
}

bool
_HasConnectedInput(
    const mxcpp::GraphNode& node,
    const std::string& inputName)
{
    const auto it = node.inputConnections.find(inputName);
    return it != node.inputConnections.end() && !it->second.empty();
}

void
_ConvertAuthoredColorParameters(
    mxcpp::GraphNode* node,
    const std::string& nodePath,
    ty::RenderColorSpace renderColorSpace)
{
    if (!node) {
        return;
    }

    static const std::string colorSpacePrefix = "colorSpace:";

    for (auto& [inputName, value] : node->parameters) {
        if (inputName.compare(
                0, colorSpacePrefix.size(), colorSpacePrefix) == 0 ||
            _HasConnectedInput(*node, inputName)) {
            continue;
        }

        const auto colorSpaceIt =
            node->parameters.find(colorSpacePrefix + inputName);
        if (colorSpaceIt == node->parameters.end() ||
            !mxcpp::ValueHolds<std::string>(colorSpaceIt->second)) {
            continue;
        }

        const std::string& sourceColorSpace =
            mxcpp::ValueGet<std::string>(colorSpaceIt->second);
        GfVec3f rgb(0.0f);
        float alpha = 1.0f;
        const bool isColor3 = mxcpp::ValueHolds<mxcpp::Vec3f>(value);
        const bool isColor4 = mxcpp::ValueHolds<mxcpp::Vec4f>(value);
        if (isColor3) {
            const mxcpp::Vec3f& source =
                mxcpp::ValueGet<mxcpp::Vec3f>(value);
            rgb = GfVec3f(source[0], source[1], source[2]);
        } else if (isColor4) {
            const mxcpp::Vec4f& source =
                mxcpp::ValueGet<mxcpp::Vec4f>(value);
            rgb = GfVec3f(source[0], source[1], source[2]);
            alpha = source[3];
        } else {
            continue;
        }

        if (!ty::ConvertToRenderColorSpace(
                sourceColorSpace, renderColorSpace, &rgb)) {
            TF_WARN(
                "Unsupported color space '%s' for material input '%s' on "
                "'%s'. Leaving the authored value unchanged.",
                sourceColorSpace.c_str(),
                inputName.c_str(),
                nodePath.c_str());
            continue;
        }

        if (isColor3) {
            value = mxcpp::Value(
                mxcpp::Vec3f(rgb[0], rgb[1], rgb[2]));
        } else {
            value = mxcpp::Value(
                mxcpp::Vec4f(rgb[0], rgb[1], rgb[2], alpha));
        }
    }
}

} // anonymous namespace

mxcpp::MaterialGraph
ty::ConvertHdNetworkToMxcppGraph(
    const HdMaterialNetwork2& network,
    ty::RenderColorSpace renderColorSpace)
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
            std::vector<mxcpp::GraphConnection>& conns =
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

        _ConvertAuthoredColorParameters(
            &node, path.GetString(), renderColorSpace);
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
