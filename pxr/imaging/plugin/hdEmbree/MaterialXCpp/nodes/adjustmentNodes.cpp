//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "adjustmentNodes.h"
#include "colorNodes.h"
#include "../nodeRegistry.h"

#include <cmath>
#include <algorithm>
#include <string>

namespace mxcpp {

static const SlotName _kIn("in");
static const SlotName _kIn1("in1");
static const SlotName _kIn2("in2");
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
Vec2f _Clamp<Vec2f>(const Vec2f& v, const Vec2f& lo, const Vec2f& hi) {
    return Vec2f(std::clamp(v[0], lo[0], hi[0]),
                   std::clamp(v[1], lo[1], hi[1]));
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

// ---- Saturate ------------------------------------------------------------

// ACEScg luminance coefficients (MaterialX default for saturate).
static constexpr float _kSatLumR = 0.2722287f;
static constexpr float _kSatLumG = 0.6740818f;
static constexpr float _kSatLumB = 0.0536895f;

static const SlotName _kLumacoeffs("lumacoeffs");

static void
_EvalSaturateColor3(const ParamMap& inputs, const ShadingContext&,
                    NodeOutputMap* outputs)
{
    Vec3f c = Get<Vec3f>(inputs, _kIn, Vec3f(0.0f));
    float amount = Get<float>(inputs, _kAmount, 1.0f);
    Vec3f luma = Get<Vec3f>(inputs, _kLumacoeffs,
                            Vec3f(_kSatLumR, _kSatLumG, _kSatLumB));
    float gray = luma[0] * c[0] + luma[1] * c[1] + luma[2] * c[2];
    Vec3f g(gray);
    (*outputs)[_kOut] = Value(g + (c - g) * amount);
}

static void
_EvalSaturateColor4(const ParamMap& inputs, const ShadingContext&,
                    NodeOutputMap* outputs)
{
    Vec4f c = Get<Vec4f>(inputs, _kIn, Vec4f(0.0f));
    float amount = Get<float>(inputs, _kAmount, 1.0f);
    Vec3f luma = Get<Vec3f>(inputs, _kLumacoeffs,
                            Vec3f(_kSatLumR, _kSatLumG, _kSatLumB));
    float gray = luma[0] * c[0] + luma[1] * c[1] + luma[2] * c[2];
    float r = gray + (c[0] - gray) * amount;
    float g = gray + (c[1] - gray) * amount;
    float b = gray + (c[2] - gray) * amount;
    (*outputs)[_kOut] = Value(Vec4f(r, g, b, c[3]));
}

// ---- HSV Adjust ----------------------------------------------------------

static void
_EvalHsvadjustColor3(const ParamMap& inputs, const ShadingContext&,
                     NodeOutputMap* outputs)
{
    Vec3f c = Get<Vec3f>(inputs, _kIn, Vec3f(0.0f));
    Vec3f amount = Get<Vec3f>(inputs, _kAmount, Vec3f(0.0f, 1.0f, 1.0f));
    Vec3f hsv = RgbToHsv(c);
    // amount = (hue_add, sat_mul, val_mul)
    hsv[0] = std::fmod(hsv[0] + amount[0], 1.0f);
    if (hsv[0] < 0.0f) hsv[0] += 1.0f;
    hsv[1] *= amount[1];
    hsv[2] *= amount[2];
    (*outputs)[_kOut] = Value(HsvToRgb(hsv));
}

static void
_EvalHsvadjustColor4(const ParamMap& inputs, const ShadingContext&,
                     NodeOutputMap* outputs)
{
    Vec4f c = Get<Vec4f>(inputs, _kIn, Vec4f(0.0f));
    Vec3f amount = Get<Vec3f>(inputs, _kAmount, Vec3f(0.0f, 1.0f, 1.0f));
    Vec3f hsv = RgbToHsv(Vec3f(c[0], c[1], c[2]));
    hsv[0] = std::fmod(hsv[0] + amount[0], 1.0f);
    if (hsv[0] < 0.0f) hsv[0] += 1.0f;
    hsv[1] *= amount[1];
    hsv[2] *= amount[2];
    Vec3f rgb = HsvToRgb(hsv);
    (*outputs)[_kOut] = Value(Vec4f(rgb[0], rgb[1], rgb[2], c[3]));
}

// ---- Color Correct -------------------------------------------------------

static const SlotName _kHue("hue");
static const SlotName _kSaturation("saturation");
static const SlotName _kGamma("gamma");
static const SlotName _kLift("lift");
static const SlotName _kGain("gain");
static const SlotName _kContrast("contrast");
static const SlotName _kContrastpivot("contrastpivot");
static const SlotName _kExposure("exposure");

static Vec3f
_ColorCorrectRgb(Vec3f c, float hue, float saturation, float gamma,
                 float lift, float gain, float contrast,
                 float contrastpivot, float exposure)
{
    // 1. HSV adjust: hue rotation + saturation
    Vec3f hsv = RgbToHsv(c);
    hsv[0] = std::fmod(hsv[0] + hue, 1.0f);
    if (hsv[0] < 0.0f) hsv[0] += 1.0f;
    hsv[1] *= saturation;
    c = HsvToRgb(hsv);

    // 2. Exposure: multiply by 2^exposure
    float expMul = std::pow(2.0f, exposure);
    c *= expMul;

    // 3. Contrast
    c = Vec3f(contrastpivot) + (c - Vec3f(contrastpivot)) * contrast;

    // 4. Gamma (apply 1/gamma)
    if (gamma > 0.0f) {
        float invGamma = 1.0f / gamma;
        c[0] = c[0] > 0.0f ? std::pow(c[0], invGamma) : 0.0f;
        c[1] = c[1] > 0.0f ? std::pow(c[1], invGamma) : 0.0f;
        c[2] = c[2] > 0.0f ? std::pow(c[2], invGamma) : 0.0f;
    }

    // 5. Lift and Gain
    c = c * (Vec3f(1.0f) - Vec3f(lift)) + Vec3f(lift);
    c *= gain;

    return c;
}

static void
_EvalColorcorrectColor3(const ParamMap& inputs, const ShadingContext&,
                        NodeOutputMap* outputs)
{
    Vec3f c              = Get<Vec3f>(inputs, _kIn, Vec3f(1.0f));
    float hue            = Get<float>(inputs, _kHue, 0.0f);
    float saturation     = Get<float>(inputs, _kSaturation, 1.0f);
    float gamma          = Get<float>(inputs, _kGamma, 1.0f);
    float lift           = Get<float>(inputs, _kLift, 0.0f);
    float gain           = Get<float>(inputs, _kGain, 1.0f);
    float contrast       = Get<float>(inputs, _kContrast, 1.0f);
    float contrastpivot  = Get<float>(inputs, _kContrastpivot, 0.5f);
    float exposure       = Get<float>(inputs, _kExposure, 0.0f);

    (*outputs)[_kOut] = Value(
        _ColorCorrectRgb(c, hue, saturation, gamma,
                         lift, gain, contrast, contrastpivot, exposure));
}

static void
_EvalColorcorrectColor4(const ParamMap& inputs, const ShadingContext&,
                        NodeOutputMap* outputs)
{
    Vec4f c              = Get<Vec4f>(inputs, _kIn, Vec4f(1.0f, 1.0f, 1.0f, 0.0f));
    float hue            = Get<float>(inputs, _kHue, 0.0f);
    float saturation     = Get<float>(inputs, _kSaturation, 1.0f);
    float gamma          = Get<float>(inputs, _kGamma, 1.0f);
    float lift           = Get<float>(inputs, _kLift, 0.0f);
    float gain           = Get<float>(inputs, _kGain, 1.0f);
    float contrast       = Get<float>(inputs, _kContrast, 1.0f);
    float contrastpivot  = Get<float>(inputs, _kContrastpivot, 0.5f);
    float exposure       = Get<float>(inputs, _kExposure, 0.0f);

    Vec3f rgb = _ColorCorrectRgb(
        Vec3f(c[0], c[1], c[2]), hue, saturation, gamma,
        lift, gain, contrast, contrastpivot, exposure);
    (*outputs)[_kOut] = Value(Vec4f(rgb[0], rgb[1], rgb[2], c[3]));
}

// ---- Range ---------------------------------------------------------------

static const SlotName _kDoclamp("doclamp");

// Component-wise safe power: pow(max(v, 0), exponent).
static float _SafePow(float v, float e) {
    return (v > 0.0f && e != 0.0f) ? std::pow(v, e) : 0.0f;
}

template<typename T>
static T _RangeImpl(const T& v, const T& inLo, const T& inHi,
                    const T& gamma, const T& outLo, const T& outHi);

template<>
float
_RangeImpl<float>(const float& v, const float& inLo, const float& inHi,
                  const float& gamma, const float& outLo, const float& outHi)
{
    if (inHi == inLo) return outLo;
    float t = (v - inLo) / (inHi - inLo);
    t = (gamma != 1.0f) ? _SafePow(t, 1.0f / gamma) : t;
    return outLo + t * (outHi - outLo);
}

template<>
Vec2f
_RangeImpl<Vec2f>(const Vec2f& v, const Vec2f& inLo, const Vec2f& inHi,
                  const Vec2f& gamma, const Vec2f& outLo, const Vec2f& outHi)
{
    return Vec2f(
        _RangeImpl<float>(v[0], inLo[0], inHi[0], gamma[0], outLo[0], outHi[0]),
        _RangeImpl<float>(v[1], inLo[1], inHi[1], gamma[1], outLo[1], outHi[1]));
}

template<>
Vec3f
_RangeImpl<Vec3f>(const Vec3f& v, const Vec3f& inLo, const Vec3f& inHi,
                  const Vec3f& gamma, const Vec3f& outLo, const Vec3f& outHi)
{
    return Vec3f(
        _RangeImpl<float>(v[0], inLo[0], inHi[0], gamma[0], outLo[0], outHi[0]),
        _RangeImpl<float>(v[1], inLo[1], inHi[1], gamma[1], outLo[1], outHi[1]),
        _RangeImpl<float>(v[2], inLo[2], inHi[2], gamma[2], outLo[2], outHi[2]));
}

template<>
Vec4f
_RangeImpl<Vec4f>(const Vec4f& v, const Vec4f& inLo, const Vec4f& inHi,
                  const Vec4f& gamma, const Vec4f& outLo, const Vec4f& outHi)
{
    return Vec4f(
        _RangeImpl<float>(v[0], inLo[0], inHi[0], gamma[0], outLo[0], outHi[0]),
        _RangeImpl<float>(v[1], inLo[1], inHi[1], gamma[1], outLo[1], outHi[1]),
        _RangeImpl<float>(v[2], inLo[2], inHi[2], gamma[2], outLo[2], outHi[2]),
        _RangeImpl<float>(v[3], inLo[3], inHi[3], gamma[3], outLo[3], outHi[3]));
}

// Full-type range (all parameters match T).
template<typename T>
static void
_EvalRange(const ParamMap& inputs, const ShadingContext&,
           NodeOutputMap* outputs)
{
    T v     = Get<T>(inputs, _kIn,      Zero<T>());
    T inLo  = Get<T>(inputs, _kInlow,   Zero<T>());
    T inHi  = Get<T>(inputs, _kInhigh,  One<T>());
    T gamma = Get<T>(inputs, _kGamma,   One<T>());
    T outLo = Get<T>(inputs, _kOutlow,  Zero<T>());
    T outHi = Get<T>(inputs, _kOuthigh, One<T>());
    bool doClamp = Get<bool>(inputs, _kDoclamp, false);

    T result = _RangeImpl<T>(v, inLo, inHi, gamma, outLo, outHi);
    if (doClamp) {
        result = _Clamp(result, outLo, outHi);
    }
    (*outputs)[_kOut] = Value(result);
}

// Float-args (FA) range: range parameters are float, applied uniformly.
template<typename T>
static void
_EvalRangeFA(const ParamMap& inputs, const ShadingContext&,
             NodeOutputMap* outputs)
{
    T v         = Get<T>(inputs, _kIn,      Zero<T>());
    float inLo  = Get<float>(inputs, _kInlow,   0.0f);
    float inHi  = Get<float>(inputs, _kInhigh,  1.0f);
    float gamma = Get<float>(inputs, _kGamma,   1.0f);
    float outLo = Get<float>(inputs, _kOutlow,  0.0f);
    float outHi = Get<float>(inputs, _kOuthigh, 1.0f);
    bool doClamp = Get<bool>(inputs, _kDoclamp, false);

    T result = _RangeImpl<T>(v, T(inLo), T(inHi), T(gamma),
                             T(outLo), T(outHi));
    if (doClamp) {
        result = _Clamp(result, T(outLo), T(outHi));
    }
    (*outputs)[_kOut] = Value(result);
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

    _REG("ND_contrast_float",  &_EvalContrastFloat);
    _REG("ND_contrast_color3", &_EvalContrast<Vec3f>);
    _REG("ND_contrast_color4", &_EvalContrast<Vec4f>);

    // Saturate
    _REG("ND_saturate_color3", &_EvalSaturateColor3);
    _REG("ND_saturate_color4", &_EvalSaturateColor4);

    // HSV Adjust
    _REG("ND_hsvadjust_color3", &_EvalHsvadjustColor3);
    _REG("ND_hsvadjust_color4", &_EvalHsvadjustColor4);

    // Color Correct
    _REG("ND_colorcorrect_color3", &_EvalColorcorrectColor3);
    _REG("ND_colorcorrect_color4", &_EvalColorcorrectColor4);

    // Range
    _REG("ND_range_float",      &_EvalRange<float>);
    _REG("ND_range_color3",     &_EvalRange<Vec3f>);
    _REG("ND_range_color4",     &_EvalRange<Vec4f>);
    _REG("ND_range_vector2",    &_EvalRange<Vec2f>);
    _REG("ND_range_vector3",    &_EvalRange<Vec3f>);
    _REG("ND_range_vector4",    &_EvalRange<Vec4f>);
    _REG("ND_range_color3FA",   &_EvalRangeFA<Vec3f>);
    _REG("ND_range_color4FA",   &_EvalRangeFA<Vec4f>);
    _REG("ND_range_vector2FA",  &_EvalRangeFA<Vec2f>);
    _REG("ND_range_vector3FA",  &_EvalRangeFA<Vec3f>);
    _REG("ND_range_vector4FA",  &_EvalRangeFA<Vec4f>);
}

#undef _REG

}  // namespace mxcpp
