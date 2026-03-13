//
// MaterialXCpp node registry — pxr-independent.
//
#include "nodeRegistry.h"

#include "nodes/mathNodes.h"
#include "nodes/adjustmentNodes.h"
#include "nodes/channelNodes.h"
#include "nodes/conditionalNodes.h"
#include "nodes/geometricNodes.h"
#include "nodes/textureNodes.h"
#include "nodes/proceduralNodes.h"
#include "nodes/colorNodes.h"

#include <mutex>

namespace mxcpp {

NodeRegistry&
NodeRegistry::GetInstance()
{
    static NodeRegistry instance;
    return instance;
}

void
NodeRegistry::Register(const std::string& nodeTypeId, NodeEvalFn fn)
{
    _nodes[nodeTypeId] = fn;
}

NodeEvalFn
NodeRegistry::Find(const std::string& nodeTypeId) const
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
