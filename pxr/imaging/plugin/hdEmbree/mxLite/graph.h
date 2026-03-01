//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_MXLITE_GRAPH_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_MXLITE_GRAPH_H

#include "pxr/pxr.h"
#include "pxr/imaging/plugin/hdEmbree/mxLite/types.h"
#include "pxr/imaging/plugin/hdEmbree/mxLite/nodeRegistry.h"
#include "pxr/imaging/hd/material.h"

#include <memory>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

/// \class MxLiteEvalGraph
///
/// A compiled, topologically-sorted shader graph ready for per-pixel
/// sequential evaluation.
///
/// Compilation converts an HdMaterialNetwork2 into an ordered list of
/// node evaluation steps. At render time, Evaluate() walks the list,
/// feeding upstream outputs into downstream inputs, and finally
/// dispatches the terminal node's parameters to the appropriate
/// hardcoded material model.
class MxLiteEvalGraph
{
public:
    /// Compile an HdMaterialNetwork2 into an evaluation graph.
    /// \p terminalName selects which terminal (e.g. "surface") to compile.
    static std::unique_ptr<MxLiteEvalGraph> Compile(
        const HdMaterialNetwork2& network,
        const TfToken& terminalName = TfToken());

    /// Evaluate the compiled graph for a single shading point.
    MxLiteSurfaceClosure Evaluate(const MxLiteShadingContext& ctx) const;

    bool IsValid() const { return _isValid; }

private:
    struct InputBinding {
        TfToken inputName;
        bool isConnected = false;
        int sourceNodeIndex = -1;
        TfToken sourceOutputName;
        VtValue defaultValue;
    };

    struct CompiledNode {
        MxLiteNodeEvalFn evalFn = nullptr;
        std::vector<InputBinding> inputs;
    };

    std::vector<CompiledNode> _nodes;
    std::vector<InputBinding> _terminalInputs;
    TfToken _materialModelType;
    bool _isValid = false;

    static MxLiteSurfaceClosure _EvalMaterialModel(
        const TfToken& modelType,
        const MxLiteParamMap& params);
};

PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_IMAGING_PLUGIN_HD_EMBREE_MXLITE_GRAPH_H
