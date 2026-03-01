//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/imaging/plugin/hdEmbree/mxLite/nodeRegistry.h"

#include "pxr/imaging/plugin/hdEmbree/mxLite/nodes/mathNodes.h"
#include "pxr/imaging/plugin/hdEmbree/mxLite/nodes/adjustmentNodes.h"
#include "pxr/imaging/plugin/hdEmbree/mxLite/nodes/channelNodes.h"
#include "pxr/imaging/plugin/hdEmbree/mxLite/nodes/conditionalNodes.h"
#include "pxr/imaging/plugin/hdEmbree/mxLite/nodes/geometricNodes.h"
#include "pxr/imaging/plugin/hdEmbree/mxLite/nodes/textureNodes.h"
#include "pxr/imaging/plugin/hdEmbree/mxLite/nodes/proceduralNodes.h"
#include "pxr/imaging/plugin/hdEmbree/mxLite/nodes/colorNodes.h"

#include <mutex>

PXR_NAMESPACE_OPEN_SCOPE

MxLiteNodeRegistry&
MxLiteNodeRegistry::GetInstance()
{
    static MxLiteNodeRegistry instance;
    return instance;
}

void
MxLiteNodeRegistry::Register(const TfToken& nodeTypeId, MxLiteNodeEvalFn fn)
{
    _nodes[nodeTypeId] = fn;
}

MxLiteNodeEvalFn
MxLiteNodeRegistry::Find(const TfToken& nodeTypeId) const
{
    auto it = _nodes.find(nodeTypeId);
    if (it != _nodes.end()) {
        return it->second;
    }
    return nullptr;
}

/* static */
void
MxLiteNodeRegistry::RegisterBuiltinNodes()
{
    static std::once_flag flag;
    std::call_once(flag, []() {
        auto& reg = GetInstance();
        MxLiteRegisterMathNodes(reg);
        MxLiteRegisterAdjustmentNodes(reg);
        MxLiteRegisterChannelNodes(reg);
        MxLiteRegisterConditionalNodes(reg);
        MxLiteRegisterGeometricNodes(reg);
        MxLiteRegisterTextureNodes(reg);
        MxLiteRegisterProceduralNodes(reg);
        MxLiteRegisterColorNodes(reg);
    });
}

PXR_NAMESPACE_CLOSE_SCOPE
