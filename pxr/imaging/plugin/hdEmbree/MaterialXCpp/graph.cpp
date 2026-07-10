//
// MaterialXCpp evaluation graph — pxr-independent.
//
#include "graph.h"
#include "paramMap.h"
#include "surfaceShaderUtils.h"
#include "materials/disneyPrincipled.h"
#include "materials/gltfPbr.h"
#include "materials/standardSurface.h"
#include "materials/adobeOpenPbr.h"
#include "materials/openPbr.h"
#include "materials/usdPreviewSurface.h"

#include <cstdio>
#include <functional>
#include <initializer_list>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace mxcpp {

static const std::string _kSurface = "surface";
static const SlotName _kIn("in");
static const SlotName _kOut("out");
static const std::string _kStandardSurface =
    "ND_standard_surface_surfaceshader";
static const std::string _kOpenPbr =
    "ND_open_pbr_surface_surfaceshader";
static const std::string _kDisneyPrincipled =
    "ND_disney_principled";
static const std::string _kGltfPbr =
    "ND_gltf_pbr_surfaceshader";
static const std::string _kUsdPreviewSurface = "UsdPreviewSurface";
static const std::string _kMaterialXUsdPreviewSurface =
    "ND_UsdPreviewSurface_surfaceshader";
static const std::string _kSurfaceConstructor = "ND_surface";
static const std::string _kMixSurfaceShader = "ND_mix_surfaceshader";
static const std::string _kConvertFloatSurfaceShader =
    "ND_convert_float_surfaceshader";
static const std::string _kConvertIntegerSurfaceShader =
    "ND_convert_integer_surfaceshader";
static const std::string _kConvertBooleanSurfaceShader =
    "ND_convert_boolean_surfaceshader";
static const std::string _kConvertColor3SurfaceShader =
    "ND_convert_color3_surfaceshader";
static const std::string _kConvertColor4SurfaceShader =
    "ND_convert_color4_surfaceshader";
static const std::string _kConvertVector2SurfaceShader =
    "ND_convert_vector2_surfaceshader";
static const std::string _kConvertVector3SurfaceShader =
    "ND_convert_vector3_surfaceshader";
static const std::string _kConvertVector4SurfaceShader =
    "ND_convert_vector4_surfaceshader";

enum class _ImplicitDefaultKind {
    Texcoord0,
    PositionObject,
    NormalObject,
    NormalWorld,
    TangentWorld,
    BitangentWorld,
    ViewDirectionWorld
};

struct _ImplicitInputDefault {
    const char* inputName;
    _ImplicitDefaultKind kind;
};

static bool
_StartsWith(const std::string& value, const char* prefix)
{
    return value.rfind(prefix, 0) == 0;
}

static bool
_MatchesAnyPrefix(
    const std::string& value,
    std::initializer_list<const char*> prefixes)
{
    for (const char* prefix : prefixes) {
        if (_StartsWith(value, prefix)) {
            return true;
        }
    }
    return false;
}

static bool
_MatchesAnyExact(
    const std::string& value,
    std::initializer_list<const char*> names)
{
    for (const char* name : names) {
        if (value == name) {
            return true;
        }
    }
    return false;
}

static GraphNode
_MakeImplicitDefaultNode(_ImplicitDefaultKind kind)
{
    GraphNode node;
    switch (kind) {
    case _ImplicitDefaultKind::Texcoord0:
        node.nodeTypeId = "ND_texcoord_vector2";
        node.parameters["index"] = Value(0);
        break;
    case _ImplicitDefaultKind::PositionObject:
        node.nodeTypeId = "ND_position_vector3";
        node.parameters["space"] = Value(std::string("object"));
        break;
    case _ImplicitDefaultKind::NormalObject:
        node.nodeTypeId = "ND_normal_vector3";
        node.parameters["space"] = Value(std::string("object"));
        break;
    case _ImplicitDefaultKind::NormalWorld:
        node.nodeTypeId = "ND_normal_vector3";
        node.parameters["space"] = Value(std::string("world"));
        break;
    case _ImplicitDefaultKind::TangentWorld:
        node.nodeTypeId = "ND_tangent_vector3";
        node.parameters["space"] = Value(std::string("world"));
        break;
    case _ImplicitDefaultKind::BitangentWorld:
        node.nodeTypeId = "ND_bitangent_vector3";
        node.parameters["space"] = Value(std::string("world"));
        break;
    case _ImplicitDefaultKind::ViewDirectionWorld:
        node.nodeTypeId = "ND_viewdirection_vector3";
        node.parameters["space"] = Value(std::string("world"));
        break;
    }
    return node;
}

static bool
_HasInputConnection(const GraphNode& node, const char* inputName)
{
    auto it = node.inputConnections.find(inputName);
    if (it == node.inputConnections.end()) {
        return false;
    }
    for (const GraphConnection& conn : it->second) {
        if (!conn.upstreamNode.empty()) {
            return true;
        }
    }
    return false;
}

static std::vector<_ImplicitInputDefault>
_GetImplicitInputDefaults(const std::string& nodeTypeId)
{
    std::vector<_ImplicitInputDefault> defaults;

    if (_MatchesAnyPrefix(nodeTypeId, {
            "ND_image_",
            "ND_tiledimage_",
            "ND_hextiledimage_",
            "ND_ramplr_",
            "ND_ramptb_",
            "ND_ramp4_",
            "ND_splitlr_",
            "ND_splittb_",
            "ND_noise2d_",
            "ND_fractal2d_",
            "ND_cellnoise2d_",
            "ND_worleynoise2d_",
            "ND_unifiednoise2d_",
            "ND_checkerboard_",
            "ND_line_",
            "ND_circle_",
            "ND_cloverleaf_",
            "ND_hexagon_",
            "ND_grid_",
            "ND_crosshatch_",
            "ND_tiledcircles_",
            "ND_tiledcloverleafs_",
            "ND_tiledhexagons_",
            "ND_hextilednormalmap",
            "ND_heighttonormal_"})) {
        defaults.push_back({"texcoord", _ImplicitDefaultKind::Texcoord0});
    }

    if (_MatchesAnyPrefix(nodeTypeId, {
            "ND_triplanarprojection_",
            "ND_noise3d_",
            "ND_fractal3d_",
            "ND_cellnoise3d_",
            "ND_worleynoise3d_",
            "ND_unifiednoise3d_"})) {
        defaults.push_back(
            {"position", _ImplicitDefaultKind::PositionObject});
    }

    if (_StartsWith(nodeTypeId, "ND_triplanarprojection_")) {
        defaults.push_back({"normal", _ImplicitDefaultKind::NormalObject});
    }

    if (_MatchesAnyPrefix(nodeTypeId, {
            "ND_bump_",
            "ND_normalmap",
            "ND_hextilednormalmap",
            "ND_reflect_",
            "ND_refract_"}) ||
        _MatchesAnyExact(nodeTypeId, {
            "ND_oren_nayar_diffuse_bsdf",
            "ND_burley_diffuse_bsdf",
            "ND_dielectric_bsdf",
            "ND_conductor_bsdf",
            "ND_generalized_schlick_bsdf",
            "ND_translucent_bsdf",
            "ND_subsurface_bsdf",
            "ND_sheen_bsdf",
            "ND_chiang_hair_bsdf",
            "ND_conical_edf",
            "ND_measured_edf",
            "ND_facingratio_float"})) {
        defaults.push_back({"normal", _ImplicitDefaultKind::NormalWorld});
    }

    if (_MatchesAnyPrefix(nodeTypeId, {
            "ND_bump_",
            "ND_normalmap",
            "ND_hextilednormalmap"}) ||
        _MatchesAnyExact(nodeTypeId, {
            "ND_dielectric_bsdf",
            "ND_conductor_bsdf",
            "ND_generalized_schlick_bsdf"})) {
        defaults.push_back({"tangent", _ImplicitDefaultKind::TangentWorld});
    }

    if (_MatchesAnyPrefix(nodeTypeId, {
            "ND_bump_",
            "ND_normalmap",
            "ND_hextilednormalmap"})) {
        defaults.push_back(
            {"bitangent", _ImplicitDefaultKind::BitangentWorld});
    }

    if (nodeTypeId == "ND_chiang_hair_bsdf") {
        defaults.push_back(
            {"curve_direction", _ImplicitDefaultKind::TangentWorld});
    }

    if (nodeTypeId == "ND_facingratio_float") {
        defaults.push_back(
            {"viewdirection", _ImplicitDefaultKind::ViewDirectionWorld});
    }

    return defaults;
}

static std::string
_MakeImplicitNodePath(
    const MaterialGraph& graph,
    const std::string& nodePath,
    const char* inputName)
{
    const std::string base = nodePath + "/__implicit_" + inputName;
    std::string path = base;
    int suffix = 1;
    while (graph.nodes.find(path) != graph.nodes.end()) {
        path = base + "_" + std::to_string(suffix++);
    }
    return path;
}

static void
_InjectImplicitDefaultConnections(MaterialGraph* graph)
{
    if (!graph) {
        return;
    }

    std::vector<std::pair<std::string, _ImplicitInputDefault>> injections;
    for (const auto& entry : graph->nodes) {
        for (const auto& defaultInput :
             _GetImplicitInputDefaults(entry.second.nodeTypeId)) {
            if (!_HasInputConnection(entry.second, defaultInput.inputName)) {
                injections.push_back({entry.first, defaultInput});
            }
        }
    }

    for (const auto& injection : injections) {
        const std::string path = _MakeImplicitNodePath(
            *graph, injection.first, injection.second.inputName);
        graph->nodes[path] = _MakeImplicitDefaultNode(injection.second.kind);

        GraphConnection conn;
        conn.upstreamNode = path;
        conn.upstreamOutputName = "out";
        graph->nodes[injection.first]
            .inputConnections[injection.second.inputName] = {std::move(conn)};
    }
}

// ---------------------------------------------------------------------------
// Compile
// ---------------------------------------------------------------------------

std::unique_ptr<EvalGraph>
EvalGraph::Compile(
    const MaterialGraph& network,
    const std::string& terminalName)
{
    enum class _VisitState {
        Unvisited,
        Visiting,
        Visited
    };

    NodeRegistry::RegisterBuiltinNodes();

    MaterialGraph normalized = network;
    _InjectImplicitDefaultConnections(&normalized);

    auto graph = std::make_unique<EvalGraph>();

    // Locate the surface terminal.
    std::string terminal = terminalName.empty()
        ? _kSurface : terminalName;
    auto termIt = normalized.terminals.find(terminal);
    if (termIt == normalized.terminals.end()) {
        if (!normalized.terminals.empty()) {
            termIt = normalized.terminals.begin();
        } else {
            fprintf(stderr,
                "EvalGraph: no terminal found in material network\n");
            return graph;
        }
    }

    const std::string terminalNodePath = termIt->second.upstreamNode;
    auto termNodeIt = normalized.nodes.find(terminalNodePath);
    if (termNodeIt == normalized.nodes.end()) {
        fprintf(stderr, "EvalGraph: terminal node %s not found\n",
                 terminalNodePath.c_str());
        return graph;
    }

    graph->_materialModelType = termNodeIt->second.nodeTypeId;

    // ---- Gather reachable nodes via DFS topological sort ----

    std::vector<std::string> sorted;
    std::map<std::string, _VisitState> visitStates;
    bool hasErrors = false;

    std::function<bool(const std::string&)> dfs =
        [&](const std::string& nodePath) {
            _VisitState& state = visitStates[nodePath];
            if (state == _VisitState::Visited) {
                return true;
            }
            if (state == _VisitState::Visiting) {
                fprintf(stderr,
                        "EvalGraph: cycle detected involving node %s\n",
                        nodePath.c_str());
                hasErrors = true;
                return false;
            }

            state = _VisitState::Visiting;

            auto nodeIt = normalized.nodes.find(nodePath);
            if (nodeIt != normalized.nodes.end()) {
                for (const auto& entry :
                     nodeIt->second.inputConnections) {
                    for (const auto& conn : entry.second) {
                        if (conn.upstreamNode != terminalNodePath) {
                            if (!dfs(conn.upstreamNode)) {
                                return false;
                            }
                        }
                    }
                }
            }

            state = _VisitState::Visited;
            sorted.push_back(nodePath);
            return true;
        };

    // Seed from the terminal node's upstream connections.
    for (const auto& entry : termNodeIt->second.inputConnections) {
        for (const auto& conn : entry.second) {
            if (!dfs(conn.upstreamNode)) {
                return graph;
            }
        }
    }

    // Build path → sorted-index map.
    std::map<std::string, int> nodeIndex;
    for (size_t i = 0; i < sorted.size(); ++i) {
        nodeIndex[sorted[i]] = static_cast<int>(i);
    }

    // ---- Build compiled nodes ----
    auto& registry = NodeRegistry::GetInstance();
    graph->_nodes.resize(sorted.size());

    for (size_t i = 0; i < sorted.size(); ++i) {
        const auto& path = sorted[i];
        const auto& node = normalized.nodes.at(path);
        auto& compiled = graph->_nodes[i];

        compiled.evalFn = registry.Find(node.nodeTypeId);
        if (!compiled.evalFn) {
            fprintf(stderr,
                "EvalGraph: no evaluator for node type %s\n",
                node.nodeTypeId.c_str());
            hasErrors = true;
        }

        // Constant parameters.
        for (const auto& param : node.parameters) {
            InputBinding binding;
            binding.inputSlot = InternSlot(param.first);
            binding.defaultValue = param.second;
            compiled.inputs.push_back(std::move(binding));
        }

        // Connections (may override a same-named constant).
        for (const auto& connEntry : node.inputConnections) {
            if (connEntry.second.empty()) continue;
            const auto& conn = connEntry.second.front();

            auto idxIt = nodeIndex.find(conn.upstreamNode);
            if (idxIt == nodeIndex.end()) continue;

            bool replaced = false;
            for (auto& binding : compiled.inputs) {
                if (binding.inputSlot == InternSlot(connEntry.first)) {
                    binding.isConnected = true;
                    binding.sourceNodeIndex = idxIt->second;
                    binding.sourceOutputSlot = conn.upstreamOutputName.empty()
                        ? _kOut.Get()
                        : InternSlot(conn.upstreamOutputName);
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                InputBinding binding;
                binding.inputSlot = InternSlot(connEntry.first);
                binding.isConnected = true;
                binding.sourceNodeIndex = idxIt->second;
                binding.sourceOutputSlot = conn.upstreamOutputName.empty()
                    ? _kOut.Get()
                    : InternSlot(conn.upstreamOutputName);
                compiled.inputs.push_back(std::move(binding));
            }
        }
    }

    // ---- Build terminal input bindings ----
    for (const auto& param : termNodeIt->second.parameters) {
        InputBinding binding;
        binding.inputSlot = InternSlot(param.first);
        binding.defaultValue = param.second;
        graph->_terminalInputs.push_back(std::move(binding));
    }

    for (const auto& connEntry : termNodeIt->second.inputConnections) {
        if (connEntry.second.empty()) continue;
        const auto& conn = connEntry.second.front();

        auto idxIt = nodeIndex.find(conn.upstreamNode);
        if (idxIt == nodeIndex.end()) continue;

        bool replaced = false;
        for (auto& binding : graph->_terminalInputs) {
            if (binding.inputSlot == InternSlot(connEntry.first)) {
                binding.isConnected = true;
                binding.sourceNodeIndex = idxIt->second;
                binding.sourceOutputSlot = conn.upstreamOutputName.empty()
                    ? _kOut.Get()
                    : InternSlot(conn.upstreamOutputName);
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            InputBinding binding;
            binding.inputSlot = InternSlot(connEntry.first);
            binding.isConnected = true;
            binding.sourceNodeIndex = idxIt->second;
            binding.sourceOutputSlot = conn.upstreamOutputName.empty()
                ? _kOut.Get()
                : InternSlot(conn.upstreamOutputName);
            graph->_terminalInputs.push_back(std::move(binding));
        }
    }

    if (hasErrors) {
        return graph;
    }

    graph->_isValid = true;
    return graph;
}

// ---------------------------------------------------------------------------
// Evaluate
// ---------------------------------------------------------------------------

void
EvalGraph::_BuildParamMap(
    const std::vector<InputBinding>& bindings,
    const std::vector<NodeOutputMap>& nodeOutputs,
    ParamMap* params) const
{
    params->Clear();
    params->Reserve(bindings.size());

    for (const auto& binding : bindings) {
        if (binding.isConnected && binding.sourceNodeIndex >= 0) {
            const Value* value = nullptr;
            if (static_cast<size_t>(binding.sourceNodeIndex) < nodeOutputs.size()) {
                value = nodeOutputs[binding.sourceNodeIndex].Find(
                    binding.sourceOutputSlot);
            }

            params->Add(
                binding.inputSlot,
                value,
                &_ReevaluateInput,
                this,
                binding.sourceNodeIndex,
                binding.sourceOutputSlot);
            continue;
        }

        if (!ValueIsEmpty(binding.defaultValue)) {
            params->Add(binding.inputSlot, &binding.defaultValue);
        }
    }
}

void
EvalGraph::_EvaluateNodes(
    const ShadingContext& ctx,
    size_t nodeCount,
    EvalScratch* scratch) const
{
    if (scratch->nodeOutputs.size() < nodeCount) {
        scratch->nodeOutputs.resize(nodeCount);
    }
    if (scratch->nodeInputs.size() < nodeCount) {
        scratch->nodeInputs.resize(nodeCount);
    }

    for (size_t i = 0; i < nodeCount; ++i) {
        const auto& node = _nodes[i];
        if (!node.evalFn) {
            continue;
        }

        auto& inputs = scratch->nodeInputs[i];
        _BuildParamMap(node.inputs, scratch->nodeOutputs, &inputs);

        auto& outputs = scratch->nodeOutputs[i];
        outputs.Clear();
        node.evalFn(inputs, ctx, &outputs);
    }
}

bool
EvalGraph::_EvaluateNodeOutput(
    int nodeIndex,
    SlotId outputSlot,
    const ShadingContext& ctx,
    Value* out) const
{
    if (nodeIndex < 0 || static_cast<size_t>(nodeIndex) >= _nodes.size()) {
        return false;
    }

    // This runs inside an outer Evaluate() whose thread_local scratch holds
    // the values that downstream nodes are still reading, so a nested
    // re-evaluation needs its own scratch.  It is also extremely hot: texture
    // nodes re-evaluate their texcoord input three times per tap to build a
    // filter footprint, so constructing a fresh EvalScratch here made every
    // connected-texture material eval allocate and free dozens of containers,
    // and the resulting allocator contention dominated textured-material
    // shading cost on many-core renders.  Keep a per-thread, depth-indexed
    // pool of scratches instead; entries grow once and are reused for the
    // lifetime of the thread.
    thread_local std::vector<std::unique_ptr<EvalScratch>> nestedScratchPool;
    thread_local size_t nestedScratchDepth = 0;
    if (nestedScratchDepth >= nestedScratchPool.size()) {
        nestedScratchPool.emplace_back(std::make_unique<EvalScratch>());
    }
    EvalScratch* const scratch = nestedScratchPool[nestedScratchDepth].get();

    struct _DepthGuard {
        size_t& depth;
        explicit _DepthGuard(size_t& d) : depth(d) { ++depth; }
        ~_DepthGuard() { --depth; }
    } depthGuard(nestedScratchDepth);

    const size_t nodeCount = static_cast<size_t>(nodeIndex) + 1;
    _EvaluateNodes(ctx, nodeCount, scratch);

    const Value* value = scratch->nodeOutputs[nodeIndex].Find(outputSlot);
    if (!value) {
        return false;
    }

    if (out) {
        *out = *value;
    }
    return true;
}

/* static */
bool
EvalGraph::_ReevaluateInput(
    const void* userData,
    int sourceNodeIndex,
    SlotId sourceOutputSlot,
    const ShadingContext& ctx,
    Value* out)
{
    if (!userData) {
        return false;
    }

    const auto* graph = static_cast<const EvalGraph*>(userData);
    return graph->_EvaluateNodeOutput(
        sourceNodeIndex, sourceOutputSlot, ctx, out);
}

SurfaceClosure
EvalGraph::Evaluate(const ShadingContext& ctx, const EvalOptions& options) const
{
    if (!_isValid) {
        return SurfaceClosure();
    }

    thread_local EvalScratch scratch;
    // Grow-only: avoid shrink/regrow thrashing when graphs of different
    // sizes share the same thread_local scratch.
    if (scratch.nodeOutputs.size() < _nodes.size()) {
        scratch.nodeOutputs.resize(_nodes.size());
    }
    if (scratch.nodeInputs.size() < _nodes.size()) {
        scratch.nodeInputs.resize(_nodes.size());
    }
    _EvaluateNodes(ctx, _nodes.size(), &scratch);

    // Gather terminal parameters.
    auto& terminalParams = scratch.terminalParams;
    _BuildParamMap(_terminalInputs, scratch.nodeOutputs, &terminalParams);

    return _EvalMaterialModel(_materialModelType, terminalParams, options);
}

// ---------------------------------------------------------------------------
// Material model dispatch
// ---------------------------------------------------------------------------

/* static */
SurfaceClosure
EvalGraph::_EvalMaterialModel(
    const std::string& modelType,
    const ParamMap& params,
    const EvalOptions& options)
{
    if (modelType == _kStandardSurface) {
        return EvalStandardSurface(params);
    }
    if (modelType == _kOpenPbr) {
        if (options.useAdobeOpenPBR) {
            if (options.visibilityOnly) {
                return EvalAdobeOpenPbrVisibility(params);
            }
            return EvalAdobeOpenPbr(params);
        }
        return EvalOpenPbr(params);
    }
    if (modelType == _kDisneyPrincipled) {
        return EvalDisneyPrincipled(params);
    }
    if (modelType == _kGltfPbr) {
        return EvalGltfPbr(params);
    }
    if (modelType == _kUsdPreviewSurface) {
        return EvalUsdPreviewSurface(params);
    }
    if (modelType == _kMaterialXUsdPreviewSurface) {
        return EvalUsdPreviewSurface(params);
    }
    if (modelType == _kSurfaceConstructor) {
        return EvalSurfaceConstructor(params);
    }
    if (modelType == _kMixSurfaceShader) {
        static const SlotName bg("bg");
        static const SlotName fg("fg");
        static const SlotName mix("mix");

        const SurfaceClosure empty = MakeEmptySurfaceClosure();
        return MixSurfaceClosures(
            Get<SurfaceClosure>(params, bg, empty),
            Get<SurfaceClosure>(params, fg, empty),
            Get<float>(params, mix, 0.0f));
    }
    if (modelType == _kConvertFloatSurfaceShader) {
        const float v = Get<float>(params, _kIn, 0.0f);
        return MakeUnlitSurfaceClosure(Vec3f(v));
    }
    if (modelType == _kConvertIntegerSurfaceShader) {
        const float v = static_cast<float>(Get<int>(params, _kIn, 0));
        return MakeUnlitSurfaceClosure(Vec3f(v));
    }
    if (modelType == _kConvertBooleanSurfaceShader) {
        const float v = Get<bool>(params, _kIn, false) ? 1.0f : 0.0f;
        return MakeUnlitSurfaceClosure(Vec3f(v));
    }
    if (modelType == _kConvertColor3SurfaceShader ||
        modelType == _kConvertVector3SurfaceShader) {
        return MakeUnlitSurfaceClosure(
            Get<Vec3f>(params, _kIn, Vec3f(0.0f)));
    }
    if (modelType == _kConvertColor4SurfaceShader ||
        modelType == _kConvertVector4SurfaceShader) {
        const Vec4f v = Get<Vec4f>(
            params, _kIn, Vec4f(0.0f, 0.0f, 0.0f, 1.0f));
        return MakeUnlitSurfaceClosure(Vec3f(v[0], v[1], v[2]), v[3]);
    }
    if (modelType == _kConvertVector2SurfaceShader) {
        const Vec2f v = Get<Vec2f>(params, _kIn, Vec2f(0.0f));
        return MakeUnlitSurfaceClosure(Vec3f(v[0], v[1], 0.0f));
    }

    // Unknown model: construct a basic closure from common parameter names.
    static const SlotName baseColor("base_color");
    SurfaceClosure closure;
    closure.baseColor = Get<Vec3f>(
        params, baseColor, Vec3f(0.8f));
    return closure;
}

}  // namespace mxcpp
