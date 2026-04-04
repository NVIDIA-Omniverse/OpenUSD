//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "convolutionNodes.h"
#include "helpers/inputEvaluationHelpers.h"
#include "helpers/mathHelpers.h"
#include "helpers/shadingContextHelpers.h"
#include "../nodeRegistry.h"

#include <cmath>
#include <iostream>
#include <mutex>

namespace mxcpp {

namespace {

static const SlotName _kIn("in");
static const SlotName _kOut("out");
static const SlotName _kScale("scale");
static const SlotName _kTexcoord("texcoord");

static constexpr float _kFloatEps = 1e-6f;
static constexpr float _kSobelScaleFactor = 1.0f / 16.0f;

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

static Vec3f
_ComputeHeightToNormalEncoded(const ParamMap& inputs,
                              const SlotName& heightSlot,
                              const ShadingContext& ctx)
{
    const float scale = Get<float>(inputs, _kScale, 1.0f);
    const Vec2f texcoord =
        EvaluateInput<Vec2f>(inputs, _kTexcoord, ctx, ctx.texcoord);

    const ShadingContext shiftedDx = OffsetContextDx(ctx);
    const ShadingContext shiftedDy = OffsetContextDy(ctx);

    const float height =
        EvaluateInput<float>(inputs, heightSlot, ctx, 0.0f);
    const float heightDx =
        EvaluateInput<float>(inputs, heightSlot, shiftedDx, 0.0f) - height;
    const float heightDy =
        EvaluateInput<float>(inputs, heightSlot, shiftedDy, 0.0f) - height;

    const Vec2f texcoordDx =
        EvaluateInput<Vec2f>(inputs, _kTexcoord, shiftedDx, shiftedDx.texcoord) -
        texcoord;
    const Vec2f texcoordDy =
        EvaluateInput<Vec2f>(inputs, _kTexcoord, shiftedDy, shiftedDy.texcoord) -
        texcoord;

    const Vec2f dHdS =
        Vec2f(heightDx, heightDy) * scale * _kSobelScaleFactor;
    const Vec2f dUdS(texcoordDx[0], texcoordDy[0]);
    const Vec2f dVdS(texcoordDx[1], texcoordDy[1]);

    const Vec3f tangent(dUdS[0], dVdS[0], dHdS[0]);
    const Vec3f bitangent(dUdS[1], dVdS[1], dHdS[1]);
    Vec3f normal = Cross(tangent, bitangent);

    if (Dot(normal, normal) < _kFloatEps * _kFloatEps) {
        normal = Vec3f(0.0f, 0.0f, 1.0f);
    } else {
        if (normal[2] < 0.0f) {
            normal = -normal;
        }
        normal.normalize();
    }

    return normal * 0.5f + Vec3f(0.5f, 0.5f, 0.5f);
}

static void
_EvalHeightToNormal(const ParamMap& inputs, const ShadingContext& ctx,
                    NodeOutputMap* outputs)
{
    (*outputs)[_kOut] = Value(
        _ComputeHeightToNormalEncoded(inputs, _kIn, ctx));
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
    _REG("ND_heighttonormal_vector3", &_EvalHeightToNormal);
}

#undef _REG

}  // namespace mxcpp
