//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/imaging/plugin/hdEmbree/MaterialXCpp/nodes/conditionalNodes.h"
#include "pxr/imaging/plugin/hdEmbree/MaterialXCpp/nodeRegistry.h"

#include "pxr/base/tf/staticTokens.h"

PXR_NAMESPACE_OPEN_SCOPE
namespace mxcpp {

TF_DEFINE_PRIVATE_TOKENS(_tokens,
    (value1)
    (value2)
    (in1)
    (in2)
    (out)
    (which)
    (in)
);

// ---- Conditional nodes ---------------------------------------------------

template<typename T>
static void
_EvalIfGreater(const ParamMap& inputs, const ShadingContext&,
               NodeOutputMap* outputs)
{
    float a = Get<float>(inputs, _tokens->value1, 0.0f);
    float b = Get<float>(inputs, _tokens->value2, 0.0f);
    T in1 = Get<T>(inputs, _tokens->in1, Zero<T>());
    T in2 = Get<T>(inputs, _tokens->in2, Zero<T>());
    (*outputs)[_tokens->out] = VtValue(a > b ? in1 : in2);
}

template<typename T>
static void
_EvalIfGreaterEq(const ParamMap& inputs, const ShadingContext&,
                 NodeOutputMap* outputs)
{
    float a = Get<float>(inputs, _tokens->value1, 0.0f);
    float b = Get<float>(inputs, _tokens->value2, 0.0f);
    T in1 = Get<T>(inputs, _tokens->in1, Zero<T>());
    T in2 = Get<T>(inputs, _tokens->in2, Zero<T>());
    (*outputs)[_tokens->out] = VtValue(a >= b ? in1 : in2);
}

template<typename T>
static void
_EvalIfEqual(const ParamMap& inputs, const ShadingContext&,
             NodeOutputMap* outputs)
{
    float a = Get<float>(inputs, _tokens->value1, 0.0f);
    float b = Get<float>(inputs, _tokens->value2, 0.0f);
    T in1 = Get<T>(inputs, _tokens->in1, Zero<T>());
    T in2 = Get<T>(inputs, _tokens->in2, Zero<T>());
    (*outputs)[_tokens->out] = VtValue(a == b ? in1 : in2);
}

// Switch selects from up to 10 inputs based on integer index.
template<typename T>
static void
_EvalSwitch(const ParamMap& inputs, const ShadingContext&,
            NodeOutputMap* outputs)
{
    int which = Get<int>(inputs, _tokens->which, 0);
    // Try in1..in10
    static const TfToken inNames[] = {
        TfToken("in1"), TfToken("in2"), TfToken("in3"),
        TfToken("in4"), TfToken("in5"), TfToken("in6"),
        TfToken("in7"), TfToken("in8"), TfToken("in9"),
        TfToken("in10")
    };
    int idx = std::clamp(which, 0, 9);
    T result = Get<T>(inputs, inNames[idx], Zero<T>());
    (*outputs)[_tokens->out] = VtValue(result);
}

// ---- Registration --------------------------------------------------------

#define _REG(name, fn) reg.Register(TfToken(name), fn)

void
RegisterConditionalNodes(NodeRegistry& reg)
{
    _REG("ND_ifgreater_float",   &_EvalIfGreater<float>);
    _REG("ND_ifgreater_color3",  &_EvalIfGreater<GfVec3f>);
    _REG("ND_ifgreater_color4",  &_EvalIfGreater<GfVec4f>);
    _REG("ND_ifgreater_vector3", &_EvalIfGreater<GfVec3f>);

    _REG("ND_ifgreatereq_float",   &_EvalIfGreaterEq<float>);
    _REG("ND_ifgreatereq_color3",  &_EvalIfGreaterEq<GfVec3f>);
    _REG("ND_ifgreatereq_vector3", &_EvalIfGreaterEq<GfVec3f>);

    _REG("ND_ifequal_float",   &_EvalIfEqual<float>);
    _REG("ND_ifequal_color3",  &_EvalIfEqual<GfVec3f>);
    _REG("ND_ifequal_vector3", &_EvalIfEqual<GfVec3f>);

    _REG("ND_switch_float",   &_EvalSwitch<float>);
    _REG("ND_switch_color3",  &_EvalSwitch<GfVec3f>);
    _REG("ND_switch_vector3", &_EvalSwitch<GfVec3f>);
}

#undef _REG

} // namespace mxcpp
PXR_NAMESPACE_CLOSE_SCOPE
