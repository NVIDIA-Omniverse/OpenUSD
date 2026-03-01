//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/imaging/plugin/hdEmbree/mxLite/nodes/mathNodes.h"
#include "pxr/imaging/plugin/hdEmbree/mxLite/nodeRegistry.h"

#include "pxr/base/tf/staticTokens.h"

#include <cmath>
#include <algorithm>

PXR_NAMESPACE_OPEN_SCOPE

TF_DEFINE_PRIVATE_TOKENS(_tokens,
    (in)
    (in1)
    (in2)
    (out)
    (power)
    (value)
    (amount)
);

// ---- Arithmetic templates ------------------------------------------------

template<typename T>
static void
_EvalAdd(const MxLiteParamMap& inputs, const MxLiteShadingContext&,
         MxLiteNodeOutputMap* outputs)
{
    T a = MxLiteGet<T>(inputs, _tokens->in1, MxLiteZero<T>());
    T b = MxLiteGet<T>(inputs, _tokens->in2, MxLiteZero<T>());
    (*outputs)[_tokens->out] = VtValue(a + b);
}

template<typename T>
static void
_EvalSubtract(const MxLiteParamMap& inputs, const MxLiteShadingContext&,
              MxLiteNodeOutputMap* outputs)
{
    T a = MxLiteGet<T>(inputs, _tokens->in1, MxLiteZero<T>());
    T b = MxLiteGet<T>(inputs, _tokens->in2, MxLiteZero<T>());
    (*outputs)[_tokens->out] = VtValue(a - b);
}

template<typename T>
static void
_EvalMultiply(const MxLiteParamMap& inputs, const MxLiteShadingContext&,
              MxLiteNodeOutputMap* outputs)
{
    T a = MxLiteGet<T>(inputs, _tokens->in1, MxLiteOne<T>());
    T b = MxLiteGet<T>(inputs, _tokens->in2, MxLiteOne<T>());
    (*outputs)[_tokens->out] = VtValue(MxLiteCompMul(a, b));
}

// Multiply vector by scalar (FA = float argument)
template<typename T>
static void
_EvalMultiplyFA(const MxLiteParamMap& inputs, const MxLiteShadingContext&,
                MxLiteNodeOutputMap* outputs)
{
    T a = MxLiteGet<T>(inputs, _tokens->in1, MxLiteOne<T>());
    float b = MxLiteGet<float>(inputs, _tokens->in2, 1.0f);
    (*outputs)[_tokens->out] = VtValue(a * b);
}

template<typename T>
static void
_EvalDivide(const MxLiteParamMap& inputs, const MxLiteShadingContext&,
            MxLiteNodeOutputMap* outputs)
{
    T a = MxLiteGet<T>(inputs, _tokens->in1, MxLiteZero<T>());
    T b = MxLiteGet<T>(inputs, _tokens->in2, MxLiteOne<T>());
    (*outputs)[_tokens->out] = VtValue(MxLiteCompDiv(a, b));
}

template<typename T>
static void
_EvalDivideFA(const MxLiteParamMap& inputs, const MxLiteShadingContext&,
              MxLiteNodeOutputMap* outputs)
{
    T a = MxLiteGet<T>(inputs, _tokens->in1, MxLiteZero<T>());
    float b = MxLiteGet<float>(inputs, _tokens->in2, 1.0f);
    (*outputs)[_tokens->out] = VtValue(b != 0.0f ? a / b : MxLiteZero<T>());
}

static void
_EvalModulo(const MxLiteParamMap& inputs, const MxLiteShadingContext&,
            MxLiteNodeOutputMap* outputs)
{
    float a = MxLiteGet<float>(inputs, _tokens->in1, 0.0f);
    float b = MxLiteGet<float>(inputs, _tokens->in2, 1.0f);
    (*outputs)[_tokens->out] = VtValue(b != 0.0f ? std::fmod(a, b) : 0.0f);
}

// ---- Unary math ----------------------------------------------------------

static void
_EvalAbsvalFloat(const MxLiteParamMap& inputs, const MxLiteShadingContext&,
                 MxLiteNodeOutputMap* outputs)
{
    float v = MxLiteGet<float>(inputs, _tokens->in, 0.0f);
    (*outputs)[_tokens->out] = VtValue(std::fabs(v));
}

static void
_EvalAbsvalVec3(const MxLiteParamMap& inputs, const MxLiteShadingContext&,
                MxLiteNodeOutputMap* outputs)
{
    GfVec3f v = MxLiteGet<GfVec3f>(inputs, _tokens->in, GfVec3f(0.0f));
    (*outputs)[_tokens->out] = VtValue(
        GfVec3f(std::fabs(v[0]), std::fabs(v[1]), std::fabs(v[2])));
}

static void
_EvalSign(const MxLiteParamMap& inputs, const MxLiteShadingContext&,
          MxLiteNodeOutputMap* outputs)
{
    float v = MxLiteGet<float>(inputs, _tokens->in, 0.0f);
    (*outputs)[_tokens->out] = VtValue(v > 0.0f ? 1.0f : (v < 0.0f ? -1.0f : 0.0f));
}

static void
_EvalFloor(const MxLiteParamMap& inputs, const MxLiteShadingContext&,
           MxLiteNodeOutputMap* outputs)
{
    float v = MxLiteGet<float>(inputs, _tokens->in, 0.0f);
    (*outputs)[_tokens->out] = VtValue(std::floor(v));
}

static void
_EvalCeil(const MxLiteParamMap& inputs, const MxLiteShadingContext&,
          MxLiteNodeOutputMap* outputs)
{
    float v = MxLiteGet<float>(inputs, _tokens->in, 0.0f);
    (*outputs)[_tokens->out] = VtValue(std::ceil(v));
}

static void
_EvalRound(const MxLiteParamMap& inputs, const MxLiteShadingContext&,
           MxLiteNodeOutputMap* outputs)
{
    float v = MxLiteGet<float>(inputs, _tokens->in, 0.0f);
    (*outputs)[_tokens->out] = VtValue(std::round(v));
}

static void
_EvalPower(const MxLiteParamMap& inputs, const MxLiteShadingContext&,
           MxLiteNodeOutputMap* outputs)
{
    float base = MxLiteGet<float>(inputs, _tokens->in1, 0.0f);
    float exp  = MxLiteGet<float>(inputs, _tokens->in2, 1.0f);
    (*outputs)[_tokens->out] = VtValue(std::pow(base, exp));
}

static void
_EvalSqrt(const MxLiteParamMap& inputs, const MxLiteShadingContext&,
          MxLiteNodeOutputMap* outputs)
{
    float v = MxLiteGet<float>(inputs, _tokens->in, 0.0f);
    (*outputs)[_tokens->out] = VtValue(std::sqrt(std::max(0.0f, v)));
}

static void
_EvalLn(const MxLiteParamMap& inputs, const MxLiteShadingContext&,
        MxLiteNodeOutputMap* outputs)
{
    float v = MxLiteGet<float>(inputs, _tokens->in, 1.0f);
    (*outputs)[_tokens->out] = VtValue(v > 0.0f ? std::log(v) : 0.0f);
}

static void
_EvalExp(const MxLiteParamMap& inputs, const MxLiteShadingContext&,
         MxLiteNodeOutputMap* outputs)
{
    float v = MxLiteGet<float>(inputs, _tokens->in, 0.0f);
    (*outputs)[_tokens->out] = VtValue(std::exp(v));
}

template<typename T>
static void
_EvalNegate(const MxLiteParamMap& inputs, const MxLiteShadingContext&,
            MxLiteNodeOutputMap* outputs)
{
    T v = MxLiteGet<T>(inputs, _tokens->in, MxLiteZero<T>());
    (*outputs)[_tokens->out] = VtValue(-v);
}

template<typename T>
static void
_EvalInvert(const MxLiteParamMap& inputs, const MxLiteShadingContext&,
            MxLiteNodeOutputMap* outputs)
{
    T v = MxLiteGet<T>(inputs, _tokens->in, MxLiteZero<T>());
    T amount = MxLiteGet<T>(inputs, _tokens->amount, MxLiteOne<T>());
    (*outputs)[_tokens->out] = VtValue(amount - v);
}

// Scalar specialization for invert
template<>
void
_EvalInvert<float>(const MxLiteParamMap& inputs, const MxLiteShadingContext&,
                   MxLiteNodeOutputMap* outputs)
{
    float v = MxLiteGet<float>(inputs, _tokens->in, 0.0f);
    float amount = MxLiteGet<float>(inputs, _tokens->amount, 1.0f);
    (*outputs)[_tokens->out] = VtValue(amount - v);
}

// ---- Trigonometric -------------------------------------------------------

static void _EvalSin(const MxLiteParamMap& in, const MxLiteShadingContext&,
                     MxLiteNodeOutputMap* out) {
    (*out)[_tokens->out] = VtValue(std::sin(MxLiteGet<float>(in, _tokens->in, 0.0f)));
}
static void _EvalCos(const MxLiteParamMap& in, const MxLiteShadingContext&,
                     MxLiteNodeOutputMap* out) {
    (*out)[_tokens->out] = VtValue(std::cos(MxLiteGet<float>(in, _tokens->in, 0.0f)));
}
static void _EvalTan(const MxLiteParamMap& in, const MxLiteShadingContext&,
                     MxLiteNodeOutputMap* out) {
    (*out)[_tokens->out] = VtValue(std::tan(MxLiteGet<float>(in, _tokens->in, 0.0f)));
}
static void _EvalAsin(const MxLiteParamMap& in, const MxLiteShadingContext&,
                      MxLiteNodeOutputMap* out) {
    float v = std::clamp(MxLiteGet<float>(in, _tokens->in, 0.0f), -1.0f, 1.0f);
    (*out)[_tokens->out] = VtValue(std::asin(v));
}
static void _EvalAcos(const MxLiteParamMap& in, const MxLiteShadingContext&,
                      MxLiteNodeOutputMap* out) {
    float v = std::clamp(MxLiteGet<float>(in, _tokens->in, 0.0f), -1.0f, 1.0f);
    (*out)[_tokens->out] = VtValue(std::acos(v));
}
static void _EvalAtan2(const MxLiteParamMap& in, const MxLiteShadingContext&,
                       MxLiteNodeOutputMap* out) {
    float y = MxLiteGet<float>(in, _tokens->in1, 0.0f);
    float x = MxLiteGet<float>(in, _tokens->in2, 1.0f);
    (*out)[_tokens->out] = VtValue(std::atan2(y, x));
}

// ---- Vector operations ---------------------------------------------------

static void
_EvalDotProduct(const MxLiteParamMap& inputs, const MxLiteShadingContext&,
                MxLiteNodeOutputMap* outputs)
{
    GfVec3f a = MxLiteGet<GfVec3f>(inputs, _tokens->in1, GfVec3f(0.0f));
    GfVec3f b = MxLiteGet<GfVec3f>(inputs, _tokens->in2, GfVec3f(0.0f));
    (*outputs)[_tokens->out] = VtValue(GfDot(a, b));
}

static void
_EvalCrossProduct(const MxLiteParamMap& inputs, const MxLiteShadingContext&,
                  MxLiteNodeOutputMap* outputs)
{
    GfVec3f a = MxLiteGet<GfVec3f>(inputs, _tokens->in1, GfVec3f(0.0f));
    GfVec3f b = MxLiteGet<GfVec3f>(inputs, _tokens->in2, GfVec3f(0.0f));
    (*outputs)[_tokens->out] = VtValue(GfCross(a, b));
}

static void
_EvalNormalize(const MxLiteParamMap& inputs, const MxLiteShadingContext&,
               MxLiteNodeOutputMap* outputs)
{
    GfVec3f v = MxLiteGet<GfVec3f>(inputs, _tokens->in, GfVec3f(0.0f, 0.0f, 1.0f));
    float len = v.GetLength();
    (*outputs)[_tokens->out] = VtValue(len > 0.0f ? v / len : GfVec3f(0.0f));
}

static void
_EvalMagnitude(const MxLiteParamMap& inputs, const MxLiteShadingContext&,
               MxLiteNodeOutputMap* outputs)
{
    GfVec3f v = MxLiteGet<GfVec3f>(inputs, _tokens->in, GfVec3f(0.0f));
    (*outputs)[_tokens->out] = VtValue(v.GetLength());
}

// ---- Constant node -------------------------------------------------------

template<typename T>
static void
_EvalConstant(const MxLiteParamMap& inputs, const MxLiteShadingContext&,
              MxLiteNodeOutputMap* outputs)
{
    T v = MxLiteGet<T>(inputs, _tokens->value, MxLiteZero<T>());
    (*outputs)[_tokens->out] = VtValue(v);
}

// ---- Registration --------------------------------------------------------

#define _REG(name, fn) reg.Register(TfToken(name), fn)

void
MxLiteRegisterMathNodes(MxLiteNodeRegistry& reg)
{
    // add
    _REG("ND_add_float",   &_EvalAdd<float>);
    _REG("ND_add_color3",  &_EvalAdd<GfVec3f>);
    _REG("ND_add_color4",  &_EvalAdd<GfVec4f>);
    _REG("ND_add_vector2", &_EvalAdd<GfVec2f>);
    _REG("ND_add_vector3", &_EvalAdd<GfVec3f>);
    _REG("ND_add_vector4", &_EvalAdd<GfVec4f>);

    // subtract
    _REG("ND_subtract_float",   &_EvalSubtract<float>);
    _REG("ND_subtract_color3",  &_EvalSubtract<GfVec3f>);
    _REG("ND_subtract_color4",  &_EvalSubtract<GfVec4f>);
    _REG("ND_subtract_vector2", &_EvalSubtract<GfVec2f>);
    _REG("ND_subtract_vector3", &_EvalSubtract<GfVec3f>);
    _REG("ND_subtract_vector4", &_EvalSubtract<GfVec4f>);

    // multiply (component-wise)
    _REG("ND_multiply_float",   &_EvalMultiply<float>);
    _REG("ND_multiply_color3",  &_EvalMultiply<GfVec3f>);
    _REG("ND_multiply_color4",  &_EvalMultiply<GfVec4f>);
    _REG("ND_multiply_vector2", &_EvalMultiply<GfVec2f>);
    _REG("ND_multiply_vector3", &_EvalMultiply<GfVec3f>);
    _REG("ND_multiply_vector4", &_EvalMultiply<GfVec4f>);

    // multiply (vector * float)
    _REG("ND_multiply_color3FA",  &_EvalMultiplyFA<GfVec3f>);
    _REG("ND_multiply_color4FA",  &_EvalMultiplyFA<GfVec4f>);
    _REG("ND_multiply_vector2FA", &_EvalMultiplyFA<GfVec2f>);
    _REG("ND_multiply_vector3FA", &_EvalMultiplyFA<GfVec3f>);
    _REG("ND_multiply_vector4FA", &_EvalMultiplyFA<GfVec4f>);

    // divide
    _REG("ND_divide_float",   &_EvalDivide<float>);
    _REG("ND_divide_color3",  &_EvalDivide<GfVec3f>);
    _REG("ND_divide_color4",  &_EvalDivide<GfVec4f>);
    _REG("ND_divide_vector2", &_EvalDivide<GfVec2f>);
    _REG("ND_divide_vector3", &_EvalDivide<GfVec3f>);
    _REG("ND_divide_vector4", &_EvalDivide<GfVec4f>);
    _REG("ND_divide_color3FA",  &_EvalDivideFA<GfVec3f>);
    _REG("ND_divide_color4FA",  &_EvalDivideFA<GfVec4f>);
    _REG("ND_divide_vector2FA", &_EvalDivideFA<GfVec2f>);
    _REG("ND_divide_vector3FA", &_EvalDivideFA<GfVec3f>);
    _REG("ND_divide_vector4FA", &_EvalDivideFA<GfVec4f>);

    // modulo
    _REG("ND_modulo_float", &_EvalModulo);

    // unary
    _REG("ND_absval_float",   &_EvalAbsvalFloat);
    _REG("ND_absval_color3",  &_EvalAbsvalVec3);
    _REG("ND_absval_vector3", &_EvalAbsvalVec3);
    _REG("ND_sign_float",  &_EvalSign);
    _REG("ND_floor_float",  &_EvalFloor);
    _REG("ND_ceil_float",   &_EvalCeil);
    _REG("ND_round_float",  &_EvalRound);
    _REG("ND_power_float",  &_EvalPower);
    _REG("ND_sqrt_float",   &_EvalSqrt);
    _REG("ND_ln_float",     &_EvalLn);
    _REG("ND_exp_float",    &_EvalExp);

    // negate
    _REG("ND_negate_float",   &_EvalNegate<float>);
    _REG("ND_negate_color3",  &_EvalNegate<GfVec3f>);
    _REG("ND_negate_vector3", &_EvalNegate<GfVec3f>);

    // invert
    _REG("ND_invert_float",   &_EvalInvert<float>);
    _REG("ND_invert_color3",  &_EvalInvert<GfVec3f>);
    _REG("ND_invert_color4",  &_EvalInvert<GfVec4f>);
    _REG("ND_invert_vector3", &_EvalInvert<GfVec3f>);

    // trigonometric
    _REG("ND_sin_float",   &_EvalSin);
    _REG("ND_cos_float",   &_EvalCos);
    _REG("ND_tan_float",   &_EvalTan);
    _REG("ND_asin_float",  &_EvalAsin);
    _REG("ND_acos_float",  &_EvalAcos);
    _REG("ND_atan2_float", &_EvalAtan2);

    // vector
    _REG("ND_dotproduct_vector3",  &_EvalDotProduct);
    _REG("ND_crossproduct_vector3", &_EvalCrossProduct);
    _REG("ND_normalize_vector3",   &_EvalNormalize);
    _REG("ND_magnitude_vector3",   &_EvalMagnitude);

    // constant
    _REG("ND_constant_float",   &_EvalConstant<float>);
    _REG("ND_constant_integer", &_EvalConstant<int>);
    _REG("ND_constant_boolean", &_EvalConstant<bool>);
    _REG("ND_constant_color3",  &_EvalConstant<GfVec3f>);
    _REG("ND_constant_color4",  &_EvalConstant<GfVec4f>);
    _REG("ND_constant_vector2", &_EvalConstant<GfVec2f>);
    _REG("ND_constant_vector3", &_EvalConstant<GfVec3f>);
    _REG("ND_constant_vector4", &_EvalConstant<GfVec4f>);
}

#undef _REG

PXR_NAMESPACE_CLOSE_SCOPE
