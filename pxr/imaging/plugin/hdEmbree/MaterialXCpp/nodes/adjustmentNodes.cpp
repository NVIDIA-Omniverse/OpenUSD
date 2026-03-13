//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/imaging/plugin/hdEmbree/MaterialXCpp/nodes/adjustmentNodes.h"
#include "pxr/imaging/plugin/hdEmbree/MaterialXCpp/nodeRegistry.h"

#include "pxr/base/tf/staticTokens.h"

#include <cmath>
#include <algorithm>

PXR_NAMESPACE_OPEN_SCOPE
namespace mxcpp {

TF_DEFINE_PRIVATE_TOKENS(_tokens,
    (in)
    (in1)
    (in2)
    (fg)
    (bg)
    (mix)
    (low)
    (high)
    (inlow)
    (inhigh)
    (outlow)
    (outhigh)
    (out)
    (amount)
    (center)
);

// ---- Clamp / Min / Max ---------------------------------------------------

template<typename T>
static T _Clamp(const T& v, const T& lo, const T& hi);

template<>
float _Clamp<float>(const float& v, const float& lo, const float& hi) {
    return std::clamp(v, lo, hi);
}
template<>
GfVec3f _Clamp<GfVec3f>(const GfVec3f& v, const GfVec3f& lo, const GfVec3f& hi) {
    return GfVec3f(std::clamp(v[0], lo[0], hi[0]),
                   std::clamp(v[1], lo[1], hi[1]),
                   std::clamp(v[2], lo[2], hi[2]));
}
template<>
GfVec4f _Clamp<GfVec4f>(const GfVec4f& v, const GfVec4f& lo, const GfVec4f& hi) {
    return GfVec4f(std::clamp(v[0], lo[0], hi[0]),
                   std::clamp(v[1], lo[1], hi[1]),
                   std::clamp(v[2], lo[2], hi[2]),
                   std::clamp(v[3], lo[3], hi[3]));
}

template<typename T>
static void
_EvalClamp(const ParamMap& inputs, const ShadingContext&,
           NodeOutputMap* outputs)
{
    T v  = Get<T>(inputs, _tokens->in,   Zero<T>());
    T lo = Get<T>(inputs, _tokens->low,  Zero<T>());
    T hi = Get<T>(inputs, _tokens->high, One<T>());
    (*outputs)[_tokens->out] = VtValue(_Clamp(v, lo, hi));
}

template<typename T>
static void
_EvalMin(const ParamMap& inputs, const ShadingContext&,
         NodeOutputMap* outputs)
{
    T a = Get<T>(inputs, _tokens->in1, Zero<T>());
    T b = Get<T>(inputs, _tokens->in2, Zero<T>());
    // Component-wise min for vectors is not needed for most use cases;
    // scalar min is the primary use.
    (*outputs)[_tokens->out] = VtValue(a < b ? a : b);
}

// Scalar-only min/max are sufficient for most MaterialX usage.
static void
_EvalMinFloat(const ParamMap& inputs, const ShadingContext&,
              NodeOutputMap* outputs)
{
    float a = Get<float>(inputs, TfToken("in1"), 0.0f);
    float b = Get<float>(inputs, TfToken("in2"), 0.0f);
    (*outputs)[_tokens->out] = VtValue(std::min(a, b));
}

static void
_EvalMaxFloat(const ParamMap& inputs, const ShadingContext&,
              NodeOutputMap* outputs)
{
    float a = Get<float>(inputs, TfToken("in1"), 0.0f);
    float b = Get<float>(inputs, TfToken("in2"), 0.0f);
    (*outputs)[_tokens->out] = VtValue(std::max(a, b));
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
    T v  = Get<T>(inputs, _tokens->in,     Zero<T>());
    T il = Get<T>(inputs, _tokens->inlow,  Zero<T>());
    T ih = Get<T>(inputs, _tokens->inhigh, One<T>());
    T ol = Get<T>(inputs, _tokens->outlow, Zero<T>());
    T oh = Get<T>(inputs, _tokens->outhigh, One<T>());
    // Scalar remap for float type.
    (*outputs)[_tokens->out] = VtValue(
        _Remap(Get<float>(inputs, _tokens->in, 0.0f),
               Get<float>(inputs, _tokens->inlow, 0.0f),
               Get<float>(inputs, _tokens->inhigh, 1.0f),
               Get<float>(inputs, _tokens->outlow, 0.0f),
               Get<float>(inputs, _tokens->outhigh, 1.0f)));
}

// Specialization for float remap
static void
_EvalRemapFloat(const ParamMap& inputs, const ShadingContext&,
                NodeOutputMap* outputs)
{
    float v  = Get<float>(inputs, _tokens->in,     0.0f);
    float il = Get<float>(inputs, _tokens->inlow,  0.0f);
    float ih = Get<float>(inputs, _tokens->inhigh, 1.0f);
    float ol = Get<float>(inputs, _tokens->outlow, 0.0f);
    float oh = Get<float>(inputs, _tokens->outhigh, 1.0f);
    (*outputs)[_tokens->out] = VtValue(_Remap(v, il, ih, ol, oh));
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
    float v  = Get<float>(inputs, _tokens->in,   0.0f);
    float lo = Get<float>(inputs, _tokens->low,  0.0f);
    float hi = Get<float>(inputs, _tokens->high, 1.0f);
    (*outputs)[_tokens->out] = VtValue(_Smoothstep(lo, hi, v));
}

// ---- Mix -----------------------------------------------------------------

template<typename T>
static void
_EvalMix(const ParamMap& inputs, const ShadingContext&,
         NodeOutputMap* outputs)
{
    T fg  = Get<T>(inputs, _tokens->fg, Zero<T>());
    T bg  = Get<T>(inputs, _tokens->bg, Zero<T>());
    float m = Get<float>(inputs, _tokens->mix, 0.0f);
    (*outputs)[_tokens->out] = VtValue(bg + (fg - bg) * m);
}

// ---- Contrast ------------------------------------------------------------

template<typename T>
static void
_EvalContrast(const ParamMap& inputs, const ShadingContext&,
              NodeOutputMap* outputs)
{
    T v      = Get<T>(inputs, _tokens->in,     Zero<T>());
    T amount = Get<T>(inputs, _tokens->amount, One<T>());
    T pivot  = Get<T>(inputs, _tokens->center, T(0.5f));
    // contrast = pivot + (v - pivot) * amount
    (*outputs)[_tokens->out] = VtValue(
        pivot + CompMul(v - pivot, amount));
}

static void
_EvalContrastFloat(const ParamMap& inputs, const ShadingContext&,
                   NodeOutputMap* outputs)
{
    float v      = Get<float>(inputs, _tokens->in,     0.0f);
    float amount = Get<float>(inputs, _tokens->amount, 1.0f);
    float pivot  = Get<float>(inputs, _tokens->center, 0.5f);
    (*outputs)[_tokens->out] = VtValue(pivot + (v - pivot) * amount);
}

// ---- Premult / Unpremult -------------------------------------------------

static void
_EvalPremult(const ParamMap& inputs, const ShadingContext&,
             NodeOutputMap* outputs)
{
    GfVec4f v = Get<GfVec4f>(inputs, _tokens->in, GfVec4f(0.0f));
    (*outputs)[_tokens->out] = VtValue(
        GfVec4f(v[0]*v[3], v[1]*v[3], v[2]*v[3], v[3]));
}

static void
_EvalUnpremult(const ParamMap& inputs, const ShadingContext&,
               NodeOutputMap* outputs)
{
    GfVec4f v = Get<GfVec4f>(inputs, _tokens->in, GfVec4f(0.0f));
    float a = v[3] > 0.0f ? 1.0f / v[3] : 0.0f;
    (*outputs)[_tokens->out] = VtValue(
        GfVec4f(v[0]*a, v[1]*a, v[2]*a, v[3]));
}

// ---- Registration --------------------------------------------------------

#define _REG(name, fn) reg.Register(TfToken(name), fn)

void
RegisterAdjustmentNodes(NodeRegistry& reg)
{
    _REG("ND_clamp_float",   &_EvalClamp<float>);
    _REG("ND_clamp_color3",  &_EvalClamp<GfVec3f>);
    _REG("ND_clamp_color4",  &_EvalClamp<GfVec4f>);
    _REG("ND_clamp_vector3", &_EvalClamp<GfVec3f>);

    _REG("ND_min_float", &_EvalMinFloat);
    _REG("ND_max_float", &_EvalMaxFloat);

    _REG("ND_remap_float", &_EvalRemapFloat);
    _REG("ND_smoothstep_float", &_EvalSmoothstep);

    _REG("ND_mix_float",   &_EvalMix<float>);
    _REG("ND_mix_color3",  &_EvalMix<GfVec3f>);
    _REG("ND_mix_color4",  &_EvalMix<GfVec4f>);
    _REG("ND_mix_vector2", &_EvalMix<GfVec2f>);
    _REG("ND_mix_vector3", &_EvalMix<GfVec3f>);
    _REG("ND_mix_vector4", &_EvalMix<GfVec4f>);

    _REG("ND_contrast_float",  &_EvalContrastFloat);
    _REG("ND_contrast_color3", &_EvalContrast<GfVec3f>);
    _REG("ND_contrast_color4", &_EvalContrast<GfVec4f>);

    _REG("ND_premult_color4",   &_EvalPremult);
    _REG("ND_unpremult_color4", &_EvalUnpremult);
}

#undef _REG

} // namespace mxcpp
PXR_NAMESPACE_CLOSE_SCOPE
