//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "mathNodes.h"
#include "../nodeRegistry.h"

#include <cmath>
#include <algorithm>
#include <string>

namespace mxcpp {

static const std::string _kIn = "in";
static const std::string _kIn1 = "in1";
static const std::string _kIn2 = "in2";
static const std::string _kOut = "out";
static const std::string _kPower = "power";
static const std::string _kValue = "value";
static const std::string _kAmount = "amount";

// ---- Arithmetic templates ------------------------------------------------

template<typename T>
static void
_EvalAdd(const ParamMap& inputs, const ShadingContext&,
         NodeOutputMap* outputs)
{
    T a = Get<T>(inputs, _kIn1, Zero<T>());
    T b = Get<T>(inputs, _kIn2, Zero<T>());
    (*outputs)[_kOut] = Value(a + b);
}

template<typename T>
static void
_EvalSubtract(const ParamMap& inputs, const ShadingContext&,
              NodeOutputMap* outputs)
{
    T a = Get<T>(inputs, _kIn1, Zero<T>());
    T b = Get<T>(inputs, _kIn2, Zero<T>());
    (*outputs)[_kOut] = Value(a - b);
}

template<typename T>
static void
_EvalMultiply(const ParamMap& inputs, const ShadingContext&,
              NodeOutputMap* outputs)
{
    T a = Get<T>(inputs, _kIn1, One<T>());
    T b = Get<T>(inputs, _kIn2, One<T>());
    (*outputs)[_kOut] = Value(CompMul(a, b));
}

// Multiply vector by scalar (FA = float argument)
template<typename T>
static void
_EvalMultiplyFA(const ParamMap& inputs, const ShadingContext&,
                NodeOutputMap* outputs)
{
    T a = Get<T>(inputs, _kIn1, One<T>());
    float b = Get<float>(inputs, _kIn2, 1.0f);
    (*outputs)[_kOut] = Value(a * b);
}

template<typename T>
static void
_EvalDivide(const ParamMap& inputs, const ShadingContext&,
            NodeOutputMap* outputs)
{
    T a = Get<T>(inputs, _kIn1, Zero<T>());
    T b = Get<T>(inputs, _kIn2, One<T>());
    (*outputs)[_kOut] = Value(CompDiv(a, b));
}

template<typename T>
static void
_EvalDivideFA(const ParamMap& inputs, const ShadingContext&,
              NodeOutputMap* outputs)
{
    T a = Get<T>(inputs, _kIn1, Zero<T>());
    float b = Get<float>(inputs, _kIn2, 1.0f);
    (*outputs)[_kOut] = Value(b != 0.0f ? a / b : Zero<T>());
}

static void
_EvalModulo(const ParamMap& inputs, const ShadingContext&,
            NodeOutputMap* outputs)
{
    float a = Get<float>(inputs, _kIn1, 0.0f);
    float b = Get<float>(inputs, _kIn2, 1.0f);
    (*outputs)[_kOut] = Value(b != 0.0f ? std::fmod(a, b) : 0.0f);
}

// ---- Unary math ----------------------------------------------------------

static void
_EvalAbsvalFloat(const ParamMap& inputs, const ShadingContext&,
                 NodeOutputMap* outputs)
{
    float v = Get<float>(inputs, _kIn, 0.0f);
    (*outputs)[_kOut] = Value(std::fabs(v));
}

static void
_EvalAbsvalVec3(const ParamMap& inputs, const ShadingContext&,
                NodeOutputMap* outputs)
{
    Vec3f v = Get<Vec3f>(inputs, _kIn, Vec3f(0.0f));
    (*outputs)[_kOut] = Value(
        Vec3f(std::fabs(v[0]), std::fabs(v[1]), std::fabs(v[2])));
}

static void
_EvalSign(const ParamMap& inputs, const ShadingContext&,
          NodeOutputMap* outputs)
{
    float v = Get<float>(inputs, _kIn, 0.0f);
    (*outputs)[_kOut] = Value(v > 0.0f ? 1.0f : (v < 0.0f ? -1.0f : 0.0f));
}

static void
_EvalFloor(const ParamMap& inputs, const ShadingContext&,
           NodeOutputMap* outputs)
{
    float v = Get<float>(inputs, _kIn, 0.0f);
    (*outputs)[_kOut] = Value(std::floor(v));
}

static void
_EvalCeil(const ParamMap& inputs, const ShadingContext&,
          NodeOutputMap* outputs)
{
    float v = Get<float>(inputs, _kIn, 0.0f);
    (*outputs)[_kOut] = Value(std::ceil(v));
}

static void
_EvalRound(const ParamMap& inputs, const ShadingContext&,
           NodeOutputMap* outputs)
{
    float v = Get<float>(inputs, _kIn, 0.0f);
    (*outputs)[_kOut] = Value(std::round(v));
}

static void
_EvalPower(const ParamMap& inputs, const ShadingContext&,
           NodeOutputMap* outputs)
{
    float base = Get<float>(inputs, _kIn1, 0.0f);
    float exp  = Get<float>(inputs, _kIn2, 1.0f);
    (*outputs)[_kOut] = Value(std::pow(base, exp));
}

static void
_EvalSqrt(const ParamMap& inputs, const ShadingContext&,
          NodeOutputMap* outputs)
{
    float v = Get<float>(inputs, _kIn, 0.0f);
    (*outputs)[_kOut] = Value(std::sqrt(std::max(0.0f, v)));
}

static void
_EvalLn(const ParamMap& inputs, const ShadingContext&,
        NodeOutputMap* outputs)
{
    float v = Get<float>(inputs, _kIn, 1.0f);
    (*outputs)[_kOut] = Value(v > 0.0f ? std::log(v) : 0.0f);
}

static void
_EvalExp(const ParamMap& inputs, const ShadingContext&,
         NodeOutputMap* outputs)
{
    float v = Get<float>(inputs, _kIn, 0.0f);
    (*outputs)[_kOut] = Value(std::exp(v));
}

template<typename T>
static void
_EvalNegate(const ParamMap& inputs, const ShadingContext&,
            NodeOutputMap* outputs)
{
    T v = Get<T>(inputs, _kIn, Zero<T>());
    (*outputs)[_kOut] = Value(-v);
}

template<typename T>
static void
_EvalInvert(const ParamMap& inputs, const ShadingContext&,
            NodeOutputMap* outputs)
{
    T v = Get<T>(inputs, _kIn, Zero<T>());
    T amount = Get<T>(inputs, _kAmount, One<T>());
    (*outputs)[_kOut] = Value(amount - v);
}

// Scalar specialization for invert
template<>
void
_EvalInvert<float>(const ParamMap& inputs, const ShadingContext&,
                   NodeOutputMap* outputs)
{
    float v = Get<float>(inputs, _kIn, 0.0f);
    float amount = Get<float>(inputs, _kAmount, 1.0f);
    (*outputs)[_kOut] = Value(amount - v);
}

// ---- Trigonometric -------------------------------------------------------

static void _EvalSin(const ParamMap& in, const ShadingContext&,
                     NodeOutputMap* out) {
    (*out)[_kOut] = Value(std::sin(Get<float>(in, _kIn, 0.0f)));
}
static void _EvalCos(const ParamMap& in, const ShadingContext&,
                     NodeOutputMap* out) {
    (*out)[_kOut] = Value(std::cos(Get<float>(in, _kIn, 0.0f)));
}
static void _EvalTan(const ParamMap& in, const ShadingContext&,
                     NodeOutputMap* out) {
    (*out)[_kOut] = Value(std::tan(Get<float>(in, _kIn, 0.0f)));
}
static void _EvalAsin(const ParamMap& in, const ShadingContext&,
                      NodeOutputMap* out) {
    float v = std::clamp(Get<float>(in, _kIn, 0.0f), -1.0f, 1.0f);
    (*out)[_kOut] = Value(std::asin(v));
}
static void _EvalAcos(const ParamMap& in, const ShadingContext&,
                      NodeOutputMap* out) {
    float v = std::clamp(Get<float>(in, _kIn, 0.0f), -1.0f, 1.0f);
    (*out)[_kOut] = Value(std::acos(v));
}
static void _EvalAtan2(const ParamMap& in, const ShadingContext&,
                       NodeOutputMap* out) {
    float y = Get<float>(in, _kIn1, 0.0f);
    float x = Get<float>(in, _kIn2, 1.0f);
    (*out)[_kOut] = Value(std::atan2(y, x));
}

// ---- Vector operations ---------------------------------------------------

static void
_EvalDotProduct(const ParamMap& inputs, const ShadingContext&,
                NodeOutputMap* outputs)
{
    Vec3f a = Get<Vec3f>(inputs, _kIn1, Vec3f(0.0f));
    Vec3f b = Get<Vec3f>(inputs, _kIn2, Vec3f(0.0f));
    (*outputs)[_kOut] = Value(Dot(a, b));
}

static void
_EvalCrossProduct(const ParamMap& inputs, const ShadingContext&,
                  NodeOutputMap* outputs)
{
    Vec3f a = Get<Vec3f>(inputs, _kIn1, Vec3f(0.0f));
    Vec3f b = Get<Vec3f>(inputs, _kIn2, Vec3f(0.0f));
    (*outputs)[_kOut] = Value(Cross(a, b));
}

static void
_EvalNormalize(const ParamMap& inputs, const ShadingContext&,
               NodeOutputMap* outputs)
{
    Vec3f v = Get<Vec3f>(inputs, _kIn, Vec3f(0.0f, 0.0f, 1.0f));
    float len = v.length();
    (*outputs)[_kOut] = Value(len > 0.0f ? v / len : Vec3f(0.0f));
}

static void
_EvalMagnitude(const ParamMap& inputs, const ShadingContext&,
               NodeOutputMap* outputs)
{
    Vec3f v = Get<Vec3f>(inputs, _kIn, Vec3f(0.0f));
    (*outputs)[_kOut] = Value(v.length());
}

// ---- Constant node -------------------------------------------------------

template<typename T>
static void
_EvalConstant(const ParamMap& inputs, const ShadingContext&,
              NodeOutputMap* outputs)
{
    T v = Get<T>(inputs, _kValue, Zero<T>());
    (*outputs)[_kOut] = Value(v);
}

// ---- Registration --------------------------------------------------------

#define _REG(name, fn) reg.Register(name, fn)

void
RegisterMathNodes(NodeRegistry& reg)
{
    // add
    _REG("ND_add_float",   &_EvalAdd<float>);
    _REG("ND_add_color3",  &_EvalAdd<Vec3f>);
    _REG("ND_add_color4",  &_EvalAdd<Vec4f>);
    _REG("ND_add_vector2", &_EvalAdd<Vec2f>);
    _REG("ND_add_vector3", &_EvalAdd<Vec3f>);
    _REG("ND_add_vector4", &_EvalAdd<Vec4f>);

    // subtract
    _REG("ND_subtract_float",   &_EvalSubtract<float>);
    _REG("ND_subtract_color3",  &_EvalSubtract<Vec3f>);
    _REG("ND_subtract_color4",  &_EvalSubtract<Vec4f>);
    _REG("ND_subtract_vector2", &_EvalSubtract<Vec2f>);
    _REG("ND_subtract_vector3", &_EvalSubtract<Vec3f>);
    _REG("ND_subtract_vector4", &_EvalSubtract<Vec4f>);

    // multiply (component-wise)
    _REG("ND_multiply_float",   &_EvalMultiply<float>);
    _REG("ND_multiply_color3",  &_EvalMultiply<Vec3f>);
    _REG("ND_multiply_color4",  &_EvalMultiply<Vec4f>);
    _REG("ND_multiply_vector2", &_EvalMultiply<Vec2f>);
    _REG("ND_multiply_vector3", &_EvalMultiply<Vec3f>);
    _REG("ND_multiply_vector4", &_EvalMultiply<Vec4f>);

    // multiply (vector * float)
    _REG("ND_multiply_color3FA",  &_EvalMultiplyFA<Vec3f>);
    _REG("ND_multiply_color4FA",  &_EvalMultiplyFA<Vec4f>);
    _REG("ND_multiply_vector2FA", &_EvalMultiplyFA<Vec2f>);
    _REG("ND_multiply_vector3FA", &_EvalMultiplyFA<Vec3f>);
    _REG("ND_multiply_vector4FA", &_EvalMultiplyFA<Vec4f>);

    // divide
    _REG("ND_divide_float",   &_EvalDivide<float>);
    _REG("ND_divide_color3",  &_EvalDivide<Vec3f>);
    _REG("ND_divide_color4",  &_EvalDivide<Vec4f>);
    _REG("ND_divide_vector2", &_EvalDivide<Vec2f>);
    _REG("ND_divide_vector3", &_EvalDivide<Vec3f>);
    _REG("ND_divide_vector4", &_EvalDivide<Vec4f>);
    _REG("ND_divide_color3FA",  &_EvalDivideFA<Vec3f>);
    _REG("ND_divide_color4FA",  &_EvalDivideFA<Vec4f>);
    _REG("ND_divide_vector2FA", &_EvalDivideFA<Vec2f>);
    _REG("ND_divide_vector3FA", &_EvalDivideFA<Vec3f>);
    _REG("ND_divide_vector4FA", &_EvalDivideFA<Vec4f>);

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
    _REG("ND_negate_color3",  &_EvalNegate<Vec3f>);
    _REG("ND_negate_vector3", &_EvalNegate<Vec3f>);

    // invert
    _REG("ND_invert_float",   &_EvalInvert<float>);
    _REG("ND_invert_color3",  &_EvalInvert<Vec3f>);
    _REG("ND_invert_color4",  &_EvalInvert<Vec4f>);
    _REG("ND_invert_vector3", &_EvalInvert<Vec3f>);

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
    _REG("ND_constant_color3",  &_EvalConstant<Vec3f>);
    _REG("ND_constant_color4",  &_EvalConstant<Vec4f>);
    _REG("ND_constant_vector2", &_EvalConstant<Vec2f>);
    _REG("ND_constant_vector3", &_EvalConstant<Vec3f>);
    _REG("ND_constant_vector4", &_EvalConstant<Vec4f>);
}

#undef _REG

} // namespace mxcpp
