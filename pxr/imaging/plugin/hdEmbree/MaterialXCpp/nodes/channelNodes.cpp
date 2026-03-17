//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "channelNodes.h"
#include "../nodeRegistry.h"

#include <string>

namespace mxcpp {

static const SlotName _kIn("in");
static const SlotName _kIn1("in1");
static const SlotName _kIn2("in2");
static const SlotName _kIn3("in3");
static const SlotName _kIn4("in4");
static const SlotName _kOut("out");
static const SlotName _kOutx("outx");
static const SlotName _kOuty("outy");
static const SlotName _kOutz("outz");
static const SlotName _kOutw("outw");
static const SlotName _kIndex("index");

// ---- Combine -------------------------------------------------------------

static void
_EvalCombine2(const ParamMap& inputs, const ShadingContext&,
              NodeOutputMap* outputs)
{
    float x = Get<float>(inputs, _kIn1, 0.0f);
    float y = Get<float>(inputs, _kIn2, 0.0f);
    (*outputs)[_kOut] = Value(Vec2f(x, y));
}

static void
_EvalCombine3_color3(const ParamMap& inputs, const ShadingContext&,
                     NodeOutputMap* outputs)
{
    float x = Get<float>(inputs, _kIn1, 0.0f);
    float y = Get<float>(inputs, _kIn2, 0.0f);
    float z = Get<float>(inputs, _kIn3, 0.0f);
    (*outputs)[_kOut] = Value(Vec3f(x, y, z));
}

static void
_EvalCombine4(const ParamMap& inputs, const ShadingContext&,
              NodeOutputMap* outputs)
{
    float x = Get<float>(inputs, _kIn1, 0.0f);
    float y = Get<float>(inputs, _kIn2, 0.0f);
    float z = Get<float>(inputs, _kIn3, 0.0f);
    float w = Get<float>(inputs, _kIn4, 0.0f);
    (*outputs)[_kOut] = Value(Vec4f(x, y, z, w));
}

// ---- Separate ------------------------------------------------------------

static void
_EvalSeparate2(const ParamMap& inputs, const ShadingContext&,
               NodeOutputMap* outputs)
{
    Vec2f v = Get<Vec2f>(inputs, _kIn, Vec2f(0.0f));
    (*outputs)[_kOutx] = Value(v[0]);
    (*outputs)[_kOuty] = Value(v[1]);
}

static void
_EvalSeparate3_color3(const ParamMap& inputs, const ShadingContext&,
                      NodeOutputMap* outputs)
{
    Vec3f v = Get<Vec3f>(inputs, _kIn, Vec3f(0.0f));
    (*outputs)[_kOutx] = Value(v[0]);
    (*outputs)[_kOuty] = Value(v[1]);
    (*outputs)[_kOutz] = Value(v[2]);
}

static void
_EvalSeparate4(const ParamMap& inputs, const ShadingContext&,
               NodeOutputMap* outputs)
{
    Vec4f v = Get<Vec4f>(inputs, _kIn, Vec4f(0.0f));
    (*outputs)[_kOutx] = Value(v[0]);
    (*outputs)[_kOuty] = Value(v[1]);
    (*outputs)[_kOutz] = Value(v[2]);
    (*outputs)[_kOutw] = Value(v[3]);
}

// ---- Extract (single channel by index) -----------------------------------

static void
_EvalExtract_vec2(const ParamMap& inputs, const ShadingContext&,
                  NodeOutputMap* outputs)
{
    Vec2f v = Get<Vec2f>(inputs, _kIn, Vec2f(0.0f));
    int idx   = Get<int>(inputs, _kIndex, 0);
    idx = std::clamp(idx, 0, 1);
    (*outputs)[_kOut] = Value(v[idx]);
}

static void
_EvalExtract_vec3(const ParamMap& inputs, const ShadingContext&,
                  NodeOutputMap* outputs)
{
    Vec3f v = Get<Vec3f>(inputs, _kIn, Vec3f(0.0f));
    int idx   = Get<int>(inputs, _kIndex, 0);
    idx = std::clamp(idx, 0, 2);
    (*outputs)[_kOut] = Value(v[idx]);
}

static void
_EvalExtract_vec4(const ParamMap& inputs, const ShadingContext&,
                  NodeOutputMap* outputs)
{
    Vec4f v = Get<Vec4f>(inputs, _kIn, Vec4f(0.0f));
    int idx   = Get<int>(inputs, _kIndex, 0);
    idx = std::clamp(idx, 0, 3);
    (*outputs)[_kOut] = Value(v[idx]);
}

// ---- Convert (type promotion/demotion) -----------------------------------

static void
_EvalConvert_float_color3(const ParamMap& inputs,
                          const ShadingContext&,
                          NodeOutputMap* outputs)
{
    float v = Get<float>(inputs, _kIn, 0.0f);
    (*outputs)[_kOut] = Value(Vec3f(v));
}

static void
_EvalConvert_color3_float(const ParamMap& inputs,
                          const ShadingContext&,
                          NodeOutputMap* outputs)
{
    Vec3f v = Get<Vec3f>(inputs, _kIn, Vec3f(0.0f));
    (*outputs)[_kOut] = Value((v[0] + v[1] + v[2]) / 3.0f);
}

static void
_EvalConvert_color3_vector3(const ParamMap& inputs,
                            const ShadingContext&,
                            NodeOutputMap* outputs)
{
    Vec3f v = Get<Vec3f>(inputs, _kIn, Vec3f(0.0f));
    (*outputs)[_kOut] = Value(v);
}

static void
_EvalConvert_color4_vector4(const ParamMap& inputs,
                            const ShadingContext&,
                            NodeOutputMap* outputs)
{
    Vec4f v = Get<Vec4f>(inputs, _kIn, Vec4f(0.0f));
    (*outputs)[_kOut] = Value(v);
}

static void
_EvalConvert_float_color4(const ParamMap& inputs,
                          const ShadingContext&,
                          NodeOutputMap* outputs)
{
    float v = Get<float>(inputs, _kIn, 0.0f);
    (*outputs)[_kOut] = Value(Vec4f(v, v, v, 1.0f));
}

static void
_EvalConvert_integer_float(const ParamMap& inputs,
                           const ShadingContext&,
                           NodeOutputMap* outputs)
{
    int v = Get<int>(inputs, _kIn, 0);
    (*outputs)[_kOut] = Value(static_cast<float>(v));
}

// ---- Registration --------------------------------------------------------

#define _REG(name, fn) reg.Register(name, fn)

void
RegisterChannelNodes(NodeRegistry& reg)
{
    _REG("ND_combine2_vector2", &_EvalCombine2);
    _REG("ND_combine3_color3",  &_EvalCombine3_color3);
    _REG("ND_combine3_vector3", &_EvalCombine3_color3);
    _REG("ND_combine4_color4",  &_EvalCombine4);
    _REG("ND_combine4_vector4", &_EvalCombine4);

    _REG("ND_separate2_vector2", &_EvalSeparate2);
    _REG("ND_separate3_color3",  &_EvalSeparate3_color3);
    _REG("ND_separate3_vector3", &_EvalSeparate3_color3);
    _REG("ND_separate4_color4",  &_EvalSeparate4);
    _REG("ND_separate4_vector4", &_EvalSeparate4);

    _REG("ND_extract_vector2", &_EvalExtract_vec2);
    _REG("ND_extract_color3",  &_EvalExtract_vec3);
    _REG("ND_extract_vector3", &_EvalExtract_vec3);
    _REG("ND_extract_color4",  &_EvalExtract_vec4);
    _REG("ND_extract_vector4", &_EvalExtract_vec4);

    _REG("ND_convert_float_color3",     &_EvalConvert_float_color3);
    _REG("ND_convert_color3_float",     &_EvalConvert_color3_float);
    _REG("ND_convert_color3_vector3",   &_EvalConvert_color3_vector3);
    _REG("ND_convert_vector3_color3",   &_EvalConvert_color3_vector3);
    _REG("ND_convert_color4_vector4",   &_EvalConvert_color4_vector4);
    _REG("ND_convert_vector4_color4",   &_EvalConvert_color4_vector4);
    _REG("ND_convert_float_color4",     &_EvalConvert_float_color4);
    _REG("ND_convert_integer_float",    &_EvalConvert_integer_float);
}

#undef _REG

} // namespace mxcpp
