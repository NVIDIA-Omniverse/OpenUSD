//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/imaging/plugin/hdEmbree/mxLite/nodes/geometricNodes.h"
#include "pxr/imaging/plugin/hdEmbree/mxLite/nodeRegistry.h"

#include "pxr/base/tf/staticTokens.h"

PXR_NAMESPACE_OPEN_SCOPE

TF_DEFINE_PRIVATE_TOKENS(_tokens,
    (out)
);

// Geometric nodes read from the shading context.

static void
_EvalPosition(const MxLiteParamMap&, const MxLiteShadingContext& ctx,
              MxLiteNodeOutputMap* outputs)
{
    (*outputs)[_tokens->out] = VtValue(ctx.position);
}

static void
_EvalNormal(const MxLiteParamMap&, const MxLiteShadingContext& ctx,
            MxLiteNodeOutputMap* outputs)
{
    (*outputs)[_tokens->out] = VtValue(ctx.normal);
}

static void
_EvalTangent(const MxLiteParamMap&, const MxLiteShadingContext& ctx,
             MxLiteNodeOutputMap* outputs)
{
    (*outputs)[_tokens->out] = VtValue(ctx.tangent);
}

static void
_EvalBitangent(const MxLiteParamMap&, const MxLiteShadingContext& ctx,
               MxLiteNodeOutputMap* outputs)
{
    (*outputs)[_tokens->out] = VtValue(ctx.bitangent);
}

static void
_EvalTexcoord(const MxLiteParamMap&, const MxLiteShadingContext& ctx,
              MxLiteNodeOutputMap* outputs)
{
    (*outputs)[_tokens->out] = VtValue(ctx.texcoord);
}

static void
_EvalGeomcolor(const MxLiteParamMap&, const MxLiteShadingContext& ctx,
               MxLiteNodeOutputMap* outputs)
{
    (*outputs)[_tokens->out] = VtValue(ctx.displayColor);
}

// ---- Registration --------------------------------------------------------

#define _REG(name, fn) reg.Register(TfToken(name), fn)

void
MxLiteRegisterGeometricNodes(MxLiteNodeRegistry& reg)
{
    _REG("ND_position_vector3",  &_EvalPosition);
    _REG("ND_normal_vector3",    &_EvalNormal);
    _REG("ND_tangent_vector3",   &_EvalTangent);
    _REG("ND_bitangent_vector3", &_EvalBitangent);
    _REG("ND_texcoord_vector2",  &_EvalTexcoord);
    _REG("ND_geomcolor_color3",  &_EvalGeomcolor);
}

#undef _REG

PXR_NAMESPACE_CLOSE_SCOPE
