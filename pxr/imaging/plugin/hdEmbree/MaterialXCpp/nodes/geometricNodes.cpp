//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "geometricNodes.h"
#include "helpers/inputEvaluationHelpers.h"
#include "helpers/shadingContextHelpers.h"
#include "../nodeRegistry.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace mxcpp {

static constexpr float _kFloatEps = 1e-6f;

static const SlotName _kIn("in");
static const SlotName _kHeight("height");
static const SlotName _kScale("scale");
static const SlotName _kTexcoord("texcoord");
static const SlotName _kNormal("normal");
static const SlotName _kTangent("tangent");
static const SlotName _kBitangent("bitangent");
static const SlotName _kOut("out");
static const SlotName _kGeomprop("geomprop");
static const SlotName _kDefault("default");
static const SlotName _kSpace("space");

static std::string
_GetSpace(const ParamMap& inputs)
{
    return NormalizeSpaceName(
        Get<std::string>(inputs, _kSpace, std::string("object")),
        std::string("object"));
}

static float
_SelectFiniteDifferenceStep(float dx, float dy)
{
    float step = 0.5f * (std::abs(dx) + std::abs(dy));
    if (step < 5.0e-4f) {
        step = 5.0e-4f;
    }
    return step;
}

static void
_BuildOrthonormalBasis(const Vec3f& normal,
                       const Vec3f& tangent,
                       const Vec3f& bitangent,
                       Vec3f* outNormal,
                       Vec3f* outTangent,
                       Vec3f* outBitangent)
{
    Vec3f n = normal;
    if (Dot(n, n) < _kFloatEps * _kFloatEps) {
        n = Vec3f(0.0f, 0.0f, 1.0f);
    } else {
        n.normalize();
    }

    Vec3f t = tangent - n * Dot(tangent, n);
    if (Dot(t, t) < _kFloatEps * _kFloatEps) {
        t = Cross(Vec3f(0.0f, 1.0f, 0.0f), n);
        if (Dot(t, t) < _kFloatEps * _kFloatEps) {
            t = Cross(Vec3f(1.0f, 0.0f, 0.0f), n);
        }
    }
    t.normalize();

    Vec3f b = bitangent - n * Dot(bitangent, n) - t * Dot(bitangent, t);
    if (Dot(b, b) < _kFloatEps * _kFloatEps) {
        b = Cross(n, t);
    }
    b.normalize();

    if (outNormal) {
        *outNormal = n;
    }
    if (outTangent) {
        *outTangent = t;
    }
    if (outBitangent) {
        *outBitangent = b;
    }
}

// Geometric nodes read from the shading context.

static void
_EvalPosition(const ParamMap& inputs, const ShadingContext& ctx,
              NodeOutputMap* outputs)
{
    const std::string space = _GetSpace(inputs);
    Vec3f result = ctx.position;
    TransformNamedVec3(
        ctx, "object", space,
        ShadingContext::TransformSpaceType::Point,
        ctx.position, &result);
    (*outputs)[_kOut] = Value(result);
}

static void
_EvalNormal(const ParamMap& inputs, const ShadingContext& ctx,
            NodeOutputMap* outputs)
{
    const std::string space = _GetSpace(inputs);
    Vec3f result = ctx.normal;
    TransformNamedVec3(
        ctx, "world", space,
        ShadingContext::TransformSpaceType::Normal,
        ctx.normal, &result);
    (*outputs)[_kOut] = Value(result);
}

static void
_EvalTangent(const ParamMap& inputs, const ShadingContext& ctx,
             NodeOutputMap* outputs)
{
    const std::string space = _GetSpace(inputs);
    Vec3f result = ctx.tangent;
    TransformNamedVec3(
        ctx, "world", space,
        ShadingContext::TransformSpaceType::Vector,
        ctx.tangent, &result);
    (*outputs)[_kOut] = Value(result);
}

static void
_EvalBitangent(const ParamMap& inputs, const ShadingContext& ctx,
               NodeOutputMap* outputs)
{
    const std::string space = _GetSpace(inputs);
    Vec3f result = ctx.bitangent;
    TransformNamedVec3(
        ctx, "world", space,
        ShadingContext::TransformSpaceType::Vector,
        ctx.bitangent, &result);
    (*outputs)[_kOut] = Value(result);
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

static void
_EvalBump(const ParamMap& inputs, const ShadingContext& ctx,
          NodeOutputMap* outputs)
{
    const float scale = Get<float>(inputs, _kScale, 1.0f);
    const float height = EvaluateInput<float>(inputs, _kHeight, ctx, 0.0f);
    const float du = _SelectFiniteDifferenceStep(ctx.dudx, ctx.dudy);
    const float dv = _SelectFiniteDifferenceStep(ctx.dvdx, ctx.dvdy);

    const float heightDu = EvaluateInput<float>(
        inputs, _kHeight, OffsetContextDu(ctx, du), 0.0f);
    const float heightDv = EvaluateInput<float>(
        inputs, _kHeight, OffsetContextDv(ctx, dv), 0.0f);

    const Vec3f inputNormal = Get<Vec3f>(inputs, _kNormal, ctx.normal);
    const Vec3f inputTangent = Get<Vec3f>(inputs, _kTangent, ctx.tangent);
    const Vec3f inputBitangent = Get<Vec3f>(inputs, _kBitangent, ctx.bitangent);

    Vec3f basisNormal;
    Vec3f basisTangent;
    Vec3f basisBitangent;
    _BuildOrthonormalBasis(
        inputNormal, inputTangent, inputBitangent,
        &basisNormal, &basisTangent, &basisBitangent);

    float tangentLength = std::sqrt(Dot(ctx.dPdu, ctx.dPdu));
    if (tangentLength < _kFloatEps) {
        tangentLength = 1.0f;
    }
    float bitangentLength = std::sqrt(Dot(ctx.dPdv, ctx.dPdv));
    if (bitangentLength < _kFloatEps) {
        bitangentLength = 1.0f;
    }

    const float dHdu = (heightDu - height) * scale / du;
    const float dHdv = (heightDv - height) * scale / dv;

    const Vec3f dpdu = basisTangent * tangentLength + basisNormal * dHdu;
    const Vec3f dpdv = basisBitangent * bitangentLength + basisNormal * dHdv;

    Vec3f worldNormal = Cross(dpdu, dpdv);
    if (Dot(worldNormal, worldNormal) < _kFloatEps * _kFloatEps) {
        (*outputs)[_kOut] = Value(Vec3f(0.0f, 0.0f, 1.0f));
        return;
    }
    worldNormal.normalize();
    if (Dot(worldNormal, basisNormal) < 0.0f) {
        worldNormal = -worldNormal;
    }

    Vec3f tangentSpaceNormal(
        Dot(worldNormal, ctx.tangent),
        Dot(worldNormal, ctx.bitangent),
        Dot(worldNormal, ctx.normal));
    if (Dot(tangentSpaceNormal, tangentSpaceNormal) <
        _kFloatEps * _kFloatEps) {
        (*outputs)[_kOut] = Value(Vec3f(0.0f, 0.0f, 1.0f));
        return;
    }
    tangentSpaceNormal.normalize();
    (*outputs)[_kOut] = Value(tangentSpaceNormal);
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
    _REG("ND_bump_vector3", &_EvalBump);

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
