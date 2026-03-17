//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "colorNodes.h"
#include "../nodeRegistry.h"

#include <cmath>
#include <algorithm>
#include <string>

namespace mxcpp {

static const SlotName _kIn("in");
static const SlotName _kOut("out");

// Rec.709 luminance weights.
static constexpr float _kLumR = 0.2126f;
static constexpr float _kLumG = 0.7152f;
static constexpr float _kLumB = 0.0722f;

static void
_EvalLuminance(const ParamMap& inputs, const ShadingContext&,
               NodeOutputMap* outputs)
{
    Vec3f c = Get<Vec3f>(inputs, _kIn, Vec3f(0.0f));
    (*outputs)[_kOut] = Value(
        _kLumR * c[0] + _kLumG * c[1] + _kLumB * c[2]);
}

// RGB <-> HSV conversion following the standard algorithm.
namespace {

Vec3f
_RgbToHsv(const Vec3f& rgb)
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
    return Vec3f(h, s, cmax);
}

Vec3f
_HsvToRgb(const Vec3f& hsv)
{
    float h = hsv[0], s = hsv[1], v = hsv[2];
    float c = v * s;
    float hh = h * 6.0f;
    float x = c * (1.0f - std::fabs(std::fmod(hh, 2.0f) - 1.0f));
    float m = v - c;

    Vec3f rgb;
    if      (hh < 1.0f) rgb = Vec3f(c, x, 0);
    else if (hh < 2.0f) rgb = Vec3f(x, c, 0);
    else if (hh < 3.0f) rgb = Vec3f(0, c, x);
    else if (hh < 4.0f) rgb = Vec3f(0, x, c);
    else if (hh < 5.0f) rgb = Vec3f(x, 0, c);
    else                 rgb = Vec3f(c, 0, x);

    return rgb + Vec3f(m);
}

}  // anonymous namespace

static void
_EvalRgbToHsv(const ParamMap& inputs, const ShadingContext&,
              NodeOutputMap* outputs)
{
    Vec3f rgb = Get<Vec3f>(inputs, _kIn, Vec3f(0.0f));
    (*outputs)[_kOut] = Value(_RgbToHsv(rgb));
}

static void
_EvalHsvToRgb(const ParamMap& inputs, const ShadingContext&,
              NodeOutputMap* outputs)
{
    Vec3f hsv = Get<Vec3f>(inputs, _kIn, Vec3f(0.0f));
    (*outputs)[_kOut] = Value(_HsvToRgb(hsv));
}

// ---- Registration --------------------------------------------------------

#define _REG(name, fn) reg.Register(name, fn)

void
RegisterColorNodes(NodeRegistry& reg)
{
    _REG("ND_luminance_color3", &_EvalLuminance);
    _REG("ND_rgbtohsv_color3",  &_EvalRgbToHsv);
    _REG("ND_hsvtorgb_color3",  &_EvalHsvToRgb);
}

#undef _REG

}  // namespace mxcpp
