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
static const SlotName _kGeomprop("geomprop");
static const SlotName _kDefault("default");

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

// ---- Geometric property nodes --------------------------------------------

template<typename T>
static void
_EvalGeomPropValue(const ParamMap& inputs, const ShadingContext& ctx,
                   NodeOutputMap* outputs)
{
    std::string name = Get<std::string>(inputs, _kGeomprop, std::string());
    T defaultVal = Get<T>(inputs, _kDefault, Zero<T>());
    if (!name.empty() && ctx.geomPropLookup) {
        Value v = ctx.geomPropLookup(ctx.geomPropUserData, name);
        if (ValueHolds<T>(v)) {
            (*outputs)[_kOut] = v;
            return;
        }
    }
    (*outputs)[_kOut] = Value(defaultVal);
}

static void
_EvalGeomPropValueUniformString(const ParamMap& inputs,
                                const ShadingContext& ctx,
                                NodeOutputMap* outputs)
{
    std::string name = Get<std::string>(inputs, _kGeomprop, std::string());
    std::string defaultVal = Get<std::string>(inputs, _kDefault, std::string());
    if (!name.empty() && ctx.uniformProps) {
        auto it = ctx.uniformProps->find(name);
        if (it != ctx.uniformProps->end() &&
            ValueHolds<std::string>(it->second)) {
            (*outputs)[_kOut] = it->second;
            return;
        }
    }
    (*outputs)[_kOut] = Value(defaultVal);
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

    // geompropvalue (per-sample varying)
    _REG("ND_geompropvalue_integer", &_EvalGeomPropValue<int>);
    _REG("ND_geompropvalue_boolean", &_EvalGeomPropValue<bool>);
    _REG("ND_geompropvalue_float",   &_EvalGeomPropValue<float>);
    _REG("ND_geompropvalue_color3",  &_EvalGeomPropValue<Vec3f>);
    _REG("ND_geompropvalue_color4",  &_EvalGeomPropValue<Vec4f>);
    _REG("ND_geompropvalue_vector2", &_EvalGeomPropValue<Vec2f>);
    _REG("ND_geompropvalue_vector3", &_EvalGeomPropValue<Vec3f>);
    _REG("ND_geompropvalue_vector4", &_EvalGeomPropValue<Vec4f>);

    // geompropvalueuniform (per-mesh uniform)
    _REG("ND_geompropvalueuniform_string",   &_EvalGeomPropValueUniformString);
    _REG("ND_geompropvalueuniform_filename", &_EvalGeomPropValueUniformString);
}

#undef _REG

}  // namespace mxcpp
