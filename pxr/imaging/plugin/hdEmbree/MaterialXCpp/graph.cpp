//
// MaterialXCpp evaluation graph — pxr-independent.
//
#include "graph.h"
#include "materials/standardSurface.h"
#include "materials/openPbr.h"
#include "materials/usdPreviewSurface.h"

#include <cstdio>
#include <functional>
#include <map>

namespace mxcpp {

static const std::string _kSurface = "surface";
static const SlotName _kOut("out");
static const std::string _kStandardSurface =
    "ND_standard_surface_surfaceshader";
static const std::string _kOpenPbr =
    "ND_open_pbr_surface_surfaceshader";
static const std::string _kUsdPreviewSurface = "UsdPreviewSurface";

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

    auto graph = std::make_unique<EvalGraph>();

    // Locate the surface terminal.
    std::string terminal = terminalName.empty()
        ? _kSurface : terminalName;
    auto termIt = network.terminals.find(terminal);
    if (termIt == network.terminals.end()) {
        if (!network.terminals.empty()) {
            termIt = network.terminals.begin();
        } else {
            fprintf(stderr,
                "EvalGraph: no terminal found in material network\n");
            return graph;
        }
    }

    const std::string& terminalNodePath = termIt->second.upstreamNode;
    auto termNodeIt = network.nodes.find(terminalNodePath);
    if (termNodeIt == network.nodes.end()) {
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

            auto nodeIt = network.nodes.find(nodePath);
            if (nodeIt != network.nodes.end()) {
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
        const auto& node = network.nodes.at(path);
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

SurfaceClosure
EvalGraph::Evaluate(const ShadingContext& ctx) const
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

    for (size_t i = 0; i < _nodes.size(); ++i) {
        const auto& node = _nodes[i];
        if (!node.evalFn) continue;

        auto& inputs = scratch.nodeInputs[i];
        inputs.Clear();
        inputs.Reserve(node.inputs.size());

        auto& outputs = scratch.nodeOutputs[i];
        outputs.Clear();

        for (const auto& binding : node.inputs) {
            if (binding.isConnected && binding.sourceNodeIndex >= 0) {
                const auto& srcOutputs = scratch.nodeOutputs[binding.sourceNodeIndex];
                const Value* value = srcOutputs.Find(binding.sourceOutputSlot);
                if (value) {
                    inputs.Add(binding.inputSlot, value);
                }
            } else if (!ValueIsEmpty(binding.defaultValue)) {
                inputs.Add(binding.inputSlot, &binding.defaultValue);
            }
        }

        node.evalFn(inputs, ctx, &outputs);
    }

    // Gather terminal parameters.
    auto& terminalParams = scratch.terminalParams;
    terminalParams.Clear();
    terminalParams.Reserve(_terminalInputs.size());

    for (const auto& binding : _terminalInputs) {
        if (binding.isConnected && binding.sourceNodeIndex >= 0) {
            const auto& srcOutputs = scratch.nodeOutputs[binding.sourceNodeIndex];
            const Value* value = srcOutputs.Find(binding.sourceOutputSlot);
            if (value) {
                terminalParams.Add(binding.inputSlot, value);
            }
        } else if (!ValueIsEmpty(binding.defaultValue)) {
            terminalParams.Add(binding.inputSlot, &binding.defaultValue);
        }
    }

    return _EvalMaterialModel(_materialModelType, terminalParams);
}

// ---------------------------------------------------------------------------
// Material model dispatch
// ---------------------------------------------------------------------------

/* static */
SurfaceClosure
EvalGraph::_EvalMaterialModel(
    const std::string& modelType,
    const ParamMap& params)
{
    if (modelType == _kStandardSurface) {
        return EvalStandardSurface(params);
    }
    if (modelType == _kOpenPbr) {
        return EvalOpenPbr(params);
    }
    if (modelType == _kUsdPreviewSurface) {
        return EvalUsdPreviewSurface(params);
    }

    // Unknown model: construct a basic closure from common parameter names.
    static const SlotName baseColor("base_color");
    SurfaceClosure closure;
    closure.baseColor = Get<Vec3f>(
        params, baseColor, Vec3f(0.8f));
    return closure;
}

}  // namespace mxcpp
