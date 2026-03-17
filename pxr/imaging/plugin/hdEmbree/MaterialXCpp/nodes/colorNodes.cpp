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

static void
_EvalRgbToHsv(const ParamMap& inputs, const ShadingContext&,
              NodeOutputMap* outputs)
{
    Vec3f rgb = Get<Vec3f>(inputs, _kIn, Vec3f(0.0f));
    (*outputs)[_kOut] = Value(RgbToHsv(rgb));
}

static void
_EvalHsvToRgb(const ParamMap& inputs, const ShadingContext&,
              NodeOutputMap* outputs)
{
    Vec3f hsv = Get<Vec3f>(inputs, _kIn, Vec3f(0.0f));
    (*outputs)[_kOut] = Value(HsvToRgb(hsv));
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
