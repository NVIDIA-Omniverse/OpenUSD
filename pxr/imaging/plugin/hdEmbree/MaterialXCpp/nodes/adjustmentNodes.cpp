//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "adjustmentNodes.h"
#include "../nodeRegistry.h"

#include <cmath>
#include <algorithm>
#include <string>

namespace mxcpp {

static const SlotName _kIn("in");
static const SlotName _kIn1("in1");
static const SlotName _kIn2("in2");
static const SlotName _kFg("fg");
static const SlotName _kBg("bg");
static const SlotName _kMix("mix");
static const SlotName _kLow("low");
static const SlotName _kHigh("high");
static const SlotName _kInlow("inlow");
static const SlotName _kInhigh("inhigh");
static const SlotName _kOutlow("outlow");
static const SlotName _kOuthigh("outhigh");
static const SlotName _kOut("out");
static const SlotName _kAmount("amount");
static const SlotName _kCenter("center");

// ---- Clamp / Min / Max ---------------------------------------------------

template<typename T>
static T _Clamp(const T& v, const T& lo, const T& hi);

template<>
float _Clamp<float>(const float& v, const float& lo, const float& hi) {
    return std::clamp(v, lo, hi);
}
template<>
Vec3f _Clamp<Vec3f>(const Vec3f& v, const Vec3f& lo, const Vec3f& hi) {
    return Vec3f(std::clamp(v[0], lo[0], hi[0]),
                   std::clamp(v[1], lo[1], hi[1]),
                   std::clamp(v[2], lo[2], hi[2]));
}
template<>
Vec4f _Clamp<Vec4f>(const Vec4f& v, const Vec4f& lo, const Vec4f& hi) {
    return Vec4f(std::clamp(v[0], lo[0], hi[0]),
                   std::clamp(v[1], lo[1], hi[1]),
                   std::clamp(v[2], lo[2], hi[2]),
                   std::clamp(v[3], lo[3], hi[3]));
}

template<typename T>
static void
_EvalClamp(const ParamMap& inputs, const ShadingContext&,
           NodeOutputMap* outputs)
{
    T v  = Get<T>(inputs, _kIn,   Zero<T>());
    T lo = Get<T>(inputs, _kLow,  Zero<T>());
    T hi = Get<T>(inputs, _kHigh, One<T>());
    (*outputs)[_kOut] = Value(_Clamp(v, lo, hi));
}

template<typename T>
static void
_EvalMin(const ParamMap& inputs, const ShadingContext&,
         NodeOutputMap* outputs)
{
    T a = Get<T>(inputs, _kIn1, Zero<T>());
    T b = Get<T>(inputs, _kIn2, Zero<T>());
    // Component-wise min for vectors is not needed for most use cases;
    // scalar min is the primary use.
    (*outputs)[_kOut] = Value(a < b ? a : b);
}

// Scalar-only min/max are sufficient for most MaterialX usage.
static void
_EvalMinFloat(const ParamMap& inputs, const ShadingContext&,
              NodeOutputMap* outputs)
{
    float a = Get<float>(inputs, _kIn1, 0.0f);
    float b = Get<float>(inputs, _kIn2, 0.0f);
    (*outputs)[_kOut] = Value(std::min(a, b));
}

static void
_EvalMaxFloat(const ParamMap& inputs, const ShadingContext&,
              NodeOutputMap* outputs)
{
    float a = Get<float>(inputs, _kIn1, 0.0f);
    float b = Get<float>(inputs, _kIn2, 0.0f);
    (*outputs)[_kOut] = Value(std::max(a, b));
}

// ---- Remap / Smoothstep --------------------------------------------------

static float
_Remap(float v, float inLo, float inHi, float outLo, float outHi) {
    if (inHi == inLo) return outLo;
    float t = (v - inLo) / (inHi - inLo);
    return outLo + t * (outHi - outLo);
}

template<typename T>
static void
_EvalRemap(const ParamMap& inputs, const ShadingContext&,
           NodeOutputMap* outputs)
{
    T v  = Get<T>(inputs, _kIn,     Zero<T>());
    T il = Get<T>(inputs, _kInlow,  Zero<T>());
    T ih = Get<T>(inputs, _kInhigh, One<T>());
    T ol = Get<T>(inputs, _kOutlow, Zero<T>());
    T oh = Get<T>(inputs, _kOuthigh, One<T>());
    // Scalar remap for float type.
    (*outputs)[_kOut] = Value(
        _Remap(Get<float>(inputs, _kIn, 0.0f),
               Get<float>(inputs, _kInlow, 0.0f),
               Get<float>(inputs, _kInhigh, 1.0f),
               Get<float>(inputs, _kOutlow, 0.0f),
               Get<float>(inputs, _kOuthigh, 1.0f)));
}

// Specialization for float remap
static void
_EvalRemapFloat(const ParamMap& inputs, const ShadingContext&,
                NodeOutputMap* outputs)
{
    float v  = Get<float>(inputs, _kIn,     0.0f);
    float il = Get<float>(inputs, _kInlow,  0.0f);
    float ih = Get<float>(inputs, _kInhigh, 1.0f);
    float ol = Get<float>(inputs, _kOutlow, 0.0f);
    float oh = Get<float>(inputs, _kOuthigh, 1.0f);
    (*outputs)[_kOut] = Value(_Remap(v, il, ih, ol, oh));
}

static float
_Smoothstep(float lo, float hi, float v) {
    if (hi <= lo) return 0.0f;
    float t = std::clamp((v - lo) / (hi - lo), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static void
_EvalSmoothstep(const ParamMap& inputs, const ShadingContext&,
                NodeOutputMap* outputs)
{
    float v  = Get<float>(inputs, _kIn,   0.0f);
    float lo = Get<float>(inputs, _kLow,  0.0f);
    float hi = Get<float>(inputs, _kHigh, 1.0f);
    (*outputs)[_kOut] = Value(_Smoothstep(lo, hi, v));
}

// ---- Mix -----------------------------------------------------------------

template<typename T>
static void
_EvalMix(const ParamMap& inputs, const ShadingContext&,
         NodeOutputMap* outputs)
{
    T fg  = Get<T>(inputs, _kFg, Zero<T>());
    T bg  = Get<T>(inputs, _kBg, Zero<T>());
    float m = Get<float>(inputs, _kMix, 0.0f);
    (*outputs)[_kOut] = Value(bg + (fg - bg) * m);
}

// ---- Contrast ------------------------------------------------------------

template<typename T>
static void
_EvalContrast(const ParamMap& inputs, const ShadingContext&,
              NodeOutputMap* outputs)
{
    T v      = Get<T>(inputs, _kIn,     Zero<T>());
    T amount = Get<T>(inputs, _kAmount, One<T>());
    T pivot  = Get<T>(inputs, _kCenter, T(0.5f));
    // contrast = pivot + (v - pivot) * amount
    (*outputs)[_kOut] = Value(
        pivot + CompMul(v - pivot, amount));
}

static void
_EvalContrastFloat(const ParamMap& inputs, const ShadingContext&,
                   NodeOutputMap* outputs)
{
    float v      = Get<float>(inputs, _kIn,     0.0f);
    float amount = Get<float>(inputs, _kAmount, 1.0f);
    float pivot  = Get<float>(inputs, _kCenter, 0.5f);
    (*outputs)[_kOut] = Value(pivot + (v - pivot) * amount);
}

// ---- Premult / Unpremult -------------------------------------------------

static void
_EvalPremult(const ParamMap& inputs, const ShadingContext&,
             NodeOutputMap* outputs)
{
    Vec4f v = Get<Vec4f>(inputs, _kIn, Vec4f(0.0f));
    (*outputs)[_kOut] = Value(
        Vec4f(v[0]*v[3], v[1]*v[3], v[2]*v[3], v[3]));
}

static void
_EvalUnpremult(const ParamMap& inputs, const ShadingContext&,
               NodeOutputMap* outputs)
{
    Vec4f v = Get<Vec4f>(inputs, _kIn, Vec4f(0.0f));
    float a = v[3] > 0.0f ? 1.0f / v[3] : 0.0f;
    (*outputs)[_kOut] = Value(
        Vec4f(v[0]*a, v[1]*a, v[2]*a, v[3]));
}

// ---- Registration --------------------------------------------------------

#define _REG(name, fn) reg.Register(name, fn)

void
RegisterAdjustmentNodes(NodeRegistry& reg)
{
    _REG("ND_clamp_float",   &_EvalClamp<float>);
    _REG("ND_clamp_color3",  &_EvalClamp<Vec3f>);
    _REG("ND_clamp_color4",  &_EvalClamp<Vec4f>);
    _REG("ND_clamp_vector3", &_EvalClamp<Vec3f>);

    _REG("ND_min_float", &_EvalMinFloat);
    _REG("ND_max_float", &_EvalMaxFloat);

    _REG("ND_remap_float", &_EvalRemapFloat);
    _REG("ND_smoothstep_float", &_EvalSmoothstep);

    _REG("ND_mix_float",   &_EvalMix<float>);
    _REG("ND_mix_color3",  &_EvalMix<Vec3f>);
    _REG("ND_mix_color4",  &_EvalMix<Vec4f>);
    _REG("ND_mix_vector2", &_EvalMix<Vec2f>);
    _REG("ND_mix_vector3", &_EvalMix<Vec3f>);
    _REG("ND_mix_vector4", &_EvalMix<Vec4f>);

    _REG("ND_contrast_float",  &_EvalContrastFloat);
    _REG("ND_contrast_color3", &_EvalContrast<Vec3f>);
    _REG("ND_contrast_color4", &_EvalContrast<Vec4f>);

    _REG("ND_premult_color4",   &_EvalPremult);
    _REG("ND_unpremult_color4", &_EvalUnpremult);
}

#undef _REG

}  // namespace mxcpp
