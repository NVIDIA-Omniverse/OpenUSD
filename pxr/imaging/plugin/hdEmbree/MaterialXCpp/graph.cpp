//
// MaterialXCpp evaluation graph — pxr-independent.
//
#include "graph.h"
#include "materials/standardSurface.h"
#include "materials/openPbr.h"
#include "materials/usdPreviewSurface.h"

#include <cstdio>
#include <functional>
#include <set>

namespace mxcpp {

static const std::string _kSurface = "surface";
static const std::string _kOut = "out";
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
    std::set<std::string> visited;
    std::set<std::string> onStack;

    std::function<void(const std::string&)> dfs =
        [&](const std::string& nodePath) {
            if (visited.count(nodePath)) return;
            if (onStack.count(nodePath)) return; // cycle guard
            onStack.insert(nodePath);

            auto nodeIt = network.nodes.find(nodePath);
            if (nodeIt != network.nodes.end()) {
                for (const auto& entry :
                     nodeIt->second.inputConnections) {
                    for (const auto& conn : entry.second) {
                        if (conn.upstreamNode != terminalNodePath) {
                            dfs(conn.upstreamNode);
                        }
                    }
                }
            }

            onStack.erase(nodePath);
            visited.insert(nodePath);
            sorted.push_back(nodePath);
        };

    // Seed from the terminal node's upstream connections.
    for (const auto& entry : termNodeIt->second.inputConnections) {
        for (const auto& conn : entry.second) {
            dfs(conn.upstreamNode);
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
        }

        // Constant parameters.
        for (const auto& param : node.parameters) {
            InputBinding binding;
            binding.inputName = param.first;
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
                if (binding.inputName == connEntry.first) {
                    binding.isConnected = true;
                    binding.sourceNodeIndex = idxIt->second;
                    binding.sourceOutputName = conn.upstreamOutputName;
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                InputBinding binding;
                binding.inputName = connEntry.first;
                binding.isConnected = true;
                binding.sourceNodeIndex = idxIt->second;
                binding.sourceOutputName = conn.upstreamOutputName;
                compiled.inputs.push_back(std::move(binding));
            }
        }
    }

    // ---- Build terminal input bindings ----
    for (const auto& param : termNodeIt->second.parameters) {
        InputBinding binding;
        binding.inputName = param.first;
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
            if (binding.inputName == connEntry.first) {
                binding.isConnected = true;
                binding.sourceNodeIndex = idxIt->second;
                binding.sourceOutputName = conn.upstreamOutputName;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            InputBinding binding;
            binding.inputName = connEntry.first;
            binding.isConnected = true;
            binding.sourceNodeIndex = idxIt->second;
            binding.sourceOutputName = conn.upstreamOutputName;
            graph->_terminalInputs.push_back(std::move(binding));
        }
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

    // Per-node output buffer.
    std::vector<NodeOutputMap> nodeOutputs(_nodes.size());

    for (size_t i = 0; i < _nodes.size(); ++i) {
        const auto& node = _nodes[i];
        if (!node.evalFn) continue;

        ParamMap inputs;
        for (const auto& binding : node.inputs) {
            if (binding.isConnected && binding.sourceNodeIndex >= 0) {
                const auto& srcOutputs =
                    nodeOutputs[binding.sourceNodeIndex];
                std::string outName =
                    binding.sourceOutputName.empty()
                    ? _kOut : binding.sourceOutputName;
                auto it = srcOutputs.find(outName);
                if (it != srcOutputs.end()) {
                    inputs[binding.inputName] = it->second;
                }
            } else if (binding.defaultValue.has_value()) {
                inputs[binding.inputName] = binding.defaultValue;
            }
        }

        node.evalFn(inputs, ctx, &nodeOutputs[i]);
    }

    // Gather terminal parameters.
    ParamMap terminalParams;
    for (const auto& binding : _terminalInputs) {
        if (binding.isConnected && binding.sourceNodeIndex >= 0) {
            const auto& srcOutputs =
                nodeOutputs[binding.sourceNodeIndex];
            std::string outName =
                binding.sourceOutputName.empty()
                ? _kOut : binding.sourceOutputName;
            auto it = srcOutputs.find(outName);
            if (it != srcOutputs.end()) {
                terminalParams[binding.inputName] = it->second;
            }
        } else if (binding.defaultValue.has_value()) {
            terminalParams[binding.inputName] = binding.defaultValue;
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
    SurfaceClosure closure;
    closure.baseColor = Get<Vec3f>(
        params, "base_color", Vec3f(0.8f));
    return closure;
}

} // namespace mxcpp
