//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_MXLITE_NODE_REGISTRY_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_MXLITE_NODE_REGISTRY_H

#include "pxr/pxr.h"
#include "pxr/imaging/plugin/hdEmbree/mxLite/types.h"
#include "pxr/base/tf/hashmap.h"
#include "pxr/base/tf/token.h"

PXR_NAMESPACE_OPEN_SCOPE

/// Output map produced by a node evaluation.
using MxLiteNodeOutputMap = TfHashMap<TfToken, VtValue, TfToken::HashFunctor>;

/// Evaluation function signature for a single node.
/// Receives resolved inputs, geometric shading context, and writes outputs.
using MxLiteNodeEvalFn = void(*)(
    const MxLiteParamMap& inputs,
    const MxLiteShadingContext& ctx,
    MxLiteNodeOutputMap* outputs);

/// \class MxLiteNodeRegistry
///
/// Singleton registry mapping MaterialX node type identifiers to
/// C++ evaluation functions.
class MxLiteNodeRegistry
{
public:
    static MxLiteNodeRegistry& GetInstance();

    void Register(const TfToken& nodeTypeId, MxLiteNodeEvalFn fn);
    MxLiteNodeEvalFn Find(const TfToken& nodeTypeId) const;

    /// Ensures all built-in node categories are registered. Thread-safe,
    /// idempotent. Called automatically by MxLiteEvalGraph::Compile().
    static void RegisterBuiltinNodes();

private:
    MxLiteNodeRegistry() = default;

    TfHashMap<TfToken, MxLiteNodeEvalFn, TfToken::HashFunctor> _nodes;
};

PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_IMAGING_PLUGIN_HD_EMBREE_MXLITE_NODE_REGISTRY_H
