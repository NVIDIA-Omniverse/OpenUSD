//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "geometricNodes.h"
#include "../nodeRegistry.h"

#include <string>

namespace mxcpp {

static const SlotName _kOut("out");

// Geometric nodes read from the shading context.

static void
_EvalPosition(const ParamMap&, const ShadingContext& ctx,
              NodeOutputMap* outputs)
{
    (*outputs)[_kOut] = Value(ctx.position);
}

static void
_EvalNormal(const ParamMap&, const ShadingContext& ctx,
            NodeOutputMap* outputs)
{
    (*outputs)[_kOut] = Value(ctx.normal);
}

static void
_EvalTangent(const ParamMap&, const ShadingContext& ctx,
             NodeOutputMap* outputs)
{
    (*outputs)[_kOut] = Value(ctx.tangent);
}

static void
_EvalBitangent(const ParamMap&, const ShadingContext& ctx,
               NodeOutputMap* outputs)
{
    (*outputs)[_kOut] = Value(ctx.bitangent);
}

static void
_EvalTexcoord(const ParamMap&, const ShadingContext& ctx,
              NodeOutputMap* outputs)
{
    (*outputs)[_kOut] = Value(ctx.texcoord);
}

static void
_EvalGeomcolor(const ParamMap&, const ShadingContext& ctx,
               NodeOutputMap* outputs)
{
    (*outputs)[_kOut] = Value(ctx.displayColor);
}

// ---- Registration --------------------------------------------------------

#define _REG(name, fn) reg.Register(name, fn)

void
RegisterGeometricNodes(NodeRegistry& reg)
{
    _REG("ND_position_vector3",  &_EvalPosition);
    _REG("ND_normal_vector3",    &_EvalNormal);
    _REG("ND_tangent_vector3",   &_EvalTangent);
    _REG("ND_bitangent_vector3", &_EvalBitangent);
    _REG("ND_texcoord_vector2",  &_EvalTexcoord);
    _REG("ND_geomcolor_color3",  &_EvalGeomcolor);
}

#undef _REG

}  // namespace mxcpp
