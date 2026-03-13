//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_MXCPP_NODE_REGISTRY_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_MXCPP_NODE_REGISTRY_H

#include "pxr/pxr.h"
#include "pxr/imaging/plugin/hdEmbree/MaterialXCpp/types.h"
#include "pxr/base/tf/hashmap.h"
#include "pxr/base/tf/token.h"

PXR_NAMESPACE_OPEN_SCOPE
namespace mxcpp {

/// Output map produced by a node evaluation.
using NodeOutputMap = TfHashMap<TfToken, VtValue, TfToken::HashFunctor>;

/// Evaluation function signature for a single node.
/// Receives resolved inputs, geometric shading context, and writes outputs.
using NodeEvalFn = void(*)(
    const ParamMap& inputs,
    const ShadingContext& ctx,
    NodeOutputMap* outputs);

/// \class NodeRegistry
///
/// Singleton registry mapping MaterialX node type identifiers to
/// C++ evaluation functions.
class NodeRegistry
{
public:
    static NodeRegistry& GetInstance();

    void Register(const TfToken& nodeTypeId, NodeEvalFn fn);
    NodeEvalFn Find(const TfToken& nodeTypeId) const;

    /// Ensures all built-in node categories are registered. Thread-safe,
    /// idempotent. Called automatically by EvalGraph::Compile().
    static void RegisterBuiltinNodes();

private:
    NodeRegistry() = default;

    TfHashMap<TfToken, NodeEvalFn, TfToken::HashFunctor> _nodes;
};

} // namespace mxcpp
PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_IMAGING_PLUGIN_HD_EMBREE_MXCPP_NODE_REGISTRY_H
