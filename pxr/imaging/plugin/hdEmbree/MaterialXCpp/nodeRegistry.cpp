//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/imaging/plugin/hdEmbree/MaterialXCpp/nodeRegistry.h"

#include "pxr/imaging/plugin/hdEmbree/MaterialXCpp/nodes/mathNodes.h"
#include "pxr/imaging/plugin/hdEmbree/MaterialXCpp/nodes/adjustmentNodes.h"
#include "pxr/imaging/plugin/hdEmbree/MaterialXCpp/nodes/channelNodes.h"
#include "pxr/imaging/plugin/hdEmbree/MaterialXCpp/nodes/conditionalNodes.h"
#include "pxr/imaging/plugin/hdEmbree/MaterialXCpp/nodes/geometricNodes.h"
#include "pxr/imaging/plugin/hdEmbree/MaterialXCpp/nodes/textureNodes.h"
#include "pxr/imaging/plugin/hdEmbree/MaterialXCpp/nodes/proceduralNodes.h"
#include "pxr/imaging/plugin/hdEmbree/MaterialXCpp/nodes/colorNodes.h"

#include <mutex>

PXR_NAMESPACE_OPEN_SCOPE
namespace mxcpp {

NodeRegistry&
NodeRegistry::GetInstance()
{
    static NodeRegistry instance;
    return instance;
}

void
NodeRegistry::Register(const TfToken& nodeTypeId, NodeEvalFn fn)
{
    _nodes[nodeTypeId] = fn;
}

NodeEvalFn
NodeRegistry::Find(const TfToken& nodeTypeId) const
{
    auto it = _nodes.find(nodeTypeId);
    if (it != _nodes.end()) {
        return it->second;
    }
    return nullptr;
}

/* static */
void
NodeRegistry::RegisterBuiltinNodes()
{
    static std::once_flag flag;
    std::call_once(flag, []() {
        auto& reg = GetInstance();
        RegisterMathNodes(reg);
        RegisterAdjustmentNodes(reg);
        RegisterChannelNodes(reg);
        RegisterConditionalNodes(reg);
        RegisterGeometricNodes(reg);
        RegisterTextureNodes(reg);
        RegisterProceduralNodes(reg);
        RegisterColorNodes(reg);
    });
}

} // namespace mxcpp
PXR_NAMESPACE_CLOSE_SCOPE
