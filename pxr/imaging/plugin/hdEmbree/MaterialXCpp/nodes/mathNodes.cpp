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

static const SlotName _kIn("in");
static const SlotName _kIn1("in1");
static const SlotName _kIn2("in2");
static const SlotName _kOut("out");
static const SlotName _kPower("power");
static const SlotName _kValue("value");
static const SlotName _kAmount("amount");
static const SlotName _kNormal("normal");
static const SlotName _kIor("ior");
static const SlotName _kIn3("in3");
static const SlotName _kIn4("in4");
static const SlotName _kMat("mat");
static const SlotName _kAxis("axis");
static const SlotName _kTexcoord("texcoord");
static const SlotName _kPivot("pivot");
static const SlotName _kScale("scale");
static const SlotName _kRotate("rotate");
static const SlotName _kOffset("offset");
static const SlotName _kOperationOrder("operationorder");

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

// ---- safepower -----------------------------------------------------------
// sign(in1) * pow(abs(in1), in2) — safe for negative base values.

static inline float
_SafePow(float base, float exp)
{
    return (base < 0.0f ? -1.0f : (base > 0.0f ? 1.0f : 0.0f))
         * std::pow(std::fabs(base), exp);
}

static void
_EvalSafePowerFloat(const ParamMap& inputs, const ShadingContext&,
                    NodeOutputMap* outputs)
{
    float a = Get<float>(inputs, _kIn1, 0.0f);
    float b = Get<float>(inputs, _kIn2, 1.0f);
    (*outputs)[_kOut] = Value(_SafePow(a, b));
}

template<typename T>
static void
_EvalSafePower(const ParamMap& inputs, const ShadingContext&,
               NodeOutputMap* outputs)
{
    T a = Get<T>(inputs, _kIn1, Zero<T>());
    T b = Get<T>(inputs, _kIn2, One<T>());
    T result;
    for (int i = 0; i < T::dimensions(); ++i) {
        result[i] = _SafePow(a[i], b[i]);
    }
    (*outputs)[_kOut] = Value(result);
}

template<typename T>
static void
_EvalSafePowerFA(const ParamMap& inputs, const ShadingContext&,
                 NodeOutputMap* outputs)
{
    T a = Get<T>(inputs, _kIn1, Zero<T>());
    float b = Get<float>(inputs, _kIn2, 1.0f);
    T result;
    for (int i = 0; i < T::dimensions(); ++i) {
        result[i] = _SafePow(a[i], b);
    }
    (*outputs)[_kOut] = Value(result);
}

// ---- fract ---------------------------------------------------------------
// in - floor(in)

static void
_EvalFractFloat(const ParamMap& inputs, const ShadingContext&,
                NodeOutputMap* outputs)
{
    float v = Get<float>(inputs, _kIn, 0.0f);
    (*outputs)[_kOut] = Value(v - std::floor(v));
}

template<typename T>
static void
_EvalFract(const ParamMap& inputs, const ShadingContext&,
           NodeOutputMap* outputs)
{
    T v = Get<T>(inputs, _kIn, Zero<T>());
    T result;
    for (int i = 0; i < T::dimensions(); ++i) {
        result[i] = v[i] - std::floor(v[i]);
    }
    (*outputs)[_kOut] = Value(result);
}

// ---- distance ------------------------------------------------------------
// length(in1 - in2)

template<typename T>
static void
_EvalDistance(const ParamMap& inputs, const ShadingContext&,
             NodeOutputMap* outputs)
{
    T a = Get<T>(inputs, _kIn1, Zero<T>());
    T b = Get<T>(inputs, _kIn2, Zero<T>());
    (*outputs)[_kOut] = Value((a - b).length());
}

// ---- reflect -------------------------------------------------------------
// in - 2 * dot(in, normal) * normal

static void
_EvalReflect(const ParamMap& inputs, const ShadingContext& ctx,
             NodeOutputMap* outputs)
{
    Vec3f I = Get<Vec3f>(inputs, _kIn, Vec3f(1.0f, 0.0f, 0.0f));
    Vec3f N = Get<Vec3f>(inputs, _kNormal, ctx.normal);
    (*outputs)[_kOut] = Value(I - 2.0f * Dot(I, N) * N);
}

// ---- refract -------------------------------------------------------------
// Snell's law refraction

static void
_EvalRefract(const ParamMap& inputs, const ShadingContext& ctx,
             NodeOutputMap* outputs)
{
    Vec3f I = Get<Vec3f>(inputs, _kIn, Vec3f(1.0f, 0.0f, 0.0f));
    Vec3f N = Get<Vec3f>(inputs, _kNormal, ctx.normal);
    float ior = Get<float>(inputs, _kIor, 1.0f);
    float NdotI = Dot(N, I);
    float k = 1.0f - ior * ior * (1.0f - NdotI * NdotI);
    if (k < 0.0f) {
        (*outputs)[_kOut] = Value(Vec3f(0.0f));
    } else {
        (*outputs)[_kOut] = Value(ior * I - (ior * NdotI + std::sqrt(k)) * N);
    }
}

// ---- creatematrix --------------------------------------------------------

static void
_EvalCreateMatrixVec3M33(const ParamMap& inputs, const ShadingContext&,
                         NodeOutputMap* outputs)
{
    Vec3f r0 = Get<Vec3f>(inputs, _kIn1, Vec3f(1.0f, 0.0f, 0.0f));
    Vec3f r1 = Get<Vec3f>(inputs, _kIn2, Vec3f(0.0f, 1.0f, 0.0f));
    Vec3f r2 = Get<Vec3f>(inputs, _kIn3, Vec3f(0.0f, 0.0f, 1.0f));
    Mat3f m;
    m[0][0] = r0[0]; m[0][1] = r0[1]; m[0][2] = r0[2];
    m[1][0] = r1[0]; m[1][1] = r1[1]; m[1][2] = r1[2];
    m[2][0] = r2[0]; m[2][1] = r2[1]; m[2][2] = r2[2];
    (*outputs)[_kOut] = Value(m);
}

static void
_EvalCreateMatrixVec3M44(const ParamMap& inputs, const ShadingContext&,
                         NodeOutputMap* outputs)
{
    Vec3f r0 = Get<Vec3f>(inputs, _kIn1, Vec3f(1.0f, 0.0f, 0.0f));
    Vec3f r1 = Get<Vec3f>(inputs, _kIn2, Vec3f(0.0f, 1.0f, 0.0f));
    Vec3f r2 = Get<Vec3f>(inputs, _kIn3, Vec3f(0.0f, 0.0f, 1.0f));
    Vec3f r3 = Get<Vec3f>(inputs, _kIn4, Vec3f(0.0f, 0.0f, 0.0f));
    Mat4f m(r0[0], r0[1], r0[2], 0.0f,
            r1[0], r1[1], r1[2], 0.0f,
            r2[0], r2[1], r2[2], 0.0f,
            r3[0], r3[1], r3[2], 1.0f);
    (*outputs)[_kOut] = Value(m);
}

static void
_EvalCreateMatrixVec4M44(const ParamMap& inputs, const ShadingContext&,
                         NodeOutputMap* outputs)
{
    Vec4f r0 = Get<Vec4f>(inputs, _kIn1, Vec4f(1.0f, 0.0f, 0.0f, 0.0f));
    Vec4f r1 = Get<Vec4f>(inputs, _kIn2, Vec4f(0.0f, 1.0f, 0.0f, 0.0f));
    Vec4f r2 = Get<Vec4f>(inputs, _kIn3, Vec4f(0.0f, 0.0f, 1.0f, 0.0f));
    Vec4f r3 = Get<Vec4f>(inputs, _kIn4, Vec4f(0.0f, 0.0f, 0.0f, 1.0f));
    Mat4f m(r0[0], r0[1], r0[2], r0[3],
            r1[0], r1[1], r1[2], r1[3],
            r2[0], r2[1], r2[2], r2[3],
            r3[0], r3[1], r3[2], r3[3]);
    (*outputs)[_kOut] = Value(m);
}

// ---- transformpoint / transformvector / transformnormal -------------------
// Named space transforms — passthrough in hdEmbree (all shading in world space).

static void
_EvalTransformPassthrough(const ParamMap& inputs, const ShadingContext&,
                          NodeOutputMap* outputs)
{
    Vec3f v = Get<Vec3f>(inputs, _kIn, Vec3f(0.0f));
    (*outputs)[_kOut] = Value(v);
}

// ---- transformmatrix -----------------------------------------------------

static void
_EvalTransformMatrixVec2M3(const ParamMap& inputs, const ShadingContext&,
                           NodeOutputMap* outputs)
{
    Vec2f v = Get<Vec2f>(inputs, _kIn, Vec2f(0.0f));
    Mat3f m;
    const Value* value = inputs.Find(_kMat);
    if (value && ValueHolds<Mat3f>(*value)) {
        m = ValueGet<Mat3f>(*value);
    }
    // Imath Matrix33::multVecMatrix operates on Vec2 (homogeneous 2D).
    Vec2f r;
    m.multVecMatrix(v, r);
    (*outputs)[_kOut] = Value(r);
}

static void
_EvalTransformMatrixVec3M3(const ParamMap& inputs, const ShadingContext&,
                           NodeOutputMap* outputs)
{
    Vec3f v = Get<Vec3f>(inputs, _kIn, Vec3f(0.0f));
    Mat3f m;
    const Value* value = inputs.Find(_kMat);
    if (value && ValueHolds<Mat3f>(*value)) {
        m = ValueGet<Mat3f>(*value);
    }
    // Manual 3x3 matrix * vec3 (Imath Matrix33::multVecMatrix is Vec2 only).
    Vec3f r;
    for (int i = 0; i < 3; ++i) {
        r[i] = v[0] * m[0][i] + v[1] * m[1][i] + v[2] * m[2][i];
    }
    (*outputs)[_kOut] = Value(r);
}

static void
_EvalTransformMatrixVec3M4(const ParamMap& inputs, const ShadingContext&,
                           NodeOutputMap* outputs)
{
    Vec3f v = Get<Vec3f>(inputs, _kIn, Vec3f(0.0f));
    Mat4f m;
    const Value* value = inputs.Find(_kMat);
    if (value && ValueHolds<Mat4f>(*value)) {
        m = ValueGet<Mat4f>(*value);
    }
    Vec3f r;
    m.multVecMatrix(v, r);
    (*outputs)[_kOut] = Value(r);
}

static void
_EvalTransformMatrixVec4M4(const ParamMap& inputs, const ShadingContext&,
                           NodeOutputMap* outputs)
{
    Vec4f v = Get<Vec4f>(inputs, _kIn, Vec4f(0.0f));
    Mat4f m;
    const Value* value = inputs.Find(_kMat);
    if (value && ValueHolds<Mat4f>(*value)) {
        m = ValueGet<Mat4f>(*value);
    }
    // Manual 4x4 * vec4 multiply
    Vec4f r;
    for (int i = 0; i < 4; ++i) {
        r[i] = v[0]*m[0][i] + v[1]*m[1][i] + v[2]*m[2][i] + v[3]*m[3][i];
    }
    (*outputs)[_kOut] = Value(r);
}

// ---- transpose -----------------------------------------------------------

static void
_EvalTransposeM33(const ParamMap& inputs, const ShadingContext&,
                  NodeOutputMap* outputs)
{
    Mat3f m;
    const Value* value = inputs.Find(_kIn);
    if (value && ValueHolds<Mat3f>(*value)) {
        m = ValueGet<Mat3f>(*value);
    }
    (*outputs)[_kOut] = Value(m.transposed());
}

static void
_EvalTransposeM44(const ParamMap& inputs, const ShadingContext&,
                  NodeOutputMap* outputs)
{
    Mat4f m;
    const Value* value = inputs.Find(_kIn);
    if (value && ValueHolds<Mat4f>(*value)) {
        m = ValueGet<Mat4f>(*value);
    }
    (*outputs)[_kOut] = Value(m.transposed());
}

// ---- determinant ---------------------------------------------------------

static void
_EvalDeterminantM33(const ParamMap& inputs, const ShadingContext&,
                    NodeOutputMap* outputs)
{
    Mat3f m;
    const Value* value = inputs.Find(_kIn);
    if (value && ValueHolds<Mat3f>(*value)) {
        m = ValueGet<Mat3f>(*value);
    }
    // M33f determinant: manually compute
    float det = m[0][0]*(m[1][1]*m[2][2] - m[1][2]*m[2][1])
              - m[0][1]*(m[1][0]*m[2][2] - m[1][2]*m[2][0])
              + m[0][2]*(m[1][0]*m[2][1] - m[1][1]*m[2][0]);
    (*outputs)[_kOut] = Value(det);
}

static void
_EvalDeterminantM44(const ParamMap& inputs, const ShadingContext&,
                    NodeOutputMap* outputs)
{
    Mat4f m;
    const Value* value = inputs.Find(_kIn);
    if (value && ValueHolds<Mat4f>(*value)) {
        m = ValueGet<Mat4f>(*value);
    }
    // Use Imath cofactor expansion
    float det = m[0][0] * (m[1][1]*(m[2][2]*m[3][3] - m[2][3]*m[3][2])
                          - m[1][2]*(m[2][1]*m[3][3] - m[2][3]*m[3][1])
                          + m[1][3]*(m[2][1]*m[3][2] - m[2][2]*m[3][1]))
              - m[0][1] * (m[1][0]*(m[2][2]*m[3][3] - m[2][3]*m[3][2])
                          - m[1][2]*(m[2][0]*m[3][3] - m[2][3]*m[3][0])
                          + m[1][3]*(m[2][0]*m[3][2] - m[2][2]*m[3][0]))
              + m[0][2] * (m[1][0]*(m[2][1]*m[3][3] - m[2][3]*m[3][1])
                          - m[1][1]*(m[2][0]*m[3][3] - m[2][3]*m[3][0])
                          + m[1][3]*(m[2][0]*m[3][1] - m[2][1]*m[3][0]))
              - m[0][3] * (m[1][0]*(m[2][1]*m[3][2] - m[2][2]*m[3][1])
                          - m[1][1]*(m[2][0]*m[3][2] - m[2][2]*m[3][0])
                          + m[1][2]*(m[2][0]*m[3][1] - m[2][1]*m[3][0]));
    (*outputs)[_kOut] = Value(det);
}

// ---- invertmatrix --------------------------------------------------------

static void
_EvalInvertMatrixM33(const ParamMap& inputs, const ShadingContext&,
                     NodeOutputMap* outputs)
{
    Mat3f m;
    const Value* value = inputs.Find(_kIn);
    if (value && ValueHolds<Mat3f>(*value)) {
        m = ValueGet<Mat3f>(*value);
    }
    (*outputs)[_kOut] = Value(m.inverse());
}

static void
_EvalInvertMatrixM44(const ParamMap& inputs, const ShadingContext&,
                     NodeOutputMap* outputs)
{
    Mat4f m;
    const Value* value = inputs.Find(_kIn);
    if (value && ValueHolds<Mat4f>(*value)) {
        m = ValueGet<Mat4f>(*value);
    }
    (*outputs)[_kOut] = Value(m.inverse());
}

// ---- rotate2d ------------------------------------------------------------
// Rotate vector2 by amount (degrees) about the origin.

static void
_EvalRotate2d(const ParamMap& inputs, const ShadingContext&,
              NodeOutputMap* outputs)
{
    Vec2f v = Get<Vec2f>(inputs, _kIn, Vec2f(0.0f));
    float amount = Get<float>(inputs, _kAmount, 0.0f);
    float rad = amount * (static_cast<float>(M_PI) / 180.0f);
    float c = std::cos(rad);
    float s = std::sin(rad);
    (*outputs)[_kOut] = Value(Vec2f(v[0]*c - v[1]*s,
                                    v[0]*s + v[1]*c));
}

// ---- rotate3d ------------------------------------------------------------
// Rotate vector3 by amount (degrees) about axis.

static void
_EvalRotate3d(const ParamMap& inputs, const ShadingContext&,
              NodeOutputMap* outputs)
{
    Vec3f v = Get<Vec3f>(inputs, _kIn, Vec3f(0.0f));
    float amount = Get<float>(inputs, _kAmount, 0.0f);
    Vec3f axis = Get<Vec3f>(inputs, _kAxis, Vec3f(0.0f, 1.0f, 0.0f));
    float len = axis.length();
    if (len < 1e-8f) {
        (*outputs)[_kOut] = Value(v);
        return;
    }
    axis /= len;
    float rad = amount * (static_cast<float>(M_PI) / 180.0f);
    float c = std::cos(rad);
    float s = std::sin(rad);
    // Rodrigues' rotation formula
    Vec3f result = v * c + Cross(axis, v) * s + axis * Dot(axis, v) * (1.0f - c);
    (*outputs)[_kOut] = Value(result);
}

// ---- place2d -------------------------------------------------------------
// Transform UV coordinates: pivot, scale, rotate, offset, operationorder.

static void
_EvalPlace2d(const ParamMap& inputs, const ShadingContext&,
             NodeOutputMap* outputs)
{
    Vec2f uv     = Get<Vec2f>(inputs, _kTexcoord, Vec2f(0.0f));
    Vec2f pivot  = Get<Vec2f>(inputs, _kPivot, Vec2f(0.0f));
    Vec2f scale  = Get<Vec2f>(inputs, _kScale, Vec2f(1.0f));
    float rotate = Get<float>(inputs, _kRotate, 0.0f);
    Vec2f offset = Get<Vec2f>(inputs, _kOffset, Vec2f(0.0f));
    int order    = Get<int>(inputs, _kOperationOrder, 0);

    float rad = rotate * (static_cast<float>(M_PI) / 180.0f);
    float c = std::cos(rad);
    float s = std::sin(rad);

    auto applyScale = [&](Vec2f p) -> Vec2f {
        return Vec2f(p[0] * scale[0], p[1] * scale[1]);
    };
    auto applyRotate = [&](Vec2f p) -> Vec2f {
        return Vec2f(p[0]*c - p[1]*s, p[0]*s + p[1]*c);
    };
    auto applyTranslate = [&](Vec2f p) -> Vec2f {
        return p + offset;
    };

    Vec2f result = uv - pivot;
    if (order == 0) {
        // SRT
        result = applyScale(result);
        result = applyRotate(result);
        result = applyTranslate(result);
    } else {
        // TRS
        result = applyTranslate(result);
        result = applyRotate(result);
        result = applyScale(result);
    }
    result = result + pivot;

    (*outputs)[_kOut] = Value(result);
}

// ---- trianglewave --------------------------------------------------------
// 2 * abs(in - floor(in + 0.5))

static void
_EvalTriangleWave(const ParamMap& inputs, const ShadingContext&,
                  NodeOutputMap* outputs)
{
    float v = Get<float>(inputs, _kIn, 0.0f);
    (*outputs)[_kOut] = Value(2.0f * std::fabs(v - std::floor(v + 0.5f)));
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

    // safepower
    _REG("ND_safepower_float",      &_EvalSafePowerFloat);
    _REG("ND_safepower_color3",     &_EvalSafePower<Vec3f>);
    _REG("ND_safepower_color4",     &_EvalSafePower<Vec4f>);
    _REG("ND_safepower_vector2",    &_EvalSafePower<Vec2f>);
    _REG("ND_safepower_vector3",    &_EvalSafePower<Vec3f>);
    _REG("ND_safepower_vector4",    &_EvalSafePower<Vec4f>);
    _REG("ND_safepower_color3FA",   &_EvalSafePowerFA<Vec3f>);
    _REG("ND_safepower_color4FA",   &_EvalSafePowerFA<Vec4f>);
    _REG("ND_safepower_vector2FA",  &_EvalSafePowerFA<Vec2f>);
    _REG("ND_safepower_vector3FA",  &_EvalSafePowerFA<Vec3f>);
    _REG("ND_safepower_vector4FA",  &_EvalSafePowerFA<Vec4f>);

    // fract
    _REG("ND_fract_float",   &_EvalFractFloat);
    _REG("ND_fract_color3",  &_EvalFract<Vec3f>);
    _REG("ND_fract_color4",  &_EvalFract<Vec4f>);
    _REG("ND_fract_vector2", &_EvalFract<Vec2f>);
    _REG("ND_fract_vector3", &_EvalFract<Vec3f>);
    _REG("ND_fract_vector4", &_EvalFract<Vec4f>);

    // distance
    _REG("ND_distance_vector2", &_EvalDistance<Vec2f>);
    _REG("ND_distance_vector3", &_EvalDistance<Vec3f>);
    _REG("ND_distance_vector4", &_EvalDistance<Vec4f>);

    // reflect / refract
    _REG("ND_reflect_vector3", &_EvalReflect);
    _REG("ND_refract_vector3", &_EvalRefract);

    // creatematrix
    _REG("ND_creatematrix_vector3_matrix33", &_EvalCreateMatrixVec3M33);
    _REG("ND_creatematrix_vector3_matrix44", &_EvalCreateMatrixVec3M44);
    _REG("ND_creatematrix_vector4_matrix44", &_EvalCreateMatrixVec4M44);

    // transform (named space — passthrough in hdEmbree)
    _REG("ND_transformpoint_vector3",  &_EvalTransformPassthrough);
    _REG("ND_transformvector_vector3", &_EvalTransformPassthrough);
    _REG("ND_transformnormal_vector3", &_EvalTransformPassthrough);

    // transformmatrix
    _REG("ND_transformmatrix_vector2M3",  &_EvalTransformMatrixVec2M3);
    _REG("ND_transformmatrix_vector3",    &_EvalTransformMatrixVec3M3);
    _REG("ND_transformmatrix_vector3M4",  &_EvalTransformMatrixVec3M4);
    _REG("ND_transformmatrix_vector4",    &_EvalTransformMatrixVec4M4);

    // transpose
    _REG("ND_transpose_matrix33", &_EvalTransposeM33);
    _REG("ND_transpose_matrix44", &_EvalTransposeM44);

    // determinant
    _REG("ND_determinant_matrix33", &_EvalDeterminantM33);
    _REG("ND_determinant_matrix44", &_EvalDeterminantM44);

    // invertmatrix
    _REG("ND_invertmatrix_matrix33", &_EvalInvertMatrixM33);
    _REG("ND_invertmatrix_matrix44", &_EvalInvertMatrixM44);

    // rotate
    _REG("ND_rotate2d_vector2", &_EvalRotate2d);
    _REG("ND_rotate3d_vector3", &_EvalRotate3d);

    // place2d
    _REG("ND_place2d_vector2", &_EvalPlace2d);

    // trianglewave
    _REG("ND_trianglewave_float", &_EvalTriangleWave);

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

}  // namespace mxcpp
