//
// MaterialXCpp evaluation graph — pxr-independent.
//
#ifndef MXCPP_GRAPH_H
#define MXCPP_GRAPH_H

#include "types.h"
#include "nodeRegistry.h"
#include "mxcpp_graph_types.h"

#include <memory>
#include <vector>

namespace mxcpp {

/// \class EvalGraph
///
/// A compiled, topologically-sorted shader graph ready for per-pixel
/// sequential evaluation.
class EvalGraph
{
public:
    /// Compile a MaterialGraph into an evaluation graph.
    /// \p terminalName selects which terminal (e.g. "surface") to compile.
    static std::unique_ptr<EvalGraph> Compile(
        const MaterialGraph& network,
        const std::string& terminalName = std::string());

    /// Evaluate the compiled graph for a single shading point.
    SurfaceClosure Evaluate(const ShadingContext& ctx) const;

    bool IsValid() const { return _isValid; }

private:
    struct InputBinding {
        SlotId inputSlot = InvalidSlotId;
        bool isConnected = false;
        int sourceNodeIndex = -1;
        SlotId sourceOutputSlot = InvalidSlotId;
        Value defaultValue;
    };

    struct CompiledNode {
        NodeEvalFn evalFn = nullptr;
        std::vector<InputBinding> inputs;
    };

    struct EvalScratch {
        std::vector<NodeOutputMap> nodeOutputs;
        std::vector<ParamMap> nodeInputs;
        ParamMap terminalParams;
    };

    std::vector<CompiledNode> _nodes;
    std::vector<InputBinding> _terminalInputs;
    std::string _materialModelType;
    bool _isValid = false;

    static SurfaceClosure _EvalMaterialModel(
        const std::string& modelType,
        const ParamMap& params);

    void _BuildParamMap(
        const std::vector<InputBinding>& bindings,
        const std::vector<NodeOutputMap>& nodeOutputs,
        ParamMap* params) const;

    void _EvaluateNodes(
        const ShadingContext& ctx,
        size_t nodeCount,
        EvalScratch* scratch) const;

    bool _EvaluateNodeOutput(
        int nodeIndex,
        SlotId outputSlot,
        const ShadingContext& ctx,
        Value* out) const;

    static bool _ReevaluateInput(
        const void* userData,
        int sourceNodeIndex,
        SlotId sourceOutputSlot,
        const ShadingContext& ctx,
        Value* out);
};

}  // namespace mxcpp

#endif  // MXCPP_GRAPH_H
