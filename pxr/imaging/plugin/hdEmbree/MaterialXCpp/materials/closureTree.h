//
// MaterialXCpp BSDF closure tree representation.
//
#ifndef MXCPP_MATERIALS_CLOSURE_TREE_H
#define MXCPP_MATERIALS_CLOSURE_TREE_H

#include "../mathTypes.h"

#include <cstdint>
#include <limits>
#include <utility>
#include <variant>
#include <vector>

namespace mxcpp {
namespace Bsdf {

using NodeId = std::uint32_t;
constexpr NodeId InvalidNodeId = std::numeric_limits<NodeId>::max();

enum class ScatterMode
{
    Reflection,
    Transmission,
    ReflectionTransmission
};

enum class SheenMode
{
    ContyKulla,
    Zeltner
};

enum class UnsupportedNodeKind
{
    Subsurface,
    ChiangHair
};

struct OrenNayarDiffuseData
{
    float weight = 1.0f;
    Vec3f color = Vec3f(0.18f);
    float roughness = 0.0f;
    bool energyCompensation = false;
};

struct BurleyDiffuseData
{
    float weight = 1.0f;
    Vec3f color = Vec3f(0.18f);
    float roughness = 0.0f;
};

struct TranslucentData
{
    float weight = 1.0f;
    Vec3f color = Vec3f(1.0f);
};

struct SubsurfaceData
{
    float weight = 1.0f;
    Vec3f color = Vec3f(1.0f);
    Vec3f radius = Vec3f(1.0f);
    float anisotropy = 0.0f;
};

struct DielectricData
{
    float weight = 1.0f;
    Vec3f tint = Vec3f(1.0f);
    float ior = 1.5f;
    Vec2f roughness = Vec2f(0.05f, 0.05f);
    bool retroreflective = false;
    float thinFilmWeight = 1.0f;
    float thinFilmThickness = 0.0f;
    float thinFilmIor = 1.5f;
    Vec3f normal = Vec3f(0.0f, 0.0f, 1.0f);
    bool hasShadingNormal = false;
    Vec3f tangent = Vec3f(1.0f, 0.0f, 0.0f);
    ScatterMode scatterMode = ScatterMode::Reflection;
};

struct ConductorData
{
    float weight = 1.0f;
    Vec3f ior = Vec3f(0.183f, 0.421f, 1.373f);
    Vec3f extinction = Vec3f(3.424f, 2.346f, 1.770f);
    Vec2f roughness = Vec2f(0.05f, 0.05f);
    bool retroreflective = false;
    float thinFilmWeight = 1.0f;
    float thinFilmThickness = 0.0f;
    float thinFilmIor = 1.5f;
    Vec3f tangent = Vec3f(1.0f, 0.0f, 0.0f);
};

struct GeneralizedSchlickData
{
    float weight = 1.0f;
    Vec3f color0 = Vec3f(1.0f);
    Vec3f color82 = Vec3f(1.0f);
    Vec3f color90 = Vec3f(1.0f);
    float exponent = 5.0f;
    Vec2f roughness = Vec2f(0.05f, 0.05f);
    bool retroreflective = false;
    float thinFilmWeight = 1.0f;
    float thinFilmThickness = 0.0f;
    float thinFilmIor = 1.5f;
    Vec3f tangent = Vec3f(1.0f, 0.0f, 0.0f);
    ScatterMode scatterMode = ScatterMode::Reflection;
};

struct SheenData
{
    float weight = 1.0f;
    Vec3f color = Vec3f(1.0f);
    float roughness = 0.3f;
    SheenMode mode = SheenMode::ContyKulla;
};

struct UnsupportedData
{
    UnsupportedNodeKind kind = UnsupportedNodeKind::Subsurface;
};

struct MixData
{
    NodeId fg = InvalidNodeId;
    NodeId bg = InvalidNodeId;
    float mix = 0.0f;
};

struct LayerData
{
    NodeId top = InvalidNodeId;
    NodeId base = InvalidNodeId;
};

struct AddData
{
    NodeId in1 = InvalidNodeId;
    NodeId in2 = InvalidNodeId;
};

struct MultiplyData
{
    NodeId input = InvalidNodeId;
    Vec3f weight = Vec3f(1.0f);
};

using NodeData = std::variant<
    OrenNayarDiffuseData,
    BurleyDiffuseData,
    TranslucentData,
    SubsurfaceData,
    DielectricData,
    ConductorData,
    GeneralizedSchlickData,
    SheenData,
    UnsupportedData,
    MixData,
    LayerData,
    AddData,
    MultiplyData>;

struct Node
{
    NodeData data;
};

struct ClosureTree
{
    std::vector<Node> nodes;
    NodeId root = InvalidNodeId;

    void Clear()
    {
        nodes.clear();
        root = InvalidNodeId;
    }

    bool Empty() const
    {
        return !IsValid(root);
    }

    bool IsValid(NodeId id) const
    {
        return id != InvalidNodeId && id < nodes.size();
    }

    template <class T>
    NodeId Add(T data)
    {
        nodes.push_back(Node{NodeData{std::move(data)}});
        return static_cast<NodeId>(nodes.size() - 1);
    }

    const Node* Get(NodeId id) const
    {
        return IsValid(id) ? &nodes[id] : nullptr;
    }
};

}  // namespace Bsdf
}  // namespace mxcpp

#endif  // MXCPP_MATERIALS_CLOSURE_TREE_H
