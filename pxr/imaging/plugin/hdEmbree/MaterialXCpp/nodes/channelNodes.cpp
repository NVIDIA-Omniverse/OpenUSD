//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/imaging/plugin/hdEmbree/MaterialXCpp/nodes/channelNodes.h"
#include "pxr/imaging/plugin/hdEmbree/MaterialXCpp/nodeRegistry.h"

#include "pxr/base/tf/staticTokens.h"

PXR_NAMESPACE_OPEN_SCOPE
namespace mxcpp {

TF_DEFINE_PRIVATE_TOKENS(_tokens,
    (in)
    (in1)
    (in2)
    (in3)
    (in4)
    (out)
    (outx)
    (outy)
    (outz)
    (outw)
    (index)
);

// ---- Combine -------------------------------------------------------------

static void
_EvalCombine2(const ParamMap& inputs, const ShadingContext&,
              NodeOutputMap* outputs)
{
    float x = Get<float>(inputs, _tokens->in1, 0.0f);
    float y = Get<float>(inputs, _tokens->in2, 0.0f);
    (*outputs)[_tokens->out] = VtValue(GfVec2f(x, y));
}

static void
_EvalCombine3_color3(const ParamMap& inputs, const ShadingContext&,
                     NodeOutputMap* outputs)
{
    float x = Get<float>(inputs, _tokens->in1, 0.0f);
    float y = Get<float>(inputs, _tokens->in2, 0.0f);
    float z = Get<float>(inputs, _tokens->in3, 0.0f);
    (*outputs)[_tokens->out] = VtValue(GfVec3f(x, y, z));
}

static void
_EvalCombine4(const ParamMap& inputs, const ShadingContext&,
              NodeOutputMap* outputs)
{
    float x = Get<float>(inputs, _tokens->in1, 0.0f);
    float y = Get<float>(inputs, _tokens->in2, 0.0f);
    float z = Get<float>(inputs, _tokens->in3, 0.0f);
    float w = Get<float>(inputs, _tokens->in4, 0.0f);
    (*outputs)[_tokens->out] = VtValue(GfVec4f(x, y, z, w));
}

// ---- Separate ------------------------------------------------------------

static void
_EvalSeparate2(const ParamMap& inputs, const ShadingContext&,
               NodeOutputMap* outputs)
{
    GfVec2f v = Get<GfVec2f>(inputs, _tokens->in, GfVec2f(0.0f));
    (*outputs)[_tokens->outx] = VtValue(v[0]);
    (*outputs)[_tokens->outy] = VtValue(v[1]);
}

static void
_EvalSeparate3_color3(const ParamMap& inputs, const ShadingContext&,
                      NodeOutputMap* outputs)
{
    GfVec3f v = Get<GfVec3f>(inputs, _tokens->in, GfVec3f(0.0f));
    (*outputs)[_tokens->outx] = VtValue(v[0]);
    (*outputs)[_tokens->outy] = VtValue(v[1]);
    (*outputs)[_tokens->outz] = VtValue(v[2]);
}

static void
_EvalSeparate4(const ParamMap& inputs, const ShadingContext&,
               NodeOutputMap* outputs)
{
    GfVec4f v = Get<GfVec4f>(inputs, _tokens->in, GfVec4f(0.0f));
    (*outputs)[_tokens->outx] = VtValue(v[0]);
    (*outputs)[_tokens->outy] = VtValue(v[1]);
    (*outputs)[_tokens->outz] = VtValue(v[2]);
    (*outputs)[_tokens->outw] = VtValue(v[3]);
}

// ---- Extract (single channel by index) -----------------------------------

static void
_EvalExtract_vec2(const ParamMap& inputs, const ShadingContext&,
                  NodeOutputMap* outputs)
{
    GfVec2f v = Get<GfVec2f>(inputs, _tokens->in, GfVec2f(0.0f));
    int idx   = Get<int>(inputs, _tokens->index, 0);
    idx = std::clamp(idx, 0, 1);
    (*outputs)[_tokens->out] = VtValue(v[idx]);
}

static void
_EvalExtract_vec3(const ParamMap& inputs, const ShadingContext&,
                  NodeOutputMap* outputs)
{
    GfVec3f v = Get<GfVec3f>(inputs, _tokens->in, GfVec3f(0.0f));
    int idx   = Get<int>(inputs, _tokens->index, 0);
    idx = std::clamp(idx, 0, 2);
    (*outputs)[_tokens->out] = VtValue(v[idx]);
}

static void
_EvalExtract_vec4(const ParamMap& inputs, const ShadingContext&,
                  NodeOutputMap* outputs)
{
    GfVec4f v = Get<GfVec4f>(inputs, _tokens->in, GfVec4f(0.0f));
    int idx   = Get<int>(inputs, _tokens->index, 0);
    idx = std::clamp(idx, 0, 3);
    (*outputs)[_tokens->out] = VtValue(v[idx]);
}

// ---- Convert (type promotion/demotion) -----------------------------------

static void
_EvalConvert_float_color3(const ParamMap& inputs,
                          const ShadingContext&,
                          NodeOutputMap* outputs)
{
    float v = Get<float>(inputs, _tokens->in, 0.0f);
    (*outputs)[_tokens->out] = VtValue(GfVec3f(v));
}

static void
_EvalConvert_color3_float(const ParamMap& inputs,
                          const ShadingContext&,
                          NodeOutputMap* outputs)
{
    GfVec3f v = Get<GfVec3f>(inputs, _tokens->in, GfVec3f(0.0f));
    (*outputs)[_tokens->out] = VtValue((v[0] + v[1] + v[2]) / 3.0f);
}

static void
_EvalConvert_color3_vector3(const ParamMap& inputs,
                            const ShadingContext&,
                            NodeOutputMap* outputs)
{
    GfVec3f v = Get<GfVec3f>(inputs, _tokens->in, GfVec3f(0.0f));
    (*outputs)[_tokens->out] = VtValue(v);
}

static void
_EvalConvert_color4_vector4(const ParamMap& inputs,
                            const ShadingContext&,
                            NodeOutputMap* outputs)
{
    GfVec4f v = Get<GfVec4f>(inputs, _tokens->in, GfVec4f(0.0f));
    (*outputs)[_tokens->out] = VtValue(v);
}

static void
_EvalConvert_float_color4(const ParamMap& inputs,
                          const ShadingContext&,
                          NodeOutputMap* outputs)
{
    float v = Get<float>(inputs, _tokens->in, 0.0f);
    (*outputs)[_tokens->out] = VtValue(GfVec4f(v, v, v, 1.0f));
}

static void
_EvalConvert_integer_float(const ParamMap& inputs,
                           const ShadingContext&,
                           NodeOutputMap* outputs)
{
    int v = Get<int>(inputs, _tokens->in, 0);
    (*outputs)[_tokens->out] = VtValue(static_cast<float>(v));
}

// ---- Registration --------------------------------------------------------

#define _REG(name, fn) reg.Register(TfToken(name), fn)

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
PXR_NAMESPACE_CLOSE_SCOPE
