//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "procedural3dNodes.h"
#include "helpers/proceduralHelpers.h"
#include "../nodeRegistry.h"

namespace mxcpp {

static const SlotName _kAmplitude("amplitude");
static const SlotName _kBitangent("bitangent");
static const SlotName _kBrightnesshigh("brightnesshigh");
static const SlotName _kBrightnesslow("brightnesslow");
static const SlotName _kClampoutput("clampoutput");
static const SlotName _kCoverage("coverage");
static const SlotName _kDiminish("diminish");
static const SlotName _kFlakenormal("flakenormal");
static const SlotName _kFreq("freq");
static const SlotName _kHuehigh("huehigh");
static const SlotName _kHuelow("huelow");
static const SlotName _kId("id");
static const SlotName _kIn("in");
static const SlotName _kJitter("jitter");
static const SlotName _kLacunarity("lacunarity");
static const SlotName _kNormal("normal");
static const SlotName _kOctaves("octaves");
static const SlotName _kOffset("offset");
static const SlotName _kOut("out");
static const SlotName _kOutmax("outmax");
static const SlotName _kOutmin("outmin");
static const SlotName _kPivot("pivot");
static const SlotName _kPosition("position");
static const SlotName _kPresence("presence");
static const SlotName _kRand("rand");
static const SlotName _kRoughness("roughness");
static const SlotName _kSaturationhigh("saturationhigh");
static const SlotName _kSaturationlow("saturationlow");
static const SlotName _kSeed("seed");
static const SlotName _kSize("size");
static const SlotName _kStyle("style");
static const SlotName _kTangent("tangent");
static const SlotName _kType("type");

static void
_EvalNoise3dFloat(const ParamMap& inputs,
                  const ShadingContext& ctx,
                  NodeOutputMap* outputs)
{
    const Vec3f pos = Get<Vec3f>(inputs, _kPosition, ctx.position);
    const float amplitude = Get<float>(inputs, _kAmplitude, 1.0f);
    const float pivot = Get<float>(inputs, _kPivot, 0.0f);
    StoreTypedOutput(outputs, _kOut,
                     PerlinNoise3d(pos[0], pos[1], pos[2]) * amplitude + pivot);
}

static void
_EvalNoise3dColor3(const ParamMap& inputs,
                   const ShadingContext& ctx,
                   NodeOutputMap* outputs)
{
    const Vec3f pos = Get<Vec3f>(inputs, _kPosition, ctx.position);
    const Vec3f amplitude = ReadAmplitude<Vec3f>(inputs, _kAmplitude);
    const float pivot = Get<float>(inputs, _kPivot, 0.0f);
    const Vec3f value = PerlinNoise3dVec3(pos[0], pos[1], pos[2]);
    StoreTypedOutput(outputs, _kOut,
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
        result += weight * PerlinNoise3d(pos[0], pos[1], pos[2]);
        pos *= lacunarity;
        weight *= diminish;
    }
    StoreTypedOutput(outputs, _kOut, result * amplitude);
}

static void
_EvalCellnoise3d(const ParamMap& inputs,
                 const ShadingContext& ctx,
                 NodeOutputMap* outputs)
{
    const Vec3f pos = Get<Vec3f>(inputs, _kPosition, ctx.position);
    StoreTypedOutput(outputs, _kOut,
                     CellNoise3d(pos[0], pos[1], pos[2]));
}

static void
_EvalWorleyNoise3dFloat(const ParamMap& inputs,
                        const ShadingContext& ctx,
                        NodeOutputMap* outputs)
{
    const Vec3f pos = Get<Vec3f>(inputs, _kPosition, ctx.position);
    const float jitter = Get<float>(inputs, _kJitter, 1.0f);
    const int style = Get<int>(inputs, _kStyle, 0);
    StoreTypedOutput(outputs, _kOut,
                     WorleyNoise3dFloat(pos, jitter, style));
}

static void
_EvalWorleyNoise3dVec2(const ParamMap& inputs,
                       const ShadingContext& ctx,
                       NodeOutputMap* outputs)
{
    const Vec3f pos = Get<Vec3f>(inputs, _kPosition, ctx.position);
    const float jitter = Get<float>(inputs, _kJitter, 1.0f);
    const int style = Get<int>(inputs, _kStyle, 0);
    StoreTypedOutput(outputs, _kOut,
                     WorleyNoise3dVec2(pos, jitter, style));
}

static void
_EvalWorleyNoise3dVec3(const ParamMap& inputs,
                       const ShadingContext& ctx,
                       NodeOutputMap* outputs)
{
    const Vec3f pos = Get<Vec3f>(inputs, _kPosition, ctx.position);
    const float jitter = Get<float>(inputs, _kJitter, 1.0f);
    const int style = Get<int>(inputs, _kStyle, 0);
    StoreTypedOutput(outputs, _kOut,
                     WorleyNoise3dVec3(pos, jitter, style));
}

static void
_EvalUnifiedNoise3dFloat(const ParamMap& inputs,
                         const ShadingContext& ctx,
                         NodeOutputMap* outputs)
{
    const Vec3f position = Get<Vec3f>(inputs, _kPosition, ctx.position);
    const Vec3f freq = Get<Vec3f>(inputs, _kFreq, Vec3f(1.0f));
    const Vec3f offset = Get<Vec3f>(inputs, _kOffset, Vec3f(0.0f));
    const float jitter = Get<float>(inputs, _kJitter, 1.0f);
    const float outMin = Get<float>(inputs, _kOutmin, 0.0f);
    const float outMax = Get<float>(inputs, _kOutmax, 1.0f);
    const bool clampOutput = Get<bool>(inputs, _kClampoutput, true);
    const int octaves = Get<int>(inputs, _kOctaves, 3);
    const float lacunarity = Get<float>(inputs, _kLacunarity, 2.0f);
    const float diminish = Get<float>(inputs, _kDiminish, 0.5f);
    const int type = Get<int>(inputs, _kType, 0);
    const int style = Get<int>(inputs, _kStyle, 0);

    const Vec3f applyFreq = CompMult(position, freq);
    const Vec3f applyOffset = applyFreq + offset;
    const float cellJitterMult = (jitter - 1.0f) * 90000.0f;
    const Vec3f applyCellJitter =
        Rotate3d(applyOffset, cellJitterMult, Vec3f(0.1f, 1.0f, 0.0f));

    float value = 0.0f;
    switch (type) {
    case 1:
        value = CellNoise3d(applyCellJitter[0],
                            applyCellJitter[1],
                            applyCellJitter[2]);
        break;
    case 2:
        value = WorleyNoise3dFloat(applyOffset, jitter, style);
        break;
    case 3:
        {
            Vec3f p = applyCellJitter;
            float weight = 1.0f;
            for (int i = 0; i < octaves; ++i) {
                value += weight * PerlinNoise3d(p[0], p[1], p[2]);
                p *= lacunarity;
                weight *= diminish;
            }
        }
        break;
    case 0:
    default:
        value = PerlinNoise3d(applyCellJitter[0],
                              applyCellJitter[1],
                              applyCellJitter[2]) * 0.5f + 0.5f;
        break;
    }

    value = Remap(value, 0.0f, 1.0f, outMin, outMax);
    if (clampOutput) {
        const float lo = std::min(outMin, outMax);
        const float hi = std::max(outMin, outMax);
        value = ClampValue(value, lo, hi);
    }
    StoreTypedOutput(outputs, _kOut, value);
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

    const float hue = RandomFloatValue(inputValue, hueLow, hueHigh, seedHue);
    const float saturation =
        RandomFloatValue(inputValue, satLow, satHigh, seedSaturation);
    const float brightness =
        RandomFloatValue(inputValue, brightLow, brightHigh, seedBrightness);
    return HsvToRgb(Vec3f(hue, saturation, brightness));
}

static void
_EvalRandomColorFloat(const ParamMap& inputs,
                      const ShadingContext&,
                      NodeOutputMap* outputs)
{
    const float inputValue = Get<float>(inputs, _kIn, 0.0f);
    const int seed = Get<int>(inputs, _kSeed, 0);
    StoreTypedOutput(outputs, _kOut, _EvalRandomColor(
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
    StoreTypedOutput(outputs, _kOut, _EvalRandomColor(
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
_EvalFlake3d(const ParamMap& inputs,
             const ShadingContext& ctx,
             NodeOutputMap* outputs)
{
    const float size = Get<float>(inputs, _kSize, 0.01f);
    const float roughness = Get<float>(inputs, _kRoughness, 0.1f);
    const float coverage = Get<float>(inputs, _kCoverage, 0.5f);
    const Vec3f position = Get<Vec3f>(inputs, _kPosition, ctx.position);
    const Vec3f normal = Get<Vec3f>(inputs, _kNormal, ctx.normal);
    const Vec3f tangent = Get<Vec3f>(inputs, _kTangent, ctx.tangent);
    const Vec3f bitangent = Get<Vec3f>(inputs, _kBitangent, ctx.bitangent);

    int id = 0;
    float rand = 0.0f;
    float presence = 0.0f;
    Vec3f flakeNormal = normal;
    EvalFlake(position,
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

    StoreTypedOutput(outputs, _kId, id);
    StoreTypedOutput(outputs, _kRand, rand);
    StoreTypedOutput(outputs, _kPresence, presence);
    StoreTypedOutput(outputs, _kFlakenormal, flakeNormal);
}

#define _REG(name, fn) reg.Register(name, fn)

void
RegisterProcedural3dNodes(NodeRegistry& reg)
{
    _REG("ND_noise3d_float", &_EvalNoise3dFloat);
    _REG("ND_noise3d_color3", &_EvalNoise3dColor3);
    _REG("ND_fractal3d_float", &_EvalFractal3dFloat);
    _REG("ND_cellnoise3d_float", &_EvalCellnoise3d);
    _REG("ND_worleynoise3d_float", &_EvalWorleyNoise3dFloat);
    _REG("ND_worleynoise3d_vector2", &_EvalWorleyNoise3dVec2);
    _REG("ND_worleynoise3d_vector3", &_EvalWorleyNoise3dVec3);
    _REG("ND_unifiednoise3d_float", &_EvalUnifiedNoise3dFloat);
    _REG("ND_randomcolor_float", &_EvalRandomColorFloat);
    _REG("ND_randomcolor_integer", &_EvalRandomColorInteger);
    _REG("ND_flake3d", &_EvalFlake3d);
}

#undef _REG

}  // namespace mxcpp
