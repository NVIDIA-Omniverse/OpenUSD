//
// MaterialXCpp node registry — pxr-independent.
//
#include "nodeRegistry.h"

#include "nodes/mathNodes.h"
#include "nodes/convolutionNodes.h"
#include "nodes/nprNodes.h"
#include "nodes/adjustmentNodes.h"
#include "nodes/channelNodes.h"
#include "nodes/conditionalNodes.h"
#include "nodes/compositingNodes.h"
#include "nodes/geometricNodes.h"
#include "nodes/applicationNodes.h"
#include "nodes/textureNodes.h"
#include "nodes/texture3dNodes.h"
#include "nodes/proceduralNodes.h"
#include "nodes/procedural2dNodes.h"
#include "nodes/procedural3dNodes.h"
#include "nodes/pbrNodes.h"
#include "nodes/colorTransformNodes.h"

#include <mutex>

namespace mxcpp {

namespace {

const SlotName _color("color");
const SlotName _out("out");

void
_EvalUniformEdf(
    const ParamMap& inputs,
    const ShadingContext&,
    NodeOutputMap* outputs)
{
    // MaterialX's uniform_edf is direction-independent, so its closure can
    // be represented by emitted radiance, but keep it typed as an EDF closure
    // until ND_surface consumes it.
    (*outputs)[_out] = Value(
        UniformEdf{Get<Vec3f>(inputs, _color, Vec3f(1.0f))});
}

} // namespace

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
        RegisterConvolutionNodes(reg);
        RegisterNprNodes(reg);
        RegisterAdjustmentNodes(reg);
        RegisterChannelNodes(reg);
        RegisterConditionalNodes(reg);
        RegisterCompositingNodes(reg);
        RegisterGeometricNodes(reg);
        RegisterApplicationNodes(reg);
        RegisterTextureNodes(reg);
        RegisterTexture3dNodes(reg);
        RegisterProceduralNodes(reg);
        RegisterProcedural2dNodes(reg);
        RegisterProcedural3dNodes(reg);
        RegisterPbrNodes(reg);
        RegisterColorTransformNodes(reg);
        reg.Register("ND_uniform_edf", &_EvalUniformEdf);
    });
}

}  // namespace mxcpp
