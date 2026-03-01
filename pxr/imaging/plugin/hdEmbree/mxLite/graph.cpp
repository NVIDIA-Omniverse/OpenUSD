//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/imaging/plugin/hdEmbree/mxLite/graph.h"
#include "pxr/imaging/plugin/hdEmbree/mxLite/materials/standardSurface.h"
#include "pxr/imaging/plugin/hdEmbree/mxLite/materials/openPbr.h"
#include "pxr/imaging/plugin/hdEmbree/mxLite/materials/usdPreviewSurface.h"

#include "pxr/base/tf/staticTokens.h"
#include "pxr/base/tf/diagnostic.h"

#include <functional>
#include <set>

PXR_NAMESPACE_OPEN_SCOPE

TF_DEFINE_PRIVATE_TOKENS(_tokens,
    (surface)
    (out)
    ((standardSurface, "ND_standard_surface_surfaceshader"))
    ((openPbr, "ND_open_pbr_surface_surfaceshader"))
    ((usdPreviewSurface, "UsdPreviewSurface"))
);

// ---------------------------------------------------------------------------
// Compile
// ---------------------------------------------------------------------------

std::unique_ptr<MxLiteEvalGraph>
MxLiteEvalGraph::Compile(
    const HdMaterialNetwork2& network,
    const TfToken& terminalName)
{
    MxLiteNodeRegistry::RegisterBuiltinNodes();

    auto graph = std::make_unique<MxLiteEvalGraph>();

    // Locate the surface terminal.
    TfToken terminal = terminalName.IsEmpty() ? _tokens->surface : terminalName;
    auto termIt = network.terminals.find(terminal);
    if (termIt == network.terminals.end()) {
        if (!network.terminals.empty()) {
            termIt = network.terminals.begin();
        } else {
            TF_WARN("MxLiteEvalGraph: no terminal found in material network");
            return graph;
        }
    }

    const SdfPath& terminalNodePath = termIt->second.upstreamNode;
    auto termNodeIt = network.nodes.find(terminalNodePath);
    if (termNodeIt == network.nodes.end()) {
        TF_WARN("MxLiteEvalGraph: terminal node %s not found",
                 terminalNodePath.GetText());
        return graph;
    }

    graph->_materialModelType = termNodeIt->second.nodeTypeId;

    // ---- Gather reachable nodes via DFS topological sort ----
    // Post-order guarantees dependencies precede dependents.

    std::vector<SdfPath> sorted;
    std::set<SdfPath> visited;
    std::set<SdfPath> onStack;

    std::function<void(const SdfPath&)> dfs =
        [&](const SdfPath& nodePath) {
            if (visited.count(nodePath)) return;
            if (onStack.count(nodePath)) return; // cycle guard
            onStack.insert(nodePath);

            auto nodeIt = network.nodes.find(nodePath);
            if (nodeIt != network.nodes.end()) {
                for (const auto& entry : nodeIt->second.inputConnections) {
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
    std::map<SdfPath, int> nodeIndex;
    for (size_t i = 0; i < sorted.size(); ++i) {
        nodeIndex[sorted[i]] = static_cast<int>(i);
    }

    // ---- Build compiled nodes ----
    auto& registry = MxLiteNodeRegistry::GetInstance();
    graph->_nodes.resize(sorted.size());

    for (size_t i = 0; i < sorted.size(); ++i) {
        const auto& path = sorted[i];
        const auto& node = network.nodes.at(path);
        auto& compiled = graph->_nodes[i];

        compiled.evalFn = registry.Find(node.nodeTypeId);
        if (!compiled.evalFn) {
            TF_WARN("MxLiteEvalGraph: no evaluator for node type %s",
                     node.nodeTypeId.GetText());
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

MxLiteSurfaceClosure
MxLiteEvalGraph::Evaluate(const MxLiteShadingContext& ctx) const
{
    if (!_isValid) {
        return MxLiteSurfaceClosure();
    }

    // Per-node output buffer.
    std::vector<MxLiteNodeOutputMap> nodeOutputs(_nodes.size());

    for (size_t i = 0; i < _nodes.size(); ++i) {
        const auto& node = _nodes[i];
        if (!node.evalFn) continue;

        MxLiteParamMap inputs;
        for (const auto& binding : node.inputs) {
            if (binding.isConnected && binding.sourceNodeIndex >= 0) {
                const auto& srcOutputs = nodeOutputs[binding.sourceNodeIndex];
                TfToken outName = binding.sourceOutputName.IsEmpty()
                    ? _tokens->out : binding.sourceOutputName;
                auto it = srcOutputs.find(outName);
                if (it != srcOutputs.end()) {
                    inputs[binding.inputName] = it->second;
                }
            } else if (!binding.defaultValue.IsEmpty()) {
                inputs[binding.inputName] = binding.defaultValue;
            }
        }

        node.evalFn(inputs, ctx, &nodeOutputs[i]);
    }

    // Gather terminal parameters.
    MxLiteParamMap terminalParams;
    for (const auto& binding : _terminalInputs) {
        if (binding.isConnected && binding.sourceNodeIndex >= 0) {
            const auto& srcOutputs = nodeOutputs[binding.sourceNodeIndex];
            TfToken outName = binding.sourceOutputName.IsEmpty()
                ? _tokens->out : binding.sourceOutputName;
            auto it = srcOutputs.find(outName);
            if (it != srcOutputs.end()) {
                terminalParams[binding.inputName] = it->second;
            }
        } else if (!binding.defaultValue.IsEmpty()) {
            terminalParams[binding.inputName] = binding.defaultValue;
        }
    }

    return _EvalMaterialModel(_materialModelType, terminalParams);
}

// ---------------------------------------------------------------------------
// Material model dispatch
// ---------------------------------------------------------------------------

/* static */
MxLiteSurfaceClosure
MxLiteEvalGraph::_EvalMaterialModel(
    const TfToken& modelType,
    const MxLiteParamMap& params)
{
    if (modelType == _tokens->standardSurface) {
        return MxLiteEvalStandardSurface(params);
    }
    if (modelType == _tokens->openPbr) {
        return MxLiteEvalOpenPbr(params);
    }
    if (modelType == _tokens->usdPreviewSurface) {
        return MxLiteEvalUsdPreviewSurface(params);
    }

    // Unknown model: construct a basic closure from common parameter names.
    MxLiteSurfaceClosure closure;
    closure.baseColor = MxLiteGet<GfVec3f>(
        params, TfToken("base_color"), GfVec3f(0.8f));
    return closure;
}

PXR_NAMESPACE_CLOSE_SCOPE
