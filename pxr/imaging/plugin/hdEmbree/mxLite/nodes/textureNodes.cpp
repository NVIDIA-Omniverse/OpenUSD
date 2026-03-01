//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/imaging/plugin/hdEmbree/mxLite/nodes/textureNodes.h"
#include "pxr/imaging/plugin/hdEmbree/mxLite/nodeRegistry.h"

#include "pxr/base/tf/staticTokens.h"
#include "pxr/usd/sdf/assetPath.h"

PXR_NAMESPACE_OPEN_SCOPE

TF_DEFINE_PRIVATE_TOKENS(_tokens,
    (file)
    (texcoord)
    (out)
    ((defaultVal, "default"))
);

// Placeholder image node: returns the default value.
// Full CPU texture sampling (via hio) can be added in a follow-up.
// The architecture allows swapping in a real sampler without changing
// the graph or other nodes.

template<typename T>
static void
_EvalImage(const MxLiteParamMap& inputs, const MxLiteShadingContext& ctx,
           MxLiteNodeOutputMap* outputs)
{
    // TODO: load and sample actual texture via hio.
    // For now, return the authored default value or a sensible fallback.
    T defaultVal = MxLiteGet<T>(inputs, TfToken("default"), MxLiteZero<T>());
    (*outputs)[_tokens->out] = VtValue(defaultVal);
}

// ---- Registration --------------------------------------------------------

#define _REG(name, fn) reg.Register(TfToken(name), fn)

void
MxLiteRegisterTextureNodes(MxLiteNodeRegistry& reg)
{
    _REG("ND_image_float",   &_EvalImage<float>);
    _REG("ND_image_color3",  &_EvalImage<GfVec3f>);
    _REG("ND_image_color4",  &_EvalImage<GfVec4f>);
    _REG("ND_image_vector2", &_EvalImage<GfVec2f>);
    _REG("ND_image_vector3", &_EvalImage<GfVec3f>);

    // tiledimage uses the same placeholder for now.
    _REG("ND_tiledimage_float",  &_EvalImage<float>);
    _REG("ND_tiledimage_color3", &_EvalImage<GfVec3f>);
    _REG("ND_tiledimage_color4", &_EvalImage<GfVec4f>);
}

#undef _REG

PXR_NAMESPACE_CLOSE_SCOPE
