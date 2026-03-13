//
// Adapter: converts pxr types into pxr-independent mxcpp types.
//
#include "pxr/imaging/plugin/hdEmbree/mxcppAdapter.h"
#include "pxr/imaging/plugin/hdEmbree/MaterialXCpp/mxcpp_math.h"

#include "pxr/base/vt/value.h"
#include "pxr/base/gf/vec2f.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/gf/vec3d.h"
#include "pxr/base/gf/vec4f.h"
#include "pxr/usd/sdf/assetPath.h"

PXR_NAMESPACE_OPEN_SCOPE

namespace {

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
        node.nodeTypeId = hdNode.nodeTypeId.GetString();

        // Parameters (VtValue → mxcpp::Value with coercion).
        for (const auto& [key, val] : hdNode.parameters) {
            node.parameters[key.GetString()] = _ConvertValue(val);
        }

        // Input connections.
        for (const auto& [inputName, connections] :
             hdNode.inputConnections) {
            auto& conns =
                node.inputConnections[inputName.GetString()];
            for (const auto& conn : connections) {
                mxcpp::GraphConnection gc;
                gc.upstreamNode = conn.upstreamNode.GetString();
                gc.upstreamOutputName =
                    conn.upstreamOutputName.GetString();
                conns.push_back(std::move(gc));
            }
        }

        graph.nodes[path.GetString()] = std::move(node);
    }

    // Convert terminals.
    for (const auto& [termName, conn] : network.terminals) {
        mxcpp::GraphConnection gc;
        gc.upstreamNode = conn.upstreamNode.GetString();
        gc.upstreamOutputName = conn.upstreamOutputName.GetString();
        graph.terminals[termName.GetString()] = gc;
    }

    return graph;
}

PXR_NAMESPACE_CLOSE_SCOPE
