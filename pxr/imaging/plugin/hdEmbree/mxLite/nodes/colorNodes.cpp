//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/imaging/plugin/hdEmbree/mxLite/nodes/colorNodes.h"
#include "pxr/imaging/plugin/hdEmbree/mxLite/nodeRegistry.h"

#include "pxr/base/tf/staticTokens.h"

#include <cmath>
#include <algorithm>

PXR_NAMESPACE_OPEN_SCOPE

TF_DEFINE_PRIVATE_TOKENS(_tokens,
    (in)
    (out)
);

// Rec.709 luminance weights.
static constexpr float _kLumR = 0.2126f;
static constexpr float _kLumG = 0.7152f;
static constexpr float _kLumB = 0.0722f;

static void
_EvalLuminance(const MxLiteParamMap& inputs, const MxLiteShadingContext&,
               MxLiteNodeOutputMap* outputs)
{
    GfVec3f c = MxLiteGet<GfVec3f>(inputs, _tokens->in, GfVec3f(0.0f));
    (*outputs)[_tokens->out] = VtValue(
        _kLumR * c[0] + _kLumG * c[1] + _kLumB * c[2]);
}

// RGB <-> HSV conversion following the standard algorithm.
namespace {

GfVec3f
_RgbToHsv(const GfVec3f& rgb)
{
    float r = rgb[0], g = rgb[1], b = rgb[2];
    float cmax = std::max({r, g, b});
    float cmin = std::min({r, g, b});
    float delta = cmax - cmin;

    float h = 0.0f;
    if (delta > 0.0f) {
        if (cmax == r) {
            h = std::fmod((g - b) / delta, 6.0f);
        } else if (cmax == g) {
            h = (b - r) / delta + 2.0f;
        } else {
            h = (r - g) / delta + 4.0f;
        }
        h /= 6.0f;
        if (h < 0.0f) h += 1.0f;
    }

    float s = cmax > 0.0f ? delta / cmax : 0.0f;
    return GfVec3f(h, s, cmax);
}

GfVec3f
_HsvToRgb(const GfVec3f& hsv)
{
    float h = hsv[0], s = hsv[1], v = hsv[2];
    float c = v * s;
    float hh = h * 6.0f;
    float x = c * (1.0f - std::fabs(std::fmod(hh, 2.0f) - 1.0f));
    float m = v - c;

    GfVec3f rgb;
    if      (hh < 1.0f) rgb = GfVec3f(c, x, 0);
    else if (hh < 2.0f) rgb = GfVec3f(x, c, 0);
    else if (hh < 3.0f) rgb = GfVec3f(0, c, x);
    else if (hh < 4.0f) rgb = GfVec3f(0, x, c);
    else if (hh < 5.0f) rgb = GfVec3f(x, 0, c);
    else                 rgb = GfVec3f(c, 0, x);

    return rgb + GfVec3f(m);
}

} // anonymous namespace

static void
_EvalRgbToHsv(const MxLiteParamMap& inputs, const MxLiteShadingContext&,
              MxLiteNodeOutputMap* outputs)
{
    GfVec3f rgb = MxLiteGet<GfVec3f>(inputs, _tokens->in, GfVec3f(0.0f));
    (*outputs)[_tokens->out] = VtValue(_RgbToHsv(rgb));
}

static void
_EvalHsvToRgb(const MxLiteParamMap& inputs, const MxLiteShadingContext&,
              MxLiteNodeOutputMap* outputs)
{
    GfVec3f hsv = MxLiteGet<GfVec3f>(inputs, _tokens->in, GfVec3f(0.0f));
    (*outputs)[_tokens->out] = VtValue(_HsvToRgb(hsv));
}

// ---- Registration --------------------------------------------------------

#define _REG(name, fn) reg.Register(TfToken(name), fn)

void
MxLiteRegisterColorNodes(MxLiteNodeRegistry& reg)
{
    _REG("ND_luminance_color3", &_EvalLuminance);
    _REG("ND_rgbtohsv_color3",  &_EvalRgbToHsv);
    _REG("ND_hsvtorgb_color3",  &_EvalHsvToRgb);
}

#undef _REG

PXR_NAMESPACE_CLOSE_SCOPE
