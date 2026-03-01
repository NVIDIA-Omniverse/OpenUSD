//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/imaging/plugin/hdEmbree/mxLite/nodes/conditionalNodes.h"
#include "pxr/imaging/plugin/hdEmbree/mxLite/nodeRegistry.h"

#include "pxr/base/tf/staticTokens.h"

PXR_NAMESPACE_OPEN_SCOPE

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
_EvalIfGreater(const MxLiteParamMap& inputs, const MxLiteShadingContext&,
               MxLiteNodeOutputMap* outputs)
{
    float a = MxLiteGet<float>(inputs, _tokens->value1, 0.0f);
    float b = MxLiteGet<float>(inputs, _tokens->value2, 0.0f);
    T in1 = MxLiteGet<T>(inputs, _tokens->in1, MxLiteZero<T>());
    T in2 = MxLiteGet<T>(inputs, _tokens->in2, MxLiteZero<T>());
    (*outputs)[_tokens->out] = VtValue(a > b ? in1 : in2);
}

template<typename T>
static void
_EvalIfGreaterEq(const MxLiteParamMap& inputs, const MxLiteShadingContext&,
                 MxLiteNodeOutputMap* outputs)
{
    float a = MxLiteGet<float>(inputs, _tokens->value1, 0.0f);
    float b = MxLiteGet<float>(inputs, _tokens->value2, 0.0f);
    T in1 = MxLiteGet<T>(inputs, _tokens->in1, MxLiteZero<T>());
    T in2 = MxLiteGet<T>(inputs, _tokens->in2, MxLiteZero<T>());
    (*outputs)[_tokens->out] = VtValue(a >= b ? in1 : in2);
}

template<typename T>
static void
_EvalIfEqual(const MxLiteParamMap& inputs, const MxLiteShadingContext&,
             MxLiteNodeOutputMap* outputs)
{
    float a = MxLiteGet<float>(inputs, _tokens->value1, 0.0f);
    float b = MxLiteGet<float>(inputs, _tokens->value2, 0.0f);
    T in1 = MxLiteGet<T>(inputs, _tokens->in1, MxLiteZero<T>());
    T in2 = MxLiteGet<T>(inputs, _tokens->in2, MxLiteZero<T>());
    (*outputs)[_tokens->out] = VtValue(a == b ? in1 : in2);
}

// Switch selects from up to 10 inputs based on integer index.
template<typename T>
static void
_EvalSwitch(const MxLiteParamMap& inputs, const MxLiteShadingContext&,
            MxLiteNodeOutputMap* outputs)
{
    int which = MxLiteGet<int>(inputs, _tokens->which, 0);
    // Try in1..in10
    static const TfToken inNames[] = {
        TfToken("in1"), TfToken("in2"), TfToken("in3"),
        TfToken("in4"), TfToken("in5"), TfToken("in6"),
        TfToken("in7"), TfToken("in8"), TfToken("in9"),
        TfToken("in10")
    };
    int idx = std::clamp(which, 0, 9);
    T result = MxLiteGet<T>(inputs, inNames[idx], MxLiteZero<T>());
    (*outputs)[_tokens->out] = VtValue(result);
}

// ---- Registration --------------------------------------------------------

#define _REG(name, fn) reg.Register(TfToken(name), fn)

void
MxLiteRegisterConditionalNodes(MxLiteNodeRegistry& reg)
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

PXR_NAMESPACE_CLOSE_SCOPE
