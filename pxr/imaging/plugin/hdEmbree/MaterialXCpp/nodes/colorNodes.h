//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef MXCPP_NODES_COLOR_NODES_H
#define MXCPP_NODES_COLOR_NODES_H

#include "../mxcpp_math.h"
#include <algorithm>
#include <cmath>

namespace mxcpp {

class NodeRegistry;
void RegisterColorNodes(NodeRegistry& reg);

// RGB <-> HSV conversion helpers shared across color/adjustment nodes.

inline Vec3f
RgbToHsv(const Vec3f& rgb)
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

inline Vec3f
HsvToRgb(const Vec3f& hsv)
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

}  // namespace mxcpp

#endif
