//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "convolutionNodes.h"
#include "../nodeRegistry.h"

#include <iostream>
#include <mutex>

namespace mxcpp {

namespace {

static const SlotName _kIn("in");
static const SlotName _kOut("out");

static void
_WarnBlurPassThroughOnce()
{
    static std::once_flag once;
    std::call_once(once, []() {
        std::cout
            << "hdEmbree MaterialX warning: 'blur' is unsupported for ray "
               "tracing and will pass through 'in' unchanged.\n";
    });
}

template<typename T>
static void
_EvalBlurPassThrough(const ParamMap& inputs,
                     const ShadingContext& ctx,
                     NodeOutputMap* outputs)
{
    _WarnBlurPassThroughOnce();

    Value value;
    if (inputs.Evaluate(_kIn, ctx, &value) && ValueHolds<T>(value)) {
        (*outputs)[_kOut] = value;
        return;
    }

    (*outputs)[_kOut] = Value(Get<T>(inputs, _kIn, Zero<T>()));
}

}  // namespace

#define _REG(name, fn) reg.Register(name, fn)

void
RegisterConvolutionNodes(NodeRegistry& reg)
{
    _REG("ND_blur_float",   &_EvalBlurPassThrough<float>);
    _REG("ND_blur_color3",  &_EvalBlurPassThrough<Vec3f>);
    _REG("ND_blur_color4",  &_EvalBlurPassThrough<Vec4f>);
    _REG("ND_blur_vector2", &_EvalBlurPassThrough<Vec2f>);
    _REG("ND_blur_vector3", &_EvalBlurPassThrough<Vec3f>);
    _REG("ND_blur_vector4", &_EvalBlurPassThrough<Vec4f>);
}

#undef _REG

}  // namespace mxcpp
