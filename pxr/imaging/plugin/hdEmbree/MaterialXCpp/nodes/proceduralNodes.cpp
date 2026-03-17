//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "proceduralNodes.h"
#include "../nodeRegistry.h"

#include <cmath>
#include <string>

namespace mxcpp {

static const SlotName _kOut("out");
static const SlotName _kTexcoord("texcoord");
static const SlotName _kPosition("position");
static const SlotName _kAmplitude("amplitude");
static const SlotName _kPivot("pivot");
static const SlotName _kOctaves("octaves");
static const SlotName _kLacunarity("lacunarity");
static const SlotName _kDiminish("diminish");

// -----------------------------------------------------------------------
// MaterialX-compatible Perlin gradient noise.
//
// Ported from MaterialX's mx_noise.glsl which is itself a GLSL conversion
// of OSL's oslnoise.h (Sony Pictures Imageworks).
// This ensures identical noise patterns between Storm (GLSL) and Embree.
// -----------------------------------------------------------------------

namespace {

// -- Bob Jenkins hash (matches mx_bjmix / mx_bjfinal in mx_noise.glsl) -----

inline uint32_t _Rotl32(uint32_t x, int k) {
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
    uint32_t len = 1u;
    uint32_t seed = 0xdeadbeefu + (len << 2u) + 13u;
    return _BJFinal(seed + uint32_t(x), seed, seed);
}
inline uint32_t _HashInt(int x, int y) {
    uint32_t len = 2u;
    uint32_t a, b, c;
    a = b = c = 0xdeadbeefu + (len << 2u) + 13u;
    a += uint32_t(x);
    b += uint32_t(y);
    return _BJFinal(a, b, c);
}
inline uint32_t _HashInt(int x, int y, int z) {
    uint32_t len = 3u;
    uint32_t a, b, c;
    a = b = c = 0xdeadbeefu + (len << 2u) + 13u;
    a += uint32_t(x);
    b += uint32_t(y);
    c += uint32_t(z);
    return _BJFinal(a, b, c);
}
inline uint32_t _HashInt(int x, int y, int z, int xx) {
    uint32_t a, b, c;
    a = b = c = 0xdeadbeefu + (4u << 2u) + 13u;
    a += uint32_t(x); b += uint32_t(y); c += uint32_t(z);
    _BJMix(a, b, c);
    a += uint32_t(xx);
    return _BJFinal(a, b, c);
}

struct _UVec3 { uint32_t x, y, z; };

inline _UVec3 _HashVec3(int x, int y, int z) {
    uint32_t h = _HashInt(x, y, z);
    return { h & 0xFFu, (h >> 8) & 0xFFu, (h >> 16) & 0xFFu };
}
inline _UVec3 _HashVec3(int x, int y) {
    uint32_t h = _HashInt(x, y);
    return { h & 0xFFu, (h >> 8) & 0xFFu, (h >> 16) & 0xFFu };
}

inline float _BitsTo01(uint32_t bits) {
    return float(bits) / float(0xFFFFFFFFu);
}

// -- Gradient functions (cube-edge vectors, matching mx_gradient_float) -----

inline float _GradientFloat(uint32_t hash, float x, float y) {
    uint32_t h = hash & 7u;
    float u = (h < 4u) ? x : y;
    float v = 2.0f * ((h < 4u) ? y : x);
    return ((h & 1u) ? -u : u) + ((h & 2u) ? -v : v);
}
inline float _GradientFloat(uint32_t hash, float x, float y, float z) {
    uint32_t h = hash & 15u;
    float u = (h < 8u) ? x : y;
    float v = (h < 4u) ? y : ((h == 12u || h == 14u) ? x : z);
    return ((h & 1u) ? -u : u) + ((h & 2u) ? -v : v);
}

inline Vec3f _GradientVec3(_UVec3 hash, float x, float y, float z) {
    return Vec3f(_GradientFloat(hash.x, x, y, z),
                   _GradientFloat(hash.y, x, y, z),
                   _GradientFloat(hash.z, x, y, z));
}

constexpr float _kGradScale2d = 0.6616f;
constexpr float _kGradScale3d = 0.9820f;

// -- Interpolation ----------------------------------------------------------

inline float _Fade(float t) {
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}
inline float _Lerp(float a, float b, float t) { return a + t * (b - a); }

inline float _Trilerp(float v0, float v1, float v2, float v3,
                       float v4, float v5, float v6, float v7,
                       float s, float t, float r) {
    float s1 = 1.0f - s, t1 = 1.0f - t, r1 = 1.0f - r;
    return r1*(t1*(v0*s1 + v1*s) + t*(v2*s1 + v3*s)) +
            r*(t1*(v4*s1 + v5*s) + t*(v6*s1 + v7*s));
}
inline float _Bilerp(float v0, float v1, float v2, float v3,
                      float s, float t) {
    float s1 = 1.0f - s;
    return (1.0f - t) * (v0*s1 + v1*s) + t * (v2*s1 + v3*s);
}
inline Vec3f _Trilerp(Vec3f v0, Vec3f v1, Vec3f v2, Vec3f v3,
                          Vec3f v4, Vec3f v5, Vec3f v6, Vec3f v7,
                          float s, float t, float r) {
    float s1 = 1.0f - s, t1 = 1.0f - t, r1 = 1.0f - r;
    return (v0*s1 + v1*s)*(t1*r1) + (v2*s1 + v3*s)*(t*r1) +
           (v4*s1 + v5*s)*(t1*r) + (v6*s1 + v7*s)*(t*r);
}

// -- Fractional floor -------------------------------------------------------

inline float _FloorFrac(float x, int& i) {
    i = static_cast<int>(std::floor(x));
    return x - float(i);
}

// -- Perlin noise -----------------------------------------------------------

float _PerlinNoise2d(float px, float py) {
    int X, Y;
    float fx = _FloorFrac(px, X);
    float fy = _FloorFrac(py, Y);
    float u = _Fade(fx), v = _Fade(fy);
    return _kGradScale2d * _Bilerp(
        _GradientFloat(_HashInt(X,   Y  ), fx,      fy),
        _GradientFloat(_HashInt(X+1, Y  ), fx-1.0f, fy),
        _GradientFloat(_HashInt(X,   Y+1), fx,      fy-1.0f),
        _GradientFloat(_HashInt(X+1, Y+1), fx-1.0f, fy-1.0f),
        u, v);
}

float _PerlinNoise3d(float px, float py, float pz) {
    int X, Y, Z;
    float fx = _FloorFrac(px, X);
    float fy = _FloorFrac(py, Y);
    float fz = _FloorFrac(pz, Z);
    float u = _Fade(fx), v = _Fade(fy), w = _Fade(fz);
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

Vec3f _PerlinNoise3d_vec3(float px, float py, float pz) {
    int X, Y, Z;
    float fx = _FloorFrac(px, X);
    float fy = _FloorFrac(py, Y);
    float fz = _FloorFrac(pz, Z);
    float u = _Fade(fx), v = _Fade(fy), w = _Fade(fz);
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

// -- Cell noise -------------------------------------------------------------

float _CellNoise2d(float x, float y) {
    return _BitsTo01(_HashInt(
        static_cast<int>(std::floor(x)),
        static_cast<int>(std::floor(y))));
}
float _CellNoise3d(float x, float y, float z) {
    return _BitsTo01(_HashInt(
        static_cast<int>(std::floor(x)),
        static_cast<int>(std::floor(y)),
        static_cast<int>(std::floor(z))));
}

} // anonymous namespace

// ---- Noise nodes ---------------------------------------------------------

static void
_EvalNoise3d_float(const ParamMap& inputs,
                   const ShadingContext& ctx,
                   NodeOutputMap* outputs)
{
    Vec3f pos = Get<Vec3f>(inputs, _kPosition, ctx.position);
    float amp   = Get<float>(inputs, _kAmplitude, 1.0f);
    float pivot = Get<float>(inputs, _kPivot,     0.0f);
    float n = _PerlinNoise3d(pos[0], pos[1], pos[2]);
    (*outputs)[_kOut] = Value(pivot + n * amp);
}

static void
_EvalNoise3d_color3(const ParamMap& inputs,
                    const ShadingContext& ctx,
                    NodeOutputMap* outputs)
{
    Vec3f pos = Get<Vec3f>(inputs, _kPosition, ctx.position);
    Vec3f amp = Get<Vec3f>(inputs, _kAmplitude, Vec3f(1.0f));
    Vec3f pivot = Get<Vec3f>(inputs, _kPivot, Vec3f(0.0f));
    Vec3f n = _PerlinNoise3d_vec3(pos[0], pos[1], pos[2]);
    (*outputs)[_kOut] = Value(pivot + CompMult(n, amp));
}

static void
_EvalNoise2d_float(const ParamMap& inputs,
                   const ShadingContext& ctx,
                   NodeOutputMap* outputs)
{
    Vec2f tc = Get<Vec2f>(inputs, _kTexcoord, ctx.texcoord);
    float amp  = Get<float>(inputs, _kAmplitude, 1.0f);
    float pivot = Get<float>(inputs, _kPivot, 0.0f);
    float n = _PerlinNoise2d(tc[0], tc[1]);
    (*outputs)[_kOut] = Value(pivot + n * amp);
}

static void
_EvalFractal3d_float(const ParamMap& inputs,
                     const ShadingContext& ctx,
                     NodeOutputMap* outputs)
{
    Vec3f pos    = Get<Vec3f>(inputs, _kPosition, ctx.position);
    float amp      = Get<float>(inputs, _kAmplitude,  1.0f);
    int   octaves  = Get<int>(inputs, _kOctaves, 3);
    float lacunarity = Get<float>(inputs, _kLacunarity, 2.0f);
    float diminish   = Get<float>(inputs, _kDiminish,   0.5f);

    float result = 0.0f;
    float weight = 1.0f;
    Vec3f p = pos;
    for (int i = 0; i < octaves; ++i) {
        result += weight * _PerlinNoise3d(p[0], p[1], p[2]);
        p *= lacunarity;
        weight *= diminish;
    }
    (*outputs)[_kOut] = Value(result * amp);
}

static void
_EvalCellnoise3d(const ParamMap& inputs,
                 const ShadingContext& ctx,
                 NodeOutputMap* outputs)
{
    Vec3f pos = Get<Vec3f>(inputs, _kPosition, ctx.position);
    (*outputs)[_kOut] = Value(
        _CellNoise3d(pos[0], pos[1], pos[2]));
}

static void
_EvalCellnoise2d(const ParamMap& inputs,
                 const ShadingContext& ctx,
                 NodeOutputMap* outputs)
{
    Vec2f tc = Get<Vec2f>(inputs, _kTexcoord, ctx.texcoord);
    (*outputs)[_kOut] = Value(_CellNoise2d(tc[0], tc[1]));
}

// ---- Registration --------------------------------------------------------

#define _REG(name, fn) reg.Register(name, fn)

void
RegisterProceduralNodes(NodeRegistry& reg)
{
    _REG("ND_noise3d_float",    &_EvalNoise3d_float);
    _REG("ND_noise3d_color3",   &_EvalNoise3d_color3);
    _REG("ND_noise2d_float",    &_EvalNoise2d_float);
    _REG("ND_fractal3d_float",  &_EvalFractal3d_float);
    _REG("ND_cellnoise3d_float", &_EvalCellnoise3d);
    _REG("ND_cellnoise2d_float", &_EvalCellnoise2d);
}

#undef _REG

} // namespace mxcpp
