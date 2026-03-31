//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "proceduralNodes.h"
#include "colorNodes.h"
#include "../nodeRegistry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <type_traits>

namespace mxcpp {

static const SlotName _kOut("out");
static const SlotName _kTexcoord("texcoord");
static const SlotName _kPosition("position");
static const SlotName _kAmplitude("amplitude");
static const SlotName _kPivot("pivot");
static const SlotName _kOctaves("octaves");
static const SlotName _kLacunarity("lacunarity");
static const SlotName _kDiminish("diminish");
static const SlotName _kJitter("jitter");
static const SlotName _kStyle("style");
static const SlotName _kValuel("valuel");
static const SlotName _kValuer("valuer");
static const SlotName _kValuet("valuet");
static const SlotName _kValueb("valueb");
static const SlotName _kCenter("center");
static const SlotName _kValuetl("valuetl");
static const SlotName _kValuetr("valuetr");
static const SlotName _kValuebl("valuebl");
static const SlotName _kValuebr("valuebr");
static const SlotName _kType("type");
static const SlotName _kInterpolation("interpolation");
static const SlotName _kNumIntervals("num_intervals");
static const SlotName _kX("x");
static const SlotName _kPrevColor("prev_color");
static const SlotName _kIntervalNum("interval_num");
static const SlotName _kFreq("freq");
static const SlotName _kOffset("offset");
static const SlotName _kOutmin("outmin");
static const SlotName _kOutmax("outmax");
static const SlotName _kClampoutput("clampoutput");
static const SlotName _kMin("min");
static const SlotName _kMax("max");
static const SlotName _kIn("in");
static const SlotName _kSeed("seed");
static const SlotName _kHuelow("huelow");
static const SlotName _kHuehigh("huehigh");
static const SlotName _kSaturationlow("saturationlow");
static const SlotName _kSaturationhigh("saturationhigh");
static const SlotName _kBrightnesslow("brightnesslow");
static const SlotName _kBrightnesshigh("brightnesshigh");
static const SlotName _kColor1("color1");
static const SlotName _kColor2("color2");
static const SlotName _kColor3("color3");
static const SlotName _kColor4("color4");
static const SlotName _kColor5("color5");
static const SlotName _kColor6("color6");
static const SlotName _kColor7("color7");
static const SlotName _kColor8("color8");
static const SlotName _kColor9("color9");
static const SlotName _kColor10("color10");
static const SlotName _kInterval1("interval1");
static const SlotName _kInterval2("interval2");
static const SlotName _kInterval3("interval3");
static const SlotName _kInterval4("interval4");
static const SlotName _kInterval5("interval5");
static const SlotName _kInterval6("interval6");
static const SlotName _kInterval7("interval7");
static const SlotName _kInterval8("interval8");
static const SlotName _kInterval9("interval9");
static const SlotName _kInterval10("interval10");
static const SlotName _kUvtiling("uvtiling");
static const SlotName _kUvoffset("uvoffset");
static const SlotName _kThickness("thickness");
static const SlotName _kStaggered("staggered");
static const SlotName _kRadius("radius");
static const SlotName _kPoint1("point1");
static const SlotName _kPoint2("point2");
static const SlotName _kSize("size");
static const SlotName _kRoughness("roughness");
static const SlotName _kCoverage("coverage");
static const SlotName _kNormal("normal");
static const SlotName _kTangent("tangent");
static const SlotName _kBitangent("bitangent");
static const SlotName _kId("id");
static const SlotName _kRand("rand");
static const SlotName _kPresence("presence");
static const SlotName _kFlakenormal("flakenormal");

namespace {

constexpr float _kPi = 3.14159265358979323846f;
constexpr float _kTwoPi = 6.28318530717958647692f;
constexpr float _kAAStepScale = 0.70710678118654757f;

inline float
_ClampFloat(float v, float lo, float hi)
{
    return std::clamp(v, lo, hi);
}

inline float
_Clamp01(float v)
{
    return _ClampFloat(v, 0.0f, 1.0f);
}

inline Vec2f
_Clamp01(const Vec2f& v)
{
    return Vec2f(_Clamp01(v[0]), _Clamp01(v[1]));
}

template<typename T>
inline T
_Mix(const T& bg, const T& fg, float t)
{
    return bg + (fg - bg) * t;
}

inline float
_Smoothstep(float lo, float hi, float v)
{
    if (hi <= lo) {
        return 0.0f;
    }
    const float t = _Clamp01((v - lo) / (hi - lo));
    return t * t * (3.0f - 2.0f * t);
}

inline float
_AAStep(float threshold, float value, float dx, float dy)
{
    const float afwidth = std::sqrt(dx * dx + dy * dy) * _kAAStepScale;
    if (afwidth <= 0.0f) {
        return value < threshold ? 0.0f : 1.0f;
    }
    return _Smoothstep(threshold - afwidth, threshold + afwidth, value);
}

inline float
_PositiveMod(float x, float y = 1.0f)
{
    if (y == 0.0f) {
        return 0.0f;
    }
    return x - y * std::floor(x / y);
}

inline float
_Remap(float v, float inLo, float inHi, float outLo, float outHi)
{
    if (inHi == inLo) {
        return outLo;
    }
    const float t = (v - inLo) / (inHi - inLo);
    return outLo + t * (outHi - outLo);
}

inline float
_Lerp(float a, float b, float t)
{
    return a + t * (b - a);
}

inline Vec2f
_Rotate2d(const Vec2f& v, float amountDegrees)
{
    const float rad = amountDegrees * (_kPi / 180.0f);
    const float c = std::cos(rad);
    const float s = std::sin(rad);
    return Vec2f(v[0] * c - v[1] * s,
                 v[0] * s + v[1] * c);
}

// -----------------------------------------------------------------------
// MaterialX-compatible Perlin / cell / Worley / flake helpers.
// Ported from MaterialX's mx_noise.glsl and mx_flake.glsl.
// -----------------------------------------------------------------------

inline uint32_t
_Rotl32(uint32_t x, int k)
{
    return (x << k) | (x >> (32 - k));
}

inline void
_BJMix(uint32_t& a, uint32_t& b, uint32_t& c)
{
    a -= c; a ^= _Rotl32(c, 4); c += b;
    b -= a; b ^= _Rotl32(a, 6); a += c;
    c -= b; c ^= _Rotl32(b, 8); b += a;
    a -= c; a ^= _Rotl32(c,16); c += b;
    b -= a; b ^= _Rotl32(a,19); a += c;
    c -= b; c ^= _Rotl32(b, 4); b += a;
}

inline uint32_t
_BJFinal(uint32_t a, uint32_t b, uint32_t c)
{
    c ^= b; c -= _Rotl32(b,14);
    a ^= c; a -= _Rotl32(c,11);
    b ^= a; b -= _Rotl32(a,25);
    c ^= b; c -= _Rotl32(b,16);
    a ^= c; a -= _Rotl32(c, 4);
    b ^= a; b -= _Rotl32(a,14);
    c ^= b; c -= _Rotl32(b,24);
    return c;
}

inline uint32_t _HashInt(int x) {
    const uint32_t len = 1u;
    const uint32_t seed = 0xdeadbeefu + (len << 2u) + 13u;
    return _BJFinal(seed + uint32_t(x), seed, seed);
}

inline uint32_t _HashInt(int x, int y) {
    const uint32_t len = 2u;
    uint32_t a = 0xdeadbeefu + (len << 2u) + 13u;
    uint32_t b = a;
    uint32_t c = a;
    a += uint32_t(x);
    b += uint32_t(y);
    return _BJFinal(a, b, c);
}

inline uint32_t _HashInt(int x, int y, int z) {
    const uint32_t len = 3u;
    uint32_t a = 0xdeadbeefu + (len << 2u) + 13u;
    uint32_t b = a;
    uint32_t c = a;
    a += uint32_t(x);
    b += uint32_t(y);
    c += uint32_t(z);
    return _BJFinal(a, b, c);
}

inline uint32_t _HashInt(int x, int y, int z, int xx) {
    uint32_t a = 0xdeadbeefu + (4u << 2u) + 13u;
    uint32_t b = a;
    uint32_t c = a;
    a += uint32_t(x);
    b += uint32_t(y);
    c += uint32_t(z);
    _BJMix(a, b, c);
    a += uint32_t(xx);
    return _BJFinal(a, b, c);
}

struct _UVec3 {
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t z = 0;
};

inline _UVec3
_HashVec3(int x, int y)
{
    const uint32_t h = _HashInt(x, y);
    return {h & 0xFFu, (h >> 8) & 0xFFu, (h >> 16) & 0xFFu};
}

inline _UVec3
_HashVec3(int x, int y, int z)
{
    const uint32_t h = _HashInt(x, y, z);
    return {h & 0xFFu, (h >> 8) & 0xFFu, (h >> 16) & 0xFFu};
}

inline float
_BitsTo01(uint32_t bits)
{
    return float(bits) / float(0xFFFFFFFFu);
}

inline float
_GradientFloat(uint32_t hash, float x, float y)
{
    const uint32_t h = hash & 7u;
    const float u = (h < 4u) ? x : y;
    const float v = 2.0f * ((h < 4u) ? y : x);
    return ((h & 1u) ? -u : u) + ((h & 2u) ? -v : v);
}

inline float
_GradientFloat(uint32_t hash, float x, float y, float z)
{
    const uint32_t h = hash & 15u;
    const float u = (h < 8u) ? x : y;
    const float v = (h < 4u) ? y : ((h == 12u || h == 14u) ? x : z);
    return ((h & 1u) ? -u : u) + ((h & 2u) ? -v : v);
}

inline Vec3f
_GradientVec3(_UVec3 hash, float x, float y)
{
    return Vec3f(_GradientFloat(hash.x, x, y),
                 _GradientFloat(hash.y, x, y),
                 _GradientFloat(hash.z, x, y));
}

inline Vec3f
_GradientVec3(_UVec3 hash, float x, float y, float z)
{
    return Vec3f(_GradientFloat(hash.x, x, y, z),
                 _GradientFloat(hash.y, x, y, z),
                 _GradientFloat(hash.z, x, y, z));
}

constexpr float _kGradScale2d = 0.6616f;
constexpr float _kGradScale3d = 0.9820f;

inline float
_Fade(float t)
{
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

inline float
_Bilerp(float v0, float v1, float v2, float v3, float s, float t)
{
    const float s1 = 1.0f - s;
    return (1.0f - t) * (v0 * s1 + v1 * s) + t * (v2 * s1 + v3 * s);
}

inline Vec3f
_Bilerp(Vec3f v0, Vec3f v1, Vec3f v2, Vec3f v3, float s, float t)
{
    const float s1 = 1.0f - s;
    return (v0 * s1 + v1 * s) * (1.0f - t) + (v2 * s1 + v3 * s) * t;
}

inline float
_Trilerp(float v0, float v1, float v2, float v3,
         float v4, float v5, float v6, float v7,
         float s, float t, float r)
{
    const float s1 = 1.0f - s;
    const float t1 = 1.0f - t;
    const float r1 = 1.0f - r;
    return r1 * (t1 * (v0 * s1 + v1 * s) + t * (v2 * s1 + v3 * s)) +
           r  * (t1 * (v4 * s1 + v5 * s) + t * (v6 * s1 + v7 * s));
}

inline Vec3f
_Trilerp(Vec3f v0, Vec3f v1, Vec3f v2, Vec3f v3,
         Vec3f v4, Vec3f v5, Vec3f v6, Vec3f v7,
         float s, float t, float r)
{
    const float s1 = 1.0f - s;
    const float t1 = 1.0f - t;
    const float r1 = 1.0f - r;
    return (v0 * s1 + v1 * s) * (t1 * r1) +
           (v2 * s1 + v3 * s) * (t  * r1) +
           (v4 * s1 + v5 * s) * (t1 * r ) +
           (v6 * s1 + v7 * s) * (t  * r );
}

inline float
_FloorFrac(float x, int& i)
{
    i = static_cast<int>(std::floor(x));
    return x - float(i);
}

float
_PerlinNoise2d(float px, float py)
{
    int X = 0;
    int Y = 0;
    const float fx = _FloorFrac(px, X);
    const float fy = _FloorFrac(py, Y);
    const float u = _Fade(fx);
    const float v = _Fade(fy);
    return _kGradScale2d * _Bilerp(
        _GradientFloat(_HashInt(X,   Y  ), fx,      fy),
        _GradientFloat(_HashInt(X+1, Y  ), fx-1.0f, fy),
        _GradientFloat(_HashInt(X,   Y+1), fx,      fy-1.0f),
        _GradientFloat(_HashInt(X+1, Y+1), fx-1.0f, fy-1.0f),
        u, v);
}

Vec3f
_PerlinNoise2dVec3(float px, float py)
{
    int X = 0;
    int Y = 0;
    const float fx = _FloorFrac(px, X);
    const float fy = _FloorFrac(py, Y);
    const float u = _Fade(fx);
    const float v = _Fade(fy);
    return _Bilerp(
        _GradientVec3(_HashVec3(X,   Y  ), fx,      fy),
        _GradientVec3(_HashVec3(X+1, Y  ), fx-1.0f, fy),
        _GradientVec3(_HashVec3(X,   Y+1), fx,      fy-1.0f),
        _GradientVec3(_HashVec3(X+1, Y+1), fx-1.0f, fy-1.0f),
        u, v) * _kGradScale2d;
}

float
_PerlinNoise3d(float px, float py, float pz)
{
    int X = 0;
    int Y = 0;
    int Z = 0;
    const float fx = _FloorFrac(px, X);
    const float fy = _FloorFrac(py, Y);
    const float fz = _FloorFrac(pz, Z);
    const float u = _Fade(fx);
    const float v = _Fade(fy);
    const float w = _Fade(fz);
    return _kGradScale3d * _Trilerp(
        _GradientFloat(_HashInt(X,   Y,   Z  ), fx,      fy,      fz),
        _GradientFloat(_HashInt(X+1, Y,   Z  ), fx-1.0f, fy,      fz),
        _GradientFloat(_HashInt(X,   Y+1, Z  ), fx,      fy-1.0f, fz),
        _GradientFloat(_HashInt(X+1, Y+1, Z  ), fx-1.0f, fy-1.0f, fz),
        _GradientFloat(_HashInt(X,   Y,   Z+1), fx,      fy,      fz-1.0f),
        _GradientFloat(_HashInt(X+1, Y,   Z+1), fx-1.0f, fy,      fz-1.0f),
        _GradientFloat(_HashInt(X,   Y+1, Z+1), fx,      fy-1.0f, fz-1.0f),
        _GradientFloat(_HashInt(X+1, Y+1, Z+1), fx-1.0f, fy-1.0f, fz-1.0f),
        u, v, w);
}

Vec3f
_PerlinNoise3dVec3(float px, float py, float pz)
{
    int X = 0;
    int Y = 0;
    int Z = 0;
    const float fx = _FloorFrac(px, X);
    const float fy = _FloorFrac(py, Y);
    const float fz = _FloorFrac(pz, Z);
    const float u = _Fade(fx);
    const float v = _Fade(fy);
    const float w = _Fade(fz);
    return _Trilerp(
        _GradientVec3(_HashVec3(X,   Y,   Z  ), fx,      fy,      fz),
        _GradientVec3(_HashVec3(X+1, Y,   Z  ), fx-1.0f, fy,      fz),
        _GradientVec3(_HashVec3(X,   Y+1, Z  ), fx,      fy-1.0f, fz),
        _GradientVec3(_HashVec3(X+1, Y+1, Z  ), fx-1.0f, fy-1.0f, fz),
        _GradientVec3(_HashVec3(X,   Y,   Z+1), fx,      fy,      fz-1.0f),
        _GradientVec3(_HashVec3(X+1, Y,   Z+1), fx-1.0f, fy,      fz-1.0f),
        _GradientVec3(_HashVec3(X,   Y+1, Z+1), fx,      fy-1.0f, fz-1.0f),
        _GradientVec3(_HashVec3(X+1, Y+1, Z+1), fx-1.0f, fy-1.0f, fz-1.0f),
        u, v, w) * _kGradScale3d;
}

float
_CellNoise2d(float x, float y)
{
    return _BitsTo01(_HashInt(static_cast<int>(std::floor(x)),
                              static_cast<int>(std::floor(y))));
}

float
_CellNoise3d(float x, float y, float z)
{
    return _BitsTo01(_HashInt(static_cast<int>(std::floor(x)),
                              static_cast<int>(std::floor(y)),
                              static_cast<int>(std::floor(z))));
}

Vec3f
_CellNoise2dVec3(float x, float y)
{
    const int ix = static_cast<int>(std::floor(x));
    const int iy = static_cast<int>(std::floor(y));
    return Vec3f(_BitsTo01(_HashInt(ix, iy, 0)),
                 _BitsTo01(_HashInt(ix, iy, 1)),
                 _BitsTo01(_HashInt(ix, iy, 2)));
}

float
_FractalNoise2dFloat(Vec2f p, int octaves, float lacunarity, float diminish)
{
    float result = 0.0f;
    float amplitude = 1.0f;
    for (int i = 0; i < octaves; ++i) {
        result += amplitude * _PerlinNoise2d(p[0], p[1]);
        amplitude *= diminish;
        p *= lacunarity;
    }
    return result;
}

Vec3f
_FractalNoise2dVec3(Vec2f p, int octaves, float lacunarity, float diminish)
{
    Vec3f result(0.0f);
    float amplitude = 1.0f;
    for (int i = 0; i < octaves; ++i) {
        result += _PerlinNoise2dVec3(p[0], p[1]) * amplitude;
        amplitude *= diminish;
        p *= lacunarity;
    }
    return result;
}

Vec2f
_FractalNoise2dVec2(Vec2f p, int octaves, float lacunarity, float diminish)
{
    return Vec2f(
        _FractalNoise2dFloat(p, octaves, lacunarity, diminish),
        _FractalNoise2dFloat(p + Vec2f(19.0f, 193.0f), octaves, lacunarity,
                             diminish));
}

Vec4f
_FractalNoise2dVec4(Vec2f p, int octaves, float lacunarity, float diminish)
{
    const Vec3f xyz = _FractalNoise2dVec3(p, octaves, lacunarity, diminish);
    const float w = _FractalNoise2dFloat(
        p + Vec2f(19.0f, 193.0f), octaves, lacunarity, diminish);
    return Vec4f(xyz[0], xyz[1], xyz[2], w);
}

Vec2f
_WorleyCellPosition2d(int x, int y, int xoff, int yoff, float jitter)
{
    Vec3f tmp = _CellNoise2dVec3(float(x + xoff), float(y + yoff));
    Vec2f off(tmp[0], tmp[1]);
    off -= Vec2f(0.5f);
    off *= jitter;
    off += Vec2f(0.5f);
    return Vec2f(float(x), float(y)) + off;
}

float
_WorleyDistance2d(const Vec2f& p,
                  int x, int y,
                  int xoff, int yoff,
                  float jitter)
{
    const Vec2f cellPos = _WorleyCellPosition2d(x, y, xoff, yoff, jitter);
    const Vec2f diff = cellPos - p;
    return Dot(diff, diff);
}

float
_WorleyNoise2dFloat(const Vec2f& p, float jitter, int style)
{
    int X = 0;
    int Y = 0;
    const Vec2f localPos(_FloorFrac(p[0], X), _FloorFrac(p[1], Y));
    float minDist = 1.0e6f;
    Vec2f minPos(0.0f);

    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            const float dist = _WorleyDistance2d(localPos, x, y, X, Y, jitter);
            const Vec2f cellPos =
                _WorleyCellPosition2d(x, y, X, Y, jitter) - localPos;
            if (dist < minDist) {
                minDist = dist;
                minPos = cellPos;
            }
        }
    }

    if (style == 1) {
        const Vec2f tmpP = minPos + p;
        return _CellNoise2d(tmpP[0], tmpP[1]);
    }
    return std::sqrt(minDist);
}

Vec2f
_WorleyNoise2dVec2(const Vec2f& p, float jitter, int style)
{
    int X = 0;
    int Y = 0;
    const Vec2f localPos(_FloorFrac(p[0], X), _FloorFrac(p[1], Y));
    Vec2f sqdist(1.0e6f, 1.0e6f);
    Vec2f minPos(0.0f);

    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            const float dist = _WorleyDistance2d(localPos, x, y, X, Y, jitter);
            const Vec2f cellPos =
                _WorleyCellPosition2d(x, y, X, Y, jitter) - localPos;
            if (dist < sqdist[0]) {
                sqdist[1] = sqdist[0];
                sqdist[0] = dist;
                minPos = cellPos;
            } else if (dist < sqdist[1]) {
                sqdist[1] = dist;
            }
        }
    }

    if (style == 1) {
        const Vec2f tmpP = minPos + p;
        const Vec3f tmp = _CellNoise2dVec3(tmpP[0], tmpP[1]);
        return Vec2f(tmp[0], tmp[1]);
    }
    return Vec2f(std::sqrt(sqdist[0]), std::sqrt(sqdist[1]));
}

Vec3f
_WorleyNoise2dVec3(const Vec2f& p, float jitter, int style)
{
    int X = 0;
    int Y = 0;
    const Vec2f localPos(_FloorFrac(p[0], X), _FloorFrac(p[1], Y));
    Vec3f sqdist(1.0e6f, 1.0e6f, 1.0e6f);
    Vec2f minPos(0.0f);

    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            const float dist = _WorleyDistance2d(localPos, x, y, X, Y, jitter);
            const Vec2f cellPos =
                _WorleyCellPosition2d(x, y, X, Y, jitter) - localPos;
            if (dist < sqdist[0]) {
                sqdist[2] = sqdist[1];
                sqdist[1] = sqdist[0];
                sqdist[0] = dist;
                minPos = cellPos;
            } else if (dist < sqdist[1]) {
                sqdist[2] = sqdist[1];
                sqdist[1] = dist;
            } else if (dist < sqdist[2]) {
                sqdist[2] = dist;
            }
        }
    }

    if (style == 1) {
        const Vec2f tmpP = minPos + p;
        return _CellNoise2dVec3(tmpP[0], tmpP[1]);
    }
    return Vec3f(std::sqrt(sqdist[0]),
                 std::sqrt(sqdist[1]),
                 std::sqrt(sqdist[2]));
}

inline uint32_t
_FlakeHash(uint32_t seed, uint32_t i)
{
    return (i ^ seed) * 1075385539u;
}

inline uint32_t
_FlakeInitSeed(int x, int y, int z)
{
    return _FlakeHash(_FlakeHash(_FlakeHash(0u, uint32_t(x)),
                                 uint32_t(y)),
                      uint32_t(z));
}

inline uint32_t
_FlakeXorShift32(uint32_t seed)
{
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

inline float
_UIntTo01(uint32_t x)
{
    return float(x) / float(0xFFFFFFFFu);
}

Vec3f
_RotateFlake(const Vec3f& p, const Vec3f& i)
{
    const float theta = _kTwoPi * i[0];
    const float phi = _kTwoPi * i[1];
    const float z = i[2] * 2.0f;

    const float r = std::sqrt(z);
    const float vx = std::sin(phi) * r;
    const float vy = std::cos(phi) * r;
    const float vz = std::sqrt(2.0f - z);

    const float sTheta = std::sin(theta);
    const float cTheta = std::cos(theta);
    const float sx = vx * sTheta - vy * sTheta;
    const float sy = vx * cTheta + vy * cTheta;

    return Vec3f(
        (vx * sx - sTheta) * p[0] + (vx * sy - sTheta) * p[1] + vx * vz * p[2],
        (vy * sx + cTheta) * p[0] + (vy * sy - cTheta) * p[1] + vy * vz * p[2],
        vz * sx * p[0] + vz * sy * p[1] + (1.0f - z) * p[2]);
}

float
_FlakeDensityToProbability(float x)
{
    const Vec4f abcd(-26.19771808f, 26.39663835f,
                     85.53857017f, -102.35069432f);
    const Vec2f ef(-101.42634862f, 118.45082288f);
    const float xx = x * x;
    return (abcd[0] * xx + abcd[1] * x) /
           (abcd[2] * xx * x + abcd[3] * xx + ef[0] * x + ef[1]);
}

void
_EvalFlake(const Vec3f& position,
           float size,
           float roughness,
           float coverage,
           const Vec3f& normal,
           const Vec3f& tangent,
           const Vec3f& bitangent,
           int* id,
           float* rand,
           float* presence,
           Vec3f* flakeNormal)
{
    const float probability =
        _FlakeDensityToProbability(_Clamp01(coverage));
    const float flakeDiameter = 1.5f / std::sqrt(3.0f);

    const Vec3f P = position / Vec3f(size);
    const Vec3f baseP(std::floor(P[0]), std::floor(P[1]), std::floor(P[2]));
    const int baseX = static_cast<int>(baseP[0]);
    const int baseY = static_cast<int>(baseP[1]);
    const int baseZ = static_cast<int>(baseP[2]);

    float flakePriority = 0.0f;
    uint32_t flakeSeed = 0u;

    for (int i = -1; i < 2; ++i) {
        for (int j = -1; j < 2; ++j) {
            for (int k = -1; k < 2; ++k) {
                uint32_t seed =
                    _FlakeInitSeed(baseX + i, baseY + j, baseZ + k);

                seed = _FlakeXorShift32(seed);
                if (_UIntTo01(seed) > probability) {
                    continue;
                }

                seed = _FlakeXorShift32(seed);
                const float priority = _UIntTo01(seed);
                if (priority < flakePriority) {
                    continue;
                }

                const Vec3f flakeP =
                    baseP + Vec3f(float(i), float(j), float(k)) + Vec3f(0.5f);
                Vec3f pp = P - flakeP;
                if (Dot(pp, pp) >= flakeDiameter * flakeDiameter * 4.0f) {
                    continue;
                }

                Vec3f rot(0.0f);
                seed = _FlakeXorShift32(seed); rot[0] = _UIntTo01(seed);
                seed = _FlakeXorShift32(seed); rot[1] = _UIntTo01(seed);
                seed = _FlakeXorShift32(seed); rot[2] = _UIntTo01(seed);
                pp = _RotateFlake(pp, rot);

                if (std::fabs(pp[0]) <= flakeDiameter &&
                    std::fabs(pp[1]) <= flakeDiameter &&
                    std::fabs(pp[2]) <= flakeDiameter) {
                    flakePriority = priority;
                    flakeSeed = seed;
                }
            }
        }
    }

    if (flakePriority <= 0.0f) {
        if (id) *id = 0;
        if (rand) *rand = 0.0f;
        if (presence) *presence = 0.0f;
        if (flakeNormal) *flakeNormal = normal;
        return;
    }

    uint32_t seed = flakeSeed;
    const float xi0 = _UIntTo01(seed);
    seed = _FlakeXorShift32(seed);
    const float xi1 = _UIntTo01(seed);
    seed = _FlakeXorShift32(seed);

    if (id) {
        *id = static_cast<int>(seed);
    }
    if (rand) {
        *rand = _UIntTo01(seed);
    }
    if (presence) {
        *presence = flakePriority;
    }

    const float phi = _kTwoPi * xi0;
    const float tanTheta = roughness * roughness *
                           std::sqrt(xi1) / std::sqrt(1.0f - xi1);
    const float sinTheta =
        tanTheta / std::sqrt(1.0f + tanTheta * tanTheta);
    const float cosTheta = std::sqrt(1.0f - sinTheta * sinTheta);

    Vec3f n = tangent * std::cos(phi) * sinTheta +
              bitangent * std::sin(phi) * sinTheta +
              normal * cosTheta;
    n.normalize();
    if (flakeNormal) {
        *flakeNormal = n;
    }
}

template<typename T>
T
_RampLr(const ParamMap& inputs, const ShadingContext& ctx)
{
    const T left = Get<T>(inputs, _kValuel, Zero<T>());
    const T right = Get<T>(inputs, _kValuer, Zero<T>());
    const Vec2f tc = Get<Vec2f>(inputs, _kTexcoord, ctx.texcoord);
    return _Mix(left, right, _Clamp01(tc[0]));
}

template<typename T>
T
_RampTb(const ParamMap& inputs, const ShadingContext& ctx)
{
    const T top = Get<T>(inputs, _kValuet, Zero<T>());
    const T bottom = Get<T>(inputs, _kValueb, Zero<T>());
    const Vec2f tc = Get<Vec2f>(inputs, _kTexcoord, ctx.texcoord);
    return _Mix(top, bottom, _Clamp01(tc[1]));
}

template<typename T>
T
_Ramp4(const ParamMap& inputs, const ShadingContext& ctx)
{
    const T topLeft = Get<T>(inputs, _kValuetl, Zero<T>());
    const T topRight = Get<T>(inputs, _kValuetr, Zero<T>());
    const T bottomLeft = Get<T>(inputs, _kValuebl, Zero<T>());
    const T bottomRight = Get<T>(inputs, _kValuebr, Zero<T>());
    const Vec2f uv = _Clamp01(Get<Vec2f>(inputs, _kTexcoord, ctx.texcoord));
    const T top = _Mix(topLeft, topRight, uv[0]);
    const T bottom = _Mix(bottomLeft, bottomRight, uv[0]);
    return _Mix(top, bottom, uv[1]);
}

Vec4f
_EvalRampGradientColor4(const ParamMap& inputs)
{
    const float x = Get<float>(inputs, _kX, 0.0f);
    const float interval1 = Get<float>(inputs, _kInterval1, 0.0f);
    const float interval2 = Get<float>(inputs, _kInterval2, 1.0f);
    const Vec4f color1 = Get<Vec4f>(inputs, _kColor1, Vec4f(0.0f));
    const Vec4f color2 = Get<Vec4f>(inputs, _kColor2, Vec4f(1.0f));
    const Vec4f prevColor = Get<Vec4f>(inputs, _kPrevColor, color1);
    const int interpolation = Get<int>(inputs, _kInterpolation, 1);
    const int intervalNum = Get<int>(inputs, _kIntervalNum, 1);
    const int numIntervals = Get<int>(inputs, _kNumIntervals, 2);

    const float linear = _Remap(
        _ClampFloat(x, interval1, interval2), interval1, interval2, 0.0f, 1.0f);
    const float smooth = _Smoothstep(interval1, interval2, x);
    const float t = interpolation == 1 ? smooth : linear;
    const Vec4f interpColor = interpolation == 2
        ? (interval2 > x ? color1 : color2)
        : _Mix(color1, color2, t);
    const Vec4f intervalColor = x > interval1 ? interpColor : prevColor;
    return intervalNum >= numIntervals ? prevColor : intervalColor;
}

float
_RampCoordinate(const ParamMap& inputs, const ShadingContext& ctx)
{
    const int type = Get<int>(inputs, _kType, 0);
    const Vec2f texcoord = Get<Vec2f>(inputs, _kTexcoord, ctx.texcoord);
    if (type == 0) {
        return texcoord[0];
    }

    const Vec2f delta = texcoord - Vec2f(0.5f);
    if (type == 1) {
        return std::atan2(delta[0], delta[1]) / 6.28319f + 0.5f;
    }
    if (type == 2) {
        return (delta * 1.414f).length();
    }

    const Vec2f deltaAbs(std::fabs(delta[0]), std::fabs(delta[1]));
    return (deltaAbs[0] > deltaAbs[1])
        ? deltaAbs[0] * 2.0f
        : deltaAbs[1] * 2.0f;
}

template<typename T>
T
_ReadAmplitude(const ParamMap& inputs)
{
    return Get<T>(inputs, _kAmplitude, One<T>());
}

template<>
Vec2f
_ReadAmplitude<Vec2f>(const ParamMap& inputs)
{
    if (const Value* value = inputs.Find(_kAmplitude)) {
        if (ValueHolds<Vec2f>(*value)) {
            return ValueGet<Vec2f>(*value);
        }
        if (ValueHolds<float>(*value)) {
            return Vec2f(ValueGet<float>(*value));
        }
    }
    return Vec2f(1.0f);
}

template<>
Vec3f
_ReadAmplitude<Vec3f>(const ParamMap& inputs)
{
    if (const Value* value = inputs.Find(_kAmplitude)) {
        if (ValueHolds<Vec3f>(*value)) {
            return ValueGet<Vec3f>(*value);
        }
        if (ValueHolds<float>(*value)) {
            return Vec3f(ValueGet<float>(*value));
        }
    }
    return Vec3f(1.0f);
}

template<>
Vec4f
_ReadAmplitude<Vec4f>(const ParamMap& inputs)
{
    if (const Value* value = inputs.Find(_kAmplitude)) {
        if (ValueHolds<Vec4f>(*value)) {
            return ValueGet<Vec4f>(*value);
        }
        if (ValueHolds<float>(*value)) {
            return Vec4f(ValueGet<float>(*value));
        }
    }
    return Vec4f(1.0f);
}

template<typename T>
void
_StoreTypedOutput(NodeOutputMap* outputs, const SlotName& slot, const T& value)
{
    (*outputs)[slot] = Value(value);
}

template<typename T>
T
_Noise2dValue(const ParamMap& inputs, const ShadingContext& ctx);

template<>
float
_Noise2dValue<float>(const ParamMap& inputs, const ShadingContext& ctx)
{
    const Vec2f tc = Get<Vec2f>(inputs, _kTexcoord, ctx.texcoord);
    const float amplitude = Get<float>(inputs, _kAmplitude, 1.0f);
    const float pivot = Get<float>(inputs, _kPivot, 0.0f);
    return _PerlinNoise2d(tc[0], tc[1]) * amplitude + pivot;
}

template<>
Vec2f
_Noise2dValue<Vec2f>(const ParamMap& inputs, const ShadingContext& ctx)
{
    const Vec2f tc = Get<Vec2f>(inputs, _kTexcoord, ctx.texcoord);
    const Vec2f amplitude = _ReadAmplitude<Vec2f>(inputs);
    const float pivot = Get<float>(inputs, _kPivot, 0.0f);
    const Vec3f value = _PerlinNoise2dVec3(tc[0], tc[1]);
    return CompMult(Vec2f(value[0], value[1]), amplitude) + Vec2f(pivot);
}

template<>
Vec3f
_Noise2dValue<Vec3f>(const ParamMap& inputs, const ShadingContext& ctx)
{
    const Vec2f tc = Get<Vec2f>(inputs, _kTexcoord, ctx.texcoord);
    const Vec3f amplitude = _ReadAmplitude<Vec3f>(inputs);
    const float pivot = Get<float>(inputs, _kPivot, 0.0f);
    return CompMult(_PerlinNoise2dVec3(tc[0], tc[1]), amplitude) + Vec3f(pivot);
}

template<>
Vec4f
_Noise2dValue<Vec4f>(const ParamMap& inputs, const ShadingContext& ctx)
{
    const Vec2f tc = Get<Vec2f>(inputs, _kTexcoord, ctx.texcoord);
    const Vec4f amplitude = _ReadAmplitude<Vec4f>(inputs);
    const float pivot = Get<float>(inputs, _kPivot, 0.0f);
    const Vec3f xyz = _PerlinNoise2dVec3(tc[0], tc[1]);
    const float w = _PerlinNoise2d(tc[0] + 19.0f, tc[1] + 73.0f);
    return CompMult(Vec4f(xyz[0], xyz[1], xyz[2], w), amplitude) +
           Vec4f(pivot);
}

template<typename T>
T
_Fractal2dValue(const ParamMap& inputs, const ShadingContext& ctx);

template<>
float
_Fractal2dValue<float>(const ParamMap& inputs, const ShadingContext& ctx)
{
    const Vec2f tc = Get<Vec2f>(inputs, _kTexcoord, ctx.texcoord);
    const float amplitude = Get<float>(inputs, _kAmplitude, 1.0f);
    const int octaves = Get<int>(inputs, _kOctaves, 3);
    const float lacunarity = Get<float>(inputs, _kLacunarity, 2.0f);
    const float diminish = Get<float>(inputs, _kDiminish, 0.5f);
    return _FractalNoise2dFloat(tc, octaves, lacunarity, diminish) * amplitude;
}

template<>
Vec2f
_Fractal2dValue<Vec2f>(const ParamMap& inputs, const ShadingContext& ctx)
{
    const Vec2f tc = Get<Vec2f>(inputs, _kTexcoord, ctx.texcoord);
    const Vec2f amplitude = _ReadAmplitude<Vec2f>(inputs);
    const int octaves = Get<int>(inputs, _kOctaves, 3);
    const float lacunarity = Get<float>(inputs, _kLacunarity, 2.0f);
    const float diminish = Get<float>(inputs, _kDiminish, 0.5f);
    return CompMult(_FractalNoise2dVec2(tc, octaves, lacunarity, diminish),
                    amplitude);
}

template<>
Vec3f
_Fractal2dValue<Vec3f>(const ParamMap& inputs, const ShadingContext& ctx)
{
    const Vec2f tc = Get<Vec2f>(inputs, _kTexcoord, ctx.texcoord);
    const Vec3f amplitude = _ReadAmplitude<Vec3f>(inputs);
    const int octaves = Get<int>(inputs, _kOctaves, 3);
    const float lacunarity = Get<float>(inputs, _kLacunarity, 2.0f);
    const float diminish = Get<float>(inputs, _kDiminish, 0.5f);
    return CompMult(_FractalNoise2dVec3(tc, octaves, lacunarity, diminish),
                    amplitude);
}

template<>
Vec4f
_Fractal2dValue<Vec4f>(const ParamMap& inputs, const ShadingContext& ctx)
{
    const Vec2f tc = Get<Vec2f>(inputs, _kTexcoord, ctx.texcoord);
    const Vec4f amplitude = _ReadAmplitude<Vec4f>(inputs);
    const int octaves = Get<int>(inputs, _kOctaves, 3);
    const float lacunarity = Get<float>(inputs, _kLacunarity, 2.0f);
    const float diminish = Get<float>(inputs, _kDiminish, 0.5f);
    return CompMult(_FractalNoise2dVec4(tc, octaves, lacunarity, diminish),
                    amplitude);
}

template<typename T>
void
_EvalNoise2dTyped(const ParamMap& inputs,
                  const ShadingContext& ctx,
                  NodeOutputMap* outputs)
{
    _StoreTypedOutput(outputs, _kOut, _Noise2dValue<T>(inputs, ctx));
}

template<typename T>
void
_EvalFractal2dTyped(const ParamMap& inputs,
                    const ShadingContext& ctx,
                    NodeOutputMap* outputs)
{
    _StoreTypedOutput(outputs, _kOut, _Fractal2dValue<T>(inputs, ctx));
}

template<typename T>
void
_EvalRampLrTyped(const ParamMap& inputs,
                 const ShadingContext& ctx,
                 NodeOutputMap* outputs)
{
    _StoreTypedOutput(outputs, _kOut, _RampLr<T>(inputs, ctx));
}

template<typename T>
void
_EvalRampTbTyped(const ParamMap& inputs,
                 const ShadingContext& ctx,
                 NodeOutputMap* outputs)
{
    _StoreTypedOutput(outputs, _kOut, _RampTb<T>(inputs, ctx));
}

template<typename T>
void
_EvalRamp4Typed(const ParamMap& inputs,
                const ShadingContext& ctx,
                NodeOutputMap* outputs)
{
    _StoreTypedOutput(outputs, _kOut, _Ramp4<T>(inputs, ctx));
}

template<typename T>
void
_EvalSplitLrTyped(const ParamMap& inputs,
                  const ShadingContext& ctx,
                  NodeOutputMap* outputs)
{
    const T left = Get<T>(inputs, _kValuel, Zero<T>());
    const T right = Get<T>(inputs, _kValuer, Zero<T>());
    const float center = Get<float>(inputs, _kCenter, 0.5f);
    const Vec2f tc = Get<Vec2f>(inputs, _kTexcoord, ctx.texcoord);
    const float t = _AAStep(center, tc[0], ctx.dudx, ctx.dudy);
    _StoreTypedOutput(outputs, _kOut, _Mix(left, right, t));
}

template<typename T>
void
_EvalSplitTbTyped(const ParamMap& inputs,
                  const ShadingContext& ctx,
                  NodeOutputMap* outputs)
{
    const T top = Get<T>(inputs, _kValuet, Zero<T>());
    const T bottom = Get<T>(inputs, _kValueb, Zero<T>());
    const float center = Get<float>(inputs, _kCenter, 0.5f);
    const Vec2f tc = Get<Vec2f>(inputs, _kTexcoord, ctx.texcoord);
    const float t = _AAStep(center, tc[1], ctx.dvdx, ctx.dvdy);
    _StoreTypedOutput(outputs, _kOut, _Mix(top, bottom, t));
}

static void
_EvalNoise3dFloat(const ParamMap& inputs,
                  const ShadingContext& ctx,
                  NodeOutputMap* outputs)
{
    const Vec3f pos = Get<Vec3f>(inputs, _kPosition, ctx.position);
    const float amplitude = Get<float>(inputs, _kAmplitude, 1.0f);
    const float pivot = Get<float>(inputs, _kPivot, 0.0f);
    _StoreTypedOutput(outputs, _kOut,
                      _PerlinNoise3d(pos[0], pos[1], pos[2]) * amplitude + pivot);
}

static void
_EvalNoise3dColor3(const ParamMap& inputs,
                   const ShadingContext& ctx,
                   NodeOutputMap* outputs)
{
    const Vec3f pos = Get<Vec3f>(inputs, _kPosition, ctx.position);
    const Vec3f amplitude = _ReadAmplitude<Vec3f>(inputs);
    const float pivot = Get<float>(inputs, _kPivot, 0.0f);
    const Vec3f value = _PerlinNoise3dVec3(pos[0], pos[1], pos[2]);
    _StoreTypedOutput(outputs, _kOut,
                      CompMult(value, amplitude) + Vec3f(pivot));
}

static void
_EvalFractal3dFloat(const ParamMap& inputs,
                    const ShadingContext& ctx,
                    NodeOutputMap* outputs)
{
    Vec3f pos = Get<Vec3f>(inputs, _kPosition, ctx.position);
    const float amplitude = Get<float>(inputs, _kAmplitude, 1.0f);
    const int octaves = Get<int>(inputs, _kOctaves, 3);
    const float lacunarity = Get<float>(inputs, _kLacunarity, 2.0f);
    const float diminish = Get<float>(inputs, _kDiminish, 0.5f);

    float result = 0.0f;
    float weight = 1.0f;
    for (int i = 0; i < octaves; ++i) {
        result += weight * _PerlinNoise3d(pos[0], pos[1], pos[2]);
        pos *= lacunarity;
        weight *= diminish;
    }
    _StoreTypedOutput(outputs, _kOut, result * amplitude);
}

static void
_EvalCellnoise3d(const ParamMap& inputs,
                 const ShadingContext& ctx,
                 NodeOutputMap* outputs)
{
    const Vec3f pos = Get<Vec3f>(inputs, _kPosition, ctx.position);
    _StoreTypedOutput(outputs, _kOut,
                      _CellNoise3d(pos[0], pos[1], pos[2]));
}

static void
_EvalCellnoise2d(const ParamMap& inputs,
                 const ShadingContext& ctx,
                 NodeOutputMap* outputs)
{
    const Vec2f tc = Get<Vec2f>(inputs, _kTexcoord, ctx.texcoord);
    _StoreTypedOutput(outputs, _kOut, _CellNoise2d(tc[0], tc[1]));
}

static void
_EvalWorleyNoise2dFloat(const ParamMap& inputs,
                        const ShadingContext& ctx,
                        NodeOutputMap* outputs)
{
    const Vec2f tc = Get<Vec2f>(inputs, _kTexcoord, ctx.texcoord);
    const float jitter = Get<float>(inputs, _kJitter, 1.0f);
    const int style = Get<int>(inputs, _kStyle, 0);
    _StoreTypedOutput(outputs, _kOut,
                      _WorleyNoise2dFloat(tc, jitter, style));
}

static void
_EvalWorleyNoise2dVec2(const ParamMap& inputs,
                       const ShadingContext& ctx,
                       NodeOutputMap* outputs)
{
    const Vec2f tc = Get<Vec2f>(inputs, _kTexcoord, ctx.texcoord);
    const float jitter = Get<float>(inputs, _kJitter, 1.0f);
    const int style = Get<int>(inputs, _kStyle, 0);
    _StoreTypedOutput(outputs, _kOut,
                      _WorleyNoise2dVec2(tc, jitter, style));
}

static void
_EvalWorleyNoise2dVec3(const ParamMap& inputs,
                       const ShadingContext& ctx,
                       NodeOutputMap* outputs)
{
    const Vec2f tc = Get<Vec2f>(inputs, _kTexcoord, ctx.texcoord);
    const float jitter = Get<float>(inputs, _kJitter, 1.0f);
    const int style = Get<int>(inputs, _kStyle, 0);
    _StoreTypedOutput(outputs, _kOut,
                      _WorleyNoise2dVec3(tc, jitter, style));
}

static void
_EvalUnifiedNoise2dFloat(const ParamMap& inputs,
                         const ShadingContext& ctx,
                         NodeOutputMap* outputs)
{
    const Vec2f texcoord = Get<Vec2f>(inputs, _kTexcoord, ctx.texcoord);
    const Vec2f freq = Get<Vec2f>(inputs, _kFreq, Vec2f(1.0f));
    const Vec2f offset = Get<Vec2f>(inputs, _kOffset, Vec2f(0.0f));
    const float jitter = Get<float>(inputs, _kJitter, 1.0f);
    const float outMin = Get<float>(inputs, _kOutmin, 0.0f);
    const float outMax = Get<float>(inputs, _kOutmax, 1.0f);
    const bool clampOutput = Get<bool>(inputs, _kClampoutput, true);
    const int octaves = Get<int>(inputs, _kOctaves, 3);
    const float lacunarity = Get<float>(inputs, _kLacunarity, 2.0f);
    const float diminish = Get<float>(inputs, _kDiminish, 0.5f);
    const int type = Get<int>(inputs, _kType, 0);
    const int style = Get<int>(inputs, _kStyle, 0);

    const Vec2f applyFreq = CompMult(texcoord, freq);
    const Vec2f applyOffset = applyFreq + offset;
    const float cellJitterMult = (jitter - 1.0f) * 90000.0f;
    const Vec2f applyCellJitter = _Rotate2d(applyOffset, cellJitterMult);
    const Vec3f fractalPos(applyOffset[0], applyOffset[1], cellJitterMult);

    float value = 0.0f;
    switch (type) {
    case 1:
        value = _CellNoise2d(applyCellJitter[0], applyCellJitter[1]);
        break;
    case 2:
        value = _WorleyNoise2dFloat(applyOffset, jitter, style);
        break;
    case 3:
        value = 0.0f;
        {
            Vec3f p = fractalPos;
            float weight = 1.0f;
            for (int i = 0; i < octaves; ++i) {
                value += weight * _PerlinNoise3d(p[0], p[1], p[2]);
                p *= lacunarity;
                weight *= diminish;
            }
        }
        break;
    case 0:
    default:
        value = _PerlinNoise2d(applyCellJitter[0], applyCellJitter[1]) * 0.5f +
                0.5f;
        break;
    }

    value = _Remap(value, 0.0f, 1.0f, outMin, outMax);
    if (clampOutput) {
        const float lo = std::min(outMin, outMax);
        const float hi = std::max(outMin, outMax);
        value = _ClampFloat(value, lo, hi);
    }
    _StoreTypedOutput(outputs, _kOut, value);
}

static void
_EvalRampGradient(const ParamMap& inputs,
                  const ShadingContext&,
                  NodeOutputMap* outputs)
{
    _StoreTypedOutput(outputs, _kOut, _EvalRampGradientColor4(inputs));
}

static void
_EvalRamp(const ParamMap& inputs,
          const ShadingContext& ctx,
          NodeOutputMap* outputs)
{
    const float x = _RampCoordinate(inputs, ctx);

    std::array<float, 10> intervals = {
        Get<float>(inputs, _kInterval1, 0.0f),
        Get<float>(inputs, _kInterval2, 1.0f),
        Get<float>(inputs, _kInterval3, 1.0f),
        Get<float>(inputs, _kInterval4, 1.0f),
        Get<float>(inputs, _kInterval5, 1.0f),
        Get<float>(inputs, _kInterval6, 1.0f),
        Get<float>(inputs, _kInterval7, 1.0f),
        Get<float>(inputs, _kInterval8, 1.0f),
        Get<float>(inputs, _kInterval9, 1.0f),
        Get<float>(inputs, _kInterval10, 1.0f)
    };

    std::array<Vec4f, 10> colors = {
        Get<Vec4f>(inputs, _kColor1, Vec4f(0.0f, 0.0f, 0.0f, 1.0f)),
        Get<Vec4f>(inputs, _kColor2, Vec4f(1.0f, 1.0f, 1.0f, 1.0f)),
        Get<Vec4f>(inputs, _kColor3, Vec4f(1.0f)),
        Get<Vec4f>(inputs, _kColor4, Vec4f(1.0f)),
        Get<Vec4f>(inputs, _kColor5, Vec4f(1.0f)),
        Get<Vec4f>(inputs, _kColor6, Vec4f(1.0f)),
        Get<Vec4f>(inputs, _kColor7, Vec4f(1.0f)),
        Get<Vec4f>(inputs, _kColor8, Vec4f(1.0f)),
        Get<Vec4f>(inputs, _kColor9, Vec4f(1.0f)),
        Get<Vec4f>(inputs, _kColor10, Vec4f(1.0f))
    };

    const int interpolation = Get<int>(inputs, _kInterpolation, 1);
    const int numIntervals = Get<int>(inputs, _kNumIntervals, 2);

    Vec4f result = colors[0];
    for (int i = 0; i < 9; ++i) {
        ParamMap gradientInputs;
        gradientInputs[_kX] = Value(x);
        gradientInputs[_kInterval1] = Value(intervals[i]);
        gradientInputs[_kInterval2] = Value(intervals[i + 1]);
        gradientInputs[_kColor1] = Value(colors[i]);
        gradientInputs[_kColor2] = Value(colors[i + 1]);
        gradientInputs[_kPrevColor] = Value(result);
        gradientInputs[_kInterpolation] = Value(interpolation);
        gradientInputs[_kIntervalNum] = Value(i + 1);
        gradientInputs[_kNumIntervals] = Value(numIntervals);
        result = _EvalRampGradientColor4(gradientInputs);
    }

    _StoreTypedOutput(outputs, _kOut, result);
}

static void
_EvalCheckerboardColor3(const ParamMap& inputs,
                        const ShadingContext& ctx,
                        NodeOutputMap* outputs)
{
    const Vec3f color1 = Get<Vec3f>(inputs, _kColor1, Vec3f(1.0f));
    const Vec3f color2 = Get<Vec3f>(inputs, _kColor2, Vec3f(0.0f));
    const Vec2f uvtiling = Get<Vec2f>(inputs, _kUvtiling, Vec2f(8.0f));
    const Vec2f uvoffset = Get<Vec2f>(inputs, _kUvoffset, Vec2f(0.0f));
    const Vec2f texcoord = Get<Vec2f>(inputs, _kTexcoord, ctx.texcoord);
    const Vec2f tiled = CompMult(texcoord, uvtiling) - uvoffset;
    const Vec2f floored(std::floor(tiled[0]), std::floor(tiled[1]));
    const float selector = _PositiveMod(floored[0] + floored[1], 2.0f);
    _StoreTypedOutput(outputs, _kOut, _Mix(color2, color1, selector));
}

inline float
_LineMask(const Vec2f& texcoord, const Vec2f& center, float radius,
          const Vec2f& point1, const Vec2f& point2)
{
    const Vec2f delta = texcoord - center;
    const Vec2f pa = delta - point1;
    const Vec2f ba = point2 - point1;
    const float dotBaBa = Dot(ba, ba);
    const float h = dotBaBa > 0.0f
        ? _Clamp01(Dot(pa, ba) / dotBaBa)
        : 0.0f;
    const float dist = (pa - ba * h).length();
    return dist > radius ? 0.0f : 1.0f;
}

static void
_EvalLineFloat(const ParamMap& inputs,
               const ShadingContext& ctx,
               NodeOutputMap* outputs)
{
    const Vec2f texcoord = Get<Vec2f>(inputs, _kTexcoord, ctx.texcoord);
    const Vec2f center = Get<Vec2f>(inputs, _kCenter, Vec2f(0.0f));
    const float radius = Get<float>(inputs, _kRadius, 0.1f);
    const Vec2f point1 = Get<Vec2f>(inputs, _kPoint1, Vec2f(0.25f, 0.25f));
    const Vec2f point2 = Get<Vec2f>(inputs, _kPoint2, Vec2f(0.75f, 0.75f));
    _StoreTypedOutput(outputs, _kOut,
                      _LineMask(texcoord, center, radius, point1, point2));
}

static void
_EvalCircleFloat(const ParamMap& inputs,
                 const ShadingContext& ctx,
                 NodeOutputMap* outputs)
{
    const Vec2f texcoord = Get<Vec2f>(inputs, _kTexcoord, ctx.texcoord);
    const Vec2f center = Get<Vec2f>(inputs, _kCenter, Vec2f(0.0f));
    const float radius = Get<float>(inputs, _kRadius, 0.5f);
    const Vec2f delta = texcoord - center;
    _StoreTypedOutput(outputs, _kOut,
                      Dot(delta, delta) > radius * radius ? 0.0f : 1.0f);
}

static void
_EvalCloverleafFloat(const ParamMap& inputs,
                     const ShadingContext& ctx,
                     NodeOutputMap* outputs)
{
    const Vec2f texcoord = Get<Vec2f>(inputs, _kTexcoord, ctx.texcoord);
    const Vec2f center = Get<Vec2f>(inputs, _kCenter, Vec2f(0.0f));
    const float radius = Get<float>(inputs, _kRadius, 0.5f);

    const Vec2f sampleDouble = texcoord + texcoord;
    const Vec2f sampleAdd = sampleDouble + Vec2f(radius);
    const Vec2f sampleSubtract = sampleDouble - Vec2f(radius);

    const std::array<Vec2f, 4> coords = {
        Vec2f(sampleAdd[0], sampleDouble[1]),
        Vec2f(sampleSubtract[0], sampleDouble[1]),
        Vec2f(sampleDouble[0], sampleSubtract[1]),
        Vec2f(sampleDouble[0], sampleAdd[1])
    };

    float result = 0.0f;
    for (const Vec2f& coord : coords) {
        const Vec2f delta = coord - center;
        result = std::max(result,
                          Dot(delta, delta) > radius * radius ? 0.0f : 1.0f);
    }
    _StoreTypedOutput(outputs, _kOut, result);
}

static void
_EvalHexagonFloat(const ParamMap& inputs,
                  const ShadingContext& ctx,
                  NodeOutputMap* outputs)
{
    const Vec2f texcoord = Get<Vec2f>(inputs, _kTexcoord, ctx.texcoord);
    const Vec2f center = Get<Vec2f>(inputs, _kCenter, Vec2f(0.0f));
    const float radius = Get<float>(inputs, _kRadius, 0.5f);

    Vec2f p = texcoord - center;
    p = Vec2f(std::fabs(p[0]), std::fabs(p[1]));

    const Vec3f k(-0.866025f, 0.5f, 0.57735f);
    const float dotKp = k[0] * p[0] + k[1] * p[1];
    const float m = std::min(dotKp, 0.0f);
    p -= Vec2f(2.0f * m * k[0], 2.0f * m * k[1]);
    p -= Vec2f(_ClampFloat(p[0], -k[2] * radius, k[2] * radius), radius);

    const float signedDistance = p.length() * (p[1] < 0.0f ? -1.0f : 1.0f);
    _StoreTypedOutput(outputs, _kOut, signedDistance > 0.0f ? 0.0f : 1.0f);
}

static void
_EvalGridColor3(const ParamMap& inputs,
                const ShadingContext& ctx,
                NodeOutputMap* outputs)
{
    const Vec2f texcoord = Get<Vec2f>(inputs, _kTexcoord, ctx.texcoord);
    const Vec2f uvtiling = Get<Vec2f>(inputs, _kUvtiling, Vec2f(1.0f));
    const Vec2f uvoffset = Get<Vec2f>(inputs, _kUvoffset, Vec2f(0.0f));
    const float thickness = Get<float>(inputs, _kThickness, 0.05f);
    const bool staggered = Get<bool>(inputs, _kStaggered, false);

    const Vec2f texcoordBias = CompMult(texcoord, uvtiling) - uvoffset;
    const float thickToSize = 1.0f - thickness;
    const float modY = _PositiveMod(texcoordBias[1]);
    const float modYRow = _PositiveMod(texcoordBias[1], 2.0f);
    const float altRowsShift = modYRow > 1.0f ? 0.5f : 0.0f;
    const float x = staggered ? texcoordBias[0] + altRowsShift : texcoordBias[0];
    const float modX = _PositiveMod(x);
    const float absX = std::fabs(modX * 2.0f - 1.0f);
    const float absY = std::fabs(modY * 2.0f - 1.0f);
    const float xDetect = absX > thickToSize ? 0.0f : 1.0f;
    const float yDetect = absY > thickToSize ? 0.0f : 1.0f;
    const float value = 1.0f - std::min(xDetect, yDetect);
    _StoreTypedOutput(outputs, _kOut, Vec3f(value));
}

static void
_EvalCrosshatchColor3(const ParamMap& inputs,
                      const ShadingContext& ctx,
                      NodeOutputMap* outputs)
{
    const Vec2f texcoord = Get<Vec2f>(inputs, _kTexcoord, ctx.texcoord);
    const Vec2f uvtiling = Get<Vec2f>(inputs, _kUvtiling, Vec2f(1.0f));
    const Vec2f uvoffset = Get<Vec2f>(inputs, _kUvoffset, Vec2f(0.0f));
    const float thickness = Get<float>(inputs, _kThickness, 0.05f);
    const bool staggered = Get<bool>(inputs, _kStaggered, false);

    const Vec2f texcoordBias = CompMult(texcoord, uvtiling) - uvoffset;
    const float modY = _PositiveMod(texcoordBias[1]);
    const float modYRow = _PositiveMod(texcoordBias[1], 2.0f);
    const float altRowsShift = modYRow > 1.0f ? 0.5f : 0.0f;
    const float x = staggered ? texcoordBias[0] + altRowsShift : texcoordBias[0];
    const float modX = _PositiveMod(x);
    const Vec2f sampleVec(modX * 2.0f - 1.0f, modY * 2.0f - 1.0f);

    const float diag1 = _LineMask(sampleVec, Vec2f(0.0f), thickness,
                                  Vec2f(1.0f, 1.0f), Vec2f(-1.0f, -1.0f));
    const float diag2 = _LineMask(sampleVec, Vec2f(0.0f), thickness,
                                  Vec2f(-1.0f, 1.0f), Vec2f(1.0f, -1.0f));
    const float value = std::max(diag1, diag2);
    _StoreTypedOutput(outputs, _kOut, Vec3f(value));
}

template<typename T>
void
_EvalRandomFloatTyped(const ParamMap& inputs,
                      const ShadingContext&,
                      NodeOutputMap* outputs)
{
    float inputValue = 0.0f;
    if constexpr (std::is_same<T, int>::value) {
        inputValue = static_cast<float>(Get<int>(inputs, _kIn, 0));
    } else {
        inputValue = Get<float>(inputs, _kIn, 0.0f) * 4096.0f;
    }

    const float minValue = Get<float>(inputs, _kMin, 0.0f);
    const float maxValue = Get<float>(inputs, _kMax, 1.0f);
    const float seed = static_cast<float>(Get<int>(inputs, _kSeed, 0));
    const float noise = _CellNoise2d(inputValue, seed);
    _StoreTypedOutput(outputs, _kOut,
                      _ClampFloat(_Remap(noise, 0.0f, 1.0f, minValue, maxValue),
                                  std::min(minValue, maxValue),
                                  std::max(minValue, maxValue)));
}

static Vec3f
_EvalRandomColor(float inputValue,
                 float hueLow, float hueHigh,
                 float satLow, float satHigh,
                 float brightLow, float brightHigh,
                 int seed)
{
    const int seedHue = static_cast<int>(std::ceil(float(seed) + 413.3f));
    const int seedSaturation =
        static_cast<int>(std::ceil(float(seed) + 1522.4f));
    const int seedBrightness =
        static_cast<int>(std::ceil(float(seed) + 1813.8f));

    auto randRange = [inputValue](float lo, float hi, int seedValue) {
        ParamMap randomInputs;
        randomInputs[_kIn] = Value(inputValue);
        randomInputs[_kMin] = Value(lo);
        randomInputs[_kMax] = Value(hi);
        randomInputs[_kSeed] = Value(seedValue);
        NodeOutputMap randomOutputs;
        _EvalRandomFloatTyped<float>(randomInputs, ShadingContext(), &randomOutputs);
        const Value* value = randomOutputs.Find(_kOut);
        return value && ValueHolds<float>(*value) ? ValueGet<float>(*value) : lo;
    };

    const float hue = randRange(hueLow, hueHigh, seedHue);
    const float saturation = randRange(satLow, satHigh, seedSaturation);
    const float brightness = randRange(brightLow, brightHigh, seedBrightness);
    return HsvToRgb(Vec3f(hue, saturation, brightness));
}

static void
_EvalRandomColorFloat(const ParamMap& inputs,
                      const ShadingContext&,
                      NodeOutputMap* outputs)
{
    const float inputValue = Get<float>(inputs, _kIn, 0.0f);
    const int seed = Get<int>(inputs, _kSeed, 0);
    _StoreTypedOutput(outputs, _kOut, _EvalRandomColor(
        inputValue,
        Get<float>(inputs, _kHuelow, 0.0f),
        Get<float>(inputs, _kHuehigh, 1.0f),
        Get<float>(inputs, _kSaturationlow, 0.825f),
        Get<float>(inputs, _kSaturationhigh, 1.0f),
        Get<float>(inputs, _kBrightnesslow, 1.0f),
        Get<float>(inputs, _kBrightnesshigh, 1.0f),
        seed));
}

static void
_EvalRandomColorInteger(const ParamMap& inputs,
                        const ShadingContext&,
                        NodeOutputMap* outputs)
{
    const float inputValue = static_cast<float>(Get<int>(inputs, _kIn, 0));
    const int seed = Get<int>(inputs, _kSeed, 0);
    _StoreTypedOutput(outputs, _kOut, _EvalRandomColor(
        inputValue,
        Get<float>(inputs, _kHuelow, 0.0f),
        Get<float>(inputs, _kHuehigh, 1.0f),
        Get<float>(inputs, _kSaturationlow, 0.825f),
        Get<float>(inputs, _kSaturationhigh, 1.0f),
        Get<float>(inputs, _kBrightnesslow, 1.0f),
        Get<float>(inputs, _kBrightnesshigh, 1.0f),
        seed));
}

static void
_EvalFlake2d(const ParamMap& inputs,
             const ShadingContext& ctx,
             NodeOutputMap* outputs)
{
    const float size = Get<float>(inputs, _kSize, 0.01f);
    const float roughness = Get<float>(inputs, _kRoughness, 0.1f);
    const float coverage = Get<float>(inputs, _kCoverage, 0.5f);
    const Vec2f texcoord = Get<Vec2f>(inputs, _kTexcoord, ctx.texcoord);
    const Vec3f normal = Get<Vec3f>(inputs, _kNormal, ctx.normal);
    const Vec3f tangent = Get<Vec3f>(inputs, _kTangent, ctx.tangent);
    const Vec3f bitangent = Get<Vec3f>(inputs, _kBitangent, ctx.bitangent);

    int id = 0;
    float rand = 0.0f;
    float presence = 0.0f;
    Vec3f flakeNormal = normal;
    _EvalFlake(Vec3f(texcoord[0], texcoord[1], 0.0f),
               size,
               roughness,
               coverage,
               normal,
               tangent,
               bitangent,
               &id,
               &rand,
               &presence,
               &flakeNormal);

    _StoreTypedOutput(outputs, _kId, id);
    _StoreTypedOutput(outputs, _kRand, rand);
    _StoreTypedOutput(outputs, _kPresence, presence);
    _StoreTypedOutput(outputs, _kFlakenormal, flakeNormal);
}

} // namespace

// ---- Registration --------------------------------------------------------

#define _REG(name, fn) reg.Register(name, fn)

void
RegisterProceduralNodes(NodeRegistry& reg)
{
    _REG("ND_noise3d_float", &_EvalNoise3dFloat);
    _REG("ND_noise3d_color3", &_EvalNoise3dColor3);

    _REG("ND_noise2d_float", &_EvalNoise2dTyped<float>);
    _REG("ND_noise2d_color3", &_EvalNoise2dTyped<Vec3f>);
    _REG("ND_noise2d_color4", &_EvalNoise2dTyped<Vec4f>);
    _REG("ND_noise2d_vector2", &_EvalNoise2dTyped<Vec2f>);
    _REG("ND_noise2d_vector3", &_EvalNoise2dTyped<Vec3f>);
    _REG("ND_noise2d_vector4", &_EvalNoise2dTyped<Vec4f>);
    _REG("ND_noise2d_color3FA", &_EvalNoise2dTyped<Vec3f>);
    _REG("ND_noise2d_color4FA", &_EvalNoise2dTyped<Vec4f>);
    _REG("ND_noise2d_vector2FA", &_EvalNoise2dTyped<Vec2f>);
    _REG("ND_noise2d_vector3FA", &_EvalNoise2dTyped<Vec3f>);
    _REG("ND_noise2d_vector4FA", &_EvalNoise2dTyped<Vec4f>);

    _REG("ND_fractal3d_float", &_EvalFractal3dFloat);
    _REG("ND_fractal2d_float", &_EvalFractal2dTyped<float>);
    _REG("ND_fractal2d_color3", &_EvalFractal2dTyped<Vec3f>);
    _REG("ND_fractal2d_color4", &_EvalFractal2dTyped<Vec4f>);
    _REG("ND_fractal2d_vector2", &_EvalFractal2dTyped<Vec2f>);
    _REG("ND_fractal2d_vector3", &_EvalFractal2dTyped<Vec3f>);
    _REG("ND_fractal2d_vector4", &_EvalFractal2dTyped<Vec4f>);
    _REG("ND_fractal2d_color3FA", &_EvalFractal2dTyped<Vec3f>);
    _REG("ND_fractal2d_color4FA", &_EvalFractal2dTyped<Vec4f>);
    _REG("ND_fractal2d_vector2FA", &_EvalFractal2dTyped<Vec2f>);
    _REG("ND_fractal2d_vector3FA", &_EvalFractal2dTyped<Vec3f>);
    _REG("ND_fractal2d_vector4FA", &_EvalFractal2dTyped<Vec4f>);

    _REG("ND_cellnoise3d_float", &_EvalCellnoise3d);
    _REG("ND_cellnoise2d_float", &_EvalCellnoise2d);

    _REG("ND_worleynoise2d_float", &_EvalWorleyNoise2dFloat);
    _REG("ND_worleynoise2d_vector2", &_EvalWorleyNoise2dVec2);
    _REG("ND_worleynoise2d_vector3", &_EvalWorleyNoise2dVec3);
    _REG("ND_unifiednoise2d_float", &_EvalUnifiedNoise2dFloat);

    _REG("ND_ramplr_float", &_EvalRampLrTyped<float>);
    _REG("ND_ramplr_color3", &_EvalRampLrTyped<Vec3f>);
    _REG("ND_ramplr_color4", &_EvalRampLrTyped<Vec4f>);
    _REG("ND_ramplr_vector2", &_EvalRampLrTyped<Vec2f>);
    _REG("ND_ramplr_vector3", &_EvalRampLrTyped<Vec3f>);
    _REG("ND_ramplr_vector4", &_EvalRampLrTyped<Vec4f>);

    _REG("ND_ramptb_float", &_EvalRampTbTyped<float>);
    _REG("ND_ramptb_color3", &_EvalRampTbTyped<Vec3f>);
    _REG("ND_ramptb_color4", &_EvalRampTbTyped<Vec4f>);
    _REG("ND_ramptb_vector2", &_EvalRampTbTyped<Vec2f>);
    _REG("ND_ramptb_vector3", &_EvalRampTbTyped<Vec3f>);
    _REG("ND_ramptb_vector4", &_EvalRampTbTyped<Vec4f>);

    _REG("ND_ramp4_float", &_EvalRamp4Typed<float>);
    _REG("ND_ramp4_color3", &_EvalRamp4Typed<Vec3f>);
    _REG("ND_ramp4_color4", &_EvalRamp4Typed<Vec4f>);
    _REG("ND_ramp4_vector2", &_EvalRamp4Typed<Vec2f>);
    _REG("ND_ramp4_vector3", &_EvalRamp4Typed<Vec3f>);
    _REG("ND_ramp4_vector4", &_EvalRamp4Typed<Vec4f>);

    _REG("ND_splitlr_float", &_EvalSplitLrTyped<float>);
    _REG("ND_splitlr_color3", &_EvalSplitLrTyped<Vec3f>);
    _REG("ND_splitlr_color4", &_EvalSplitLrTyped<Vec4f>);
    _REG("ND_splitlr_vector2", &_EvalSplitLrTyped<Vec2f>);
    _REG("ND_splitlr_vector3", &_EvalSplitLrTyped<Vec3f>);
    _REG("ND_splitlr_vector4", &_EvalSplitLrTyped<Vec4f>);

    _REG("ND_splittb_float", &_EvalSplitTbTyped<float>);
    _REG("ND_splittb_color3", &_EvalSplitTbTyped<Vec3f>);
    _REG("ND_splittb_color4", &_EvalSplitTbTyped<Vec4f>);
    _REG("ND_splittb_vector2", &_EvalSplitTbTyped<Vec2f>);
    _REG("ND_splittb_vector3", &_EvalSplitTbTyped<Vec3f>);
    _REG("ND_splittb_vector4", &_EvalSplitTbTyped<Vec4f>);

    _REG("ND_ramp_gradient", &_EvalRampGradient);
    _REG("ND_ramp", &_EvalRamp);

    _REG("ND_checkerboard_color3", &_EvalCheckerboardColor3);
    _REG("ND_line_float", &_EvalLineFloat);
    _REG("ND_circle_float", &_EvalCircleFloat);
    _REG("ND_cloverleaf_float", &_EvalCloverleafFloat);
    _REG("ND_hexagon_float", &_EvalHexagonFloat);
    _REG("ND_grid_color3", &_EvalGridColor3);
    _REG("ND_crosshatch_color3", &_EvalCrosshatchColor3);

    _REG("ND_randomfloat_float", &_EvalRandomFloatTyped<float>);
    _REG("ND_randomfloat_integer", &_EvalRandomFloatTyped<int>);
    _REG("ND_randomcolor_float", &_EvalRandomColorFloat);
    _REG("ND_randomcolor_integer", &_EvalRandomColorInteger);

    _REG("ND_flake2d", &_EvalFlake2d);
}

#undef _REG

} // namespace mxcpp
