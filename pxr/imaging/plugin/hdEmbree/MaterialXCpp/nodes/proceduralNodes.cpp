//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "proceduralNodes.h"
#include "helpers/proceduralHelpers.h"
#include "../nodeRegistry.h"

namespace mxcpp {

static const SlotName _kIn("in");
static const SlotName _kMax("max");
static const SlotName _kMin("min");
static const SlotName _kOut("out");
static const SlotName _kSeed("seed");
static const SlotName _kValue("value");

template<typename T>
static void
_EvalConstant(const ParamMap& inputs, const ShadingContext&,
              NodeOutputMap* outputs)
{
    const T value = Get<T>(inputs, _kValue, Zero<T>());
    (*outputs)[_kOut] = Value(value);
}

template<typename T>
static void
_EvalRandomFloatTyped(const ParamMap& inputs,
                      const ShadingContext&,
                      NodeOutputMap* outputs)
{
    float inputValue = 0.0f;
    if constexpr (std::is_same<T, int>::value) {
        inputValue = static_cast<float>(Get<int>(inputs, _kIn, 0));
    } else {
        inputValue = Get<float>(inputs, _kIn, 0.0f) * 4096.0f;
    }

    StoreTypedOutput(
        outputs,
        _kOut,
        RandomFloatValue(
            inputValue,
            Get<float>(inputs, _kMin, 0.0f),
            Get<float>(inputs, _kMax, 1.0f),
            Get<int>(inputs, _kSeed, 0)));
}

#define _REG(name, fn) reg.Register(name, fn)

void
RegisterProceduralNodes(NodeRegistry& reg)
{
    _REG("ND_constant_float", &_EvalConstant<float>);
    _REG("ND_constant_integer", &_EvalConstant<int>);
    _REG("ND_constant_boolean", &_EvalConstant<bool>);
    _REG("ND_constant_color3", &_EvalConstant<Vec3f>);
    _REG("ND_constant_color4", &_EvalConstant<Vec4f>);
    _REG("ND_constant_vector2", &_EvalConstant<Vec2f>);
    _REG("ND_constant_vector3", &_EvalConstant<Vec3f>);
    _REG("ND_constant_vector4", &_EvalConstant<Vec4f>);

    _REG("ND_randomfloat_float", &_EvalRandomFloatTyped<float>);
    _REG("ND_randomfloat_integer", &_EvalRandomFloatTyped<int>);
}

#undef _REG

}  // namespace mxcpp
