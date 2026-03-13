//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "conditionalNodes.h"
#include "../nodeRegistry.h"

#include <string>

namespace mxcpp {

static const std::string _kValue1 = "value1";
static const std::string _kValue2 = "value2";
static const std::string _kIn1 = "in1";
static const std::string _kIn2 = "in2";
static const std::string _kOut = "out";
static const std::string _kWhich = "which";
static const std::string _kIn = "in";

// ---- Conditional nodes ---------------------------------------------------

template<typename T>
static void
_EvalIfGreater(const ParamMap& inputs, const ShadingContext&,
               NodeOutputMap* outputs)
{
    float a = Get<float>(inputs, _kValue1, 0.0f);
    float b = Get<float>(inputs, _kValue2, 0.0f);
    T in1 = Get<T>(inputs, _kIn1, Zero<T>());
    T in2 = Get<T>(inputs, _kIn2, Zero<T>());
    (*outputs)[_kOut] = Value(a > b ? in1 : in2);
}

template<typename T>
static void
_EvalIfGreaterEq(const ParamMap& inputs, const ShadingContext&,
                 NodeOutputMap* outputs)
{
    float a = Get<float>(inputs, _kValue1, 0.0f);
    float b = Get<float>(inputs, _kValue2, 0.0f);
    T in1 = Get<T>(inputs, _kIn1, Zero<T>());
    T in2 = Get<T>(inputs, _kIn2, Zero<T>());
    (*outputs)[_kOut] = Value(a >= b ? in1 : in2);
}

template<typename T>
static void
_EvalIfEqual(const ParamMap& inputs, const ShadingContext&,
             NodeOutputMap* outputs)
{
    float a = Get<float>(inputs, _kValue1, 0.0f);
    float b = Get<float>(inputs, _kValue2, 0.0f);
    T in1 = Get<T>(inputs, _kIn1, Zero<T>());
    T in2 = Get<T>(inputs, _kIn2, Zero<T>());
    (*outputs)[_kOut] = Value(a == b ? in1 : in2);
}

// Switch selects from up to 10 inputs based on integer index.
template<typename T>
static void
_EvalSwitch(const ParamMap& inputs, const ShadingContext&,
            NodeOutputMap* outputs)
{
    int which = Get<int>(inputs, _kWhich, 0);
    // Try in1..in10
    static const std::string inNames[] = {
        "in1", "in2", "in3",
        "in4", "in5", "in6",
        "in7", "in8", "in9",
        "in10"
    };
    int idx = std::clamp(which, 0, 9);
    T result = Get<T>(inputs, inNames[idx], Zero<T>());
    (*outputs)[_kOut] = Value(result);
}

// ---- Registration --------------------------------------------------------

#define _REG(name, fn) reg.Register(name, fn)

void
RegisterConditionalNodes(NodeRegistry& reg)
{
    _REG("ND_ifgreater_float",   &_EvalIfGreater<float>);
    _REG("ND_ifgreater_color3",  &_EvalIfGreater<Vec3f>);
    _REG("ND_ifgreater_color4",  &_EvalIfGreater<Vec4f>);
    _REG("ND_ifgreater_vector3", &_EvalIfGreater<Vec3f>);

    _REG("ND_ifgreatereq_float",   &_EvalIfGreaterEq<float>);
    _REG("ND_ifgreatereq_color3",  &_EvalIfGreaterEq<Vec3f>);
    _REG("ND_ifgreatereq_vector3", &_EvalIfGreaterEq<Vec3f>);

    _REG("ND_ifequal_float",   &_EvalIfEqual<float>);
    _REG("ND_ifequal_color3",  &_EvalIfEqual<Vec3f>);
    _REG("ND_ifequal_vector3", &_EvalIfEqual<Vec3f>);

    _REG("ND_switch_float",   &_EvalSwitch<float>);
    _REG("ND_switch_color3",  &_EvalSwitch<Vec3f>);
    _REG("ND_switch_vector3", &_EvalSwitch<Vec3f>);
}

#undef _REG

} // namespace mxcpp
