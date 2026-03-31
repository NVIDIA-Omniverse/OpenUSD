//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "../nodeRegistry.h"
#include "../types.h"

#include <cmath>
#include <cstdio>
#include <functional>
#include <unordered_map>

using namespace mxcpp;

void Test_Register(const char* name, std::function<bool()> fn);
bool Test_IsClose(float a, float b, float eps = 1e-5f);
bool Test_IsClose(const Vec3f& a, const Vec3f& b, float eps = 1e-5f);

#define _REG(name) Test_Register("Nodes." #name, &name)

// Helper: evaluate a registered node with given inputs.
static NodeOutputMap
_Eval(const char* nodeTypeId, const ParamMap& inputs)
{
    NodeRegistry::RegisterBuiltinNodes();
    auto fn = NodeRegistry::GetInstance().Find(std::string(nodeTypeId));
    NodeOutputMap outputs;
    ShadingContext ctx;
    if (fn) {
        fn(inputs, ctx, &outputs);
    }
    return outputs;
}

static NodeOutputMap
_EvalWithCtx(const char* nodeTypeId,
             const ParamMap& inputs,
             const ShadingContext& ctx)
{
    NodeRegistry::RegisterBuiltinNodes();
    auto fn = NodeRegistry::GetInstance().Find(std::string(nodeTypeId));
    NodeOutputMap outputs;
    if (fn) {
        fn(inputs, ctx, &outputs);
    }
    return outputs;
}

static float _GetFloat(const NodeOutputMap& o, const char* name = "out") {
    const Value* value = o.Find(name);
    if (value && ValueHolds<float>(*value))
        return ValueGet<float>(*value);
    return -9999.0f;
}

static Vec3f _GetVec3(const NodeOutputMap& o, const char* name = "out") {
    const Value* value = o.Find(name);
    if (value && ValueHolds<Vec3f>(*value))
        return ValueGet<Vec3f>(*value);
    return Vec3f(-9999.0f);
}

static Vec2f _GetVec2(const NodeOutputMap& o, const char* name = "out") {
    const Value* value = o.Find(name);
    if (value && ValueHolds<Vec2f>(*value))
        return ValueGet<Vec2f>(*value);
    return Vec2f(-9999.0f);
}

static Vec4f _GetVec4(const NodeOutputMap& o, const char* name = "out") {
    const Value* value = o.Find(name);
    if (value && ValueHolds<Vec4f>(*value))
        return ValueGet<Vec4f>(*value);
    return Vec4f(-9999.0f);
}

static bool _GetBool(const NodeOutputMap& o, const char* name = "out") {
    const Value* value = o.Find(name);
    if (value && ValueHolds<bool>(*value))
        return ValueGet<bool>(*value);
    return false;
}

static int _GetInt(const NodeOutputMap& o, const char* name = "out") {
    const Value* value = o.Find(name);
    if (value && ValueHolds<int>(*value))
        return ValueGet<int>(*value);
    return -9999;
}

static bool Test_IsClose(const Vec2f& a, const Vec2f& b, float eps = 1e-5f) {
    return Test_IsClose(a[0], b[0], eps) && Test_IsClose(a[1], b[1], eps);
}

static bool Test_IsClose(const Vec4f& a, const Vec4f& b, float eps = 1e-5f) {
    return Test_IsClose(a[0], b[0], eps) && Test_IsClose(a[1], b[1], eps) &&
           Test_IsClose(a[2], b[2], eps) && Test_IsClose(a[3], b[3], eps);
}

static Mat4f _MakeTranslationMatrix(const Vec3f& t) {
    Mat4f m(1.0f);
    m[3][0] = t[0];
    m[3][1] = t[1];
    m[3][2] = t[2];
    return m;
}

static void _SetObjectWorldTransform(ShadingContext* ctx, const Mat4f& objectToWorld) {
    if (!ctx) {
        return;
    }
    ctx->objectToWorldMatrix = objectToWorld;
    ctx->worldToObjectMatrix = objectToWorld.inverse();
    ctx->hasObjectToWorldTransform = true;
    ctx->hasWorldToObjectTransform = true;
}

static bool
_EvalHeightFromTexcoordX(const void*,
                         int,
                         SlotId,
                         const ShadingContext& ctx,
                         Value* out)
{
    if (out) {
        *out = Value(ctx.texcoord[0]);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Math node tests
// ---------------------------------------------------------------------------

static bool TestAddFloat() {
    ParamMap in;
    in["in1"] = Value(2.0f);
    in["in2"] = Value(3.0f);
    auto out = _Eval("ND_add_float", in);
    return Test_IsClose(_GetFloat(out), 5.0f);
}

static bool TestAddColor3() {
    ParamMap in;
    in["in1"] = Value(Vec3f(1, 2, 3));
    in["in2"] = Value(Vec3f(4, 5, 6));
    auto out = _Eval("ND_add_color3", in);
    return Test_IsClose(_GetVec3(out), Vec3f(5, 7, 9));
}

static bool TestMultiplyFloat() {
    ParamMap in;
    in["in1"] = Value(3.0f);
    in["in2"] = Value(4.0f);
    auto out = _Eval("ND_multiply_float", in);
    return Test_IsClose(_GetFloat(out), 12.0f);
}

static bool TestSubtractFloat() {
    ParamMap in;
    in["in1"] = Value(10.0f);
    in["in2"] = Value(3.0f);
    auto out = _Eval("ND_subtract_float", in);
    return Test_IsClose(_GetFloat(out), 7.0f);
}

static bool TestDivideFloat() {
    ParamMap in;
    in["in1"] = Value(10.0f);
    in["in2"] = Value(4.0f);
    auto out = _Eval("ND_divide_float", in);
    if (!Test_IsClose(_GetFloat(out), 2.5f)) return false;

    // Division by zero should be safe.
    in["in2"] = Value(0.0f);
    out = _Eval("ND_divide_float", in);
    return Test_IsClose(_GetFloat(out), 0.0f);
}

static bool TestClamp() {
    ParamMap in;
    in["in"] = Value(1.5f);
    in["low"] = Value(0.0f);
    in["high"] = Value(1.0f);
    auto out = _Eval("ND_clamp_float", in);
    if (!Test_IsClose(_GetFloat(out), 1.0f)) return false;

    in["in"] = Value(-0.5f);
    out = _Eval("ND_clamp_float", in);
    return Test_IsClose(_GetFloat(out), 0.0f);
}

static bool TestMix() {
    ParamMap in;
    in["fg"] = Value(10.0f);
    in["bg"] = Value(0.0f);

    in["mix"] = Value(0.0f);
    auto out = _Eval("ND_mix_float", in);
    if (!Test_IsClose(_GetFloat(out), 0.0f)) return false;

    in["mix"] = Value(1.0f);
    out = _Eval("ND_mix_float", in);
    if (!Test_IsClose(_GetFloat(out), 10.0f)) return false;

    in["mix"] = Value(0.5f);
    out = _Eval("ND_mix_float", in);
    return Test_IsClose(_GetFloat(out), 5.0f);
}

static bool TestSmoothstep() {
    ParamMap in;
    in["low"] = Value(0.0f);
    in["high"] = Value(1.0f);

    in["in"] = Value(0.0f);
    auto out = _Eval("ND_smoothstep_float", in);
    if (!Test_IsClose(_GetFloat(out), 0.0f)) return false;

    in["in"] = Value(1.0f);
    out = _Eval("ND_smoothstep_float", in);
    if (!Test_IsClose(_GetFloat(out), 1.0f)) return false;

    in["in"] = Value(0.5f);
    out = _Eval("ND_smoothstep_float", in);
    return Test_IsClose(_GetFloat(out), 0.5f);
}

static bool TestRemap() {
    ParamMap in;
    in["in"] = Value(0.5f);
    in["inlow"] = Value(0.0f);
    in["inhigh"] = Value(1.0f);
    in["outlow"] = Value(10.0f);
    in["outhigh"] = Value(20.0f);
    auto out = _Eval("ND_remap_float", in);
    return Test_IsClose(_GetFloat(out), 15.0f);
}

// ---------------------------------------------------------------------------
// Channel node tests
// ---------------------------------------------------------------------------

static bool TestCombineSeparateRoundtrip() {
    ParamMap in;
    in["in1"] = Value(0.2f);
    in["in2"] = Value(0.4f);
    in["in3"] = Value(0.6f);
    auto combOut = _Eval("ND_combine3_color3", in);
    Vec3f combined = _GetVec3(combOut);

    ParamMap in2;
    in2["in"] = Value(combined);
    auto sepOut = _Eval("ND_separate3_color3", in2);
    float x = _GetFloat(sepOut, "outx");
    float y = _GetFloat(sepOut, "outy");
    float z = _GetFloat(sepOut, "outz");
    return Test_IsClose(x, 0.2f) &&
           Test_IsClose(y, 0.4f) &&
           Test_IsClose(z, 0.6f);
}

static bool TestConvertColor4ToColor3() {
    ParamMap in;
    in["in"] = Value(Vec4f(0.2f, 0.4f, 0.6f, 0.8f));

    auto colorOut = _Eval("ND_convert_color4_color3", in);
    if (!Test_IsClose(_GetVec3(colorOut), Vec3f(0.2f, 0.4f, 0.6f), 1e-6f)) {
        return false;
    }

    auto vec2Out = _Eval("ND_convert_vector4_vector2", in);
    return Test_IsClose(_GetVec2(vec2Out), Vec2f(0.2f, 0.4f), 1e-6f);
}

// ---------------------------------------------------------------------------
// Conditional node tests
// ---------------------------------------------------------------------------

static bool TestIfgreater() {
    NodeRegistry::RegisterBuiltinNodes();
    auto fn = NodeRegistry::GetInstance().Find(std::string("ND_ifgreater_float"));
    if (!fn) {
        printf("    ND_ifgreater_float not registered\n");
        return false;
    }
    // Just verify the node exists and is callable.
    ParamMap in;
    in["value1"] = Value(2.0f);
    in["value2"] = Value(1.0f);
    in["in1"] = Value(10.0f);
    in["in2"] = Value(20.0f);
    NodeOutputMap out;
    ShadingContext ctx;
    fn(in, ctx, &out);
    return Test_IsClose(_GetFloat(out), 10.0f);
}

static bool TestIfgreaterInteger() {
    ParamMap in;
    in["value1"] = Value(2.0f);
    in["value2"] = Value(1.0f);
    in["in1"] = Value(10);
    in["in2"] = Value(20);
    auto out = _Eval("ND_ifgreater_integer", in);
    return _GetInt(out) == 10;
}

static bool TestIfgreaterVector2() {
    ParamMap in;
    in["value1"] = Value(0.0f);
    in["value2"] = Value(1.0f);
    in["in1"] = Value(Vec2f(1.0f, 2.0f));
    in["in2"] = Value(Vec2f(3.0f, 4.0f));
    auto out = _Eval("ND_ifgreater_vector2", in);
    return Test_IsClose(_GetVec2(out), Vec2f(3.0f, 4.0f));
}

static bool TestIfgreaterBoolOutput() {
    ParamMap in;
    in["value1"] = Value(2.0f);
    in["value2"] = Value(1.0f);
    auto out = _Eval("ND_ifgreater_boolean", in);
    if (!_GetBool(out)) return false;

    in["value1"] = Value(0.0f);
    out = _Eval("ND_ifgreater_boolean", in);
    return !_GetBool(out);
}

static bool TestIfgreaterIntComparison() {
    ParamMap in;
    in["value1"] = Value(5);
    in["value2"] = Value(3);
    in["in1"] = Value(Vec3f(1.0f));
    in["in2"] = Value(Vec3f(0.0f));
    auto out = _Eval("ND_ifgreater_color3I", in);
    return Test_IsClose(_GetVec3(out), Vec3f(1.0f));
}

static bool TestIfEqualBoolComparison() {
    ParamMap in;
    in["value1"] = Value(true);
    in["value2"] = Value(true);
    in["in1"] = Value(10.0f);
    in["in2"] = Value(20.0f);
    auto out = _Eval("ND_ifequal_floatB", in);
    if (!Test_IsClose(_GetFloat(out), 10.0f)) return false;

    in["value2"] = Value(false);
    out = _Eval("ND_ifequal_floatB", in);
    return Test_IsClose(_GetFloat(out), 20.0f);
}

static bool TestSwitchFloat() {
    ParamMap in;
    in["in1"] = Value(10.0f);
    in["in2"] = Value(20.0f);
    in["in3"] = Value(30.0f);
    in["which"] = Value(1.0f);
    auto out = _Eval("ND_switch_float", in);
    return Test_IsClose(_GetFloat(out), 20.0f);
}

static bool TestSwitchIntegerWhich() {
    ParamMap in;
    in["in1"] = Value(Vec3f(1.0f));
    in["in2"] = Value(Vec3f(2.0f));
    in["in3"] = Value(Vec3f(3.0f));
    in["which"] = Value(2);
    auto out = _Eval("ND_switch_color3I", in);
    return Test_IsClose(_GetVec3(out), Vec3f(3.0f));
}

static bool TestLogicalOps() {
    {
        ParamMap in;
        in["in1"] = Value(true);
        in["in2"] = Value(false);
        auto out = _Eval("ND_logical_and", in);
        if (_GetBool(out)) return false;
    }
    {
        ParamMap in;
        in["in1"] = Value(true);
        in["in2"] = Value(false);
        auto out = _Eval("ND_logical_or", in);
        if (!_GetBool(out)) return false;
    }
    {
        ParamMap in;
        in["in1"] = Value(true);
        in["in2"] = Value(true);
        auto out = _Eval("ND_logical_xor", in);
        if (_GetBool(out)) return false;
    }
    {
        ParamMap in;
        in["in"] = Value(false);
        auto out = _Eval("ND_logical_not", in);
        if (!_GetBool(out)) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Compositing node tests
// ---------------------------------------------------------------------------

static bool TestCompositingPlus() {
    ParamMap in;
    in["fg"] = Value(0.3f);
    in["bg"] = Value(0.4f);
    in["mix"] = Value(1.0f);
    auto out = _Eval("ND_plus_float", in);
    return Test_IsClose(_GetFloat(out), 0.7f);
}

static bool TestCompositingMinus() {
    ParamMap in;
    in["fg"] = Value(0.3f);
    in["bg"] = Value(0.5f);
    in["mix"] = Value(1.0f);
    auto out = _Eval("ND_minus_float", in);
    return Test_IsClose(_GetFloat(out), 0.2f);
}

static bool TestCompositingBurn() {
    ParamMap in;
    in["fg"] = Value(0.5f);
    in["bg"] = Value(0.8f);
    in["mix"] = Value(1.0f);
    auto out = _Eval("ND_burn_float", in);
    if (!Test_IsClose(_GetFloat(out), 0.6f)) return false;

    in["fg"] = Value(0.0f);
    out = _Eval("ND_burn_float", in);
    return Test_IsClose(_GetFloat(out), 0.0f);
}

static bool TestCompositingDodge() {
    ParamMap in;
    in["fg"] = Value(0.5f);
    in["bg"] = Value(0.4f);
    in["mix"] = Value(1.0f);
    auto out = _Eval("ND_dodge_float", in);
    if (!Test_IsClose(_GetFloat(out), 0.8f)) return false;

    in["fg"] = Value(1.0f);
    out = _Eval("ND_dodge_float", in);
    return Test_IsClose(_GetFloat(out), 1.0f);
}

static bool TestCompositingScreen() {
    ParamMap in;
    in["fg"] = Value(0.5f);
    in["bg"] = Value(0.3f);
    in["mix"] = Value(1.0f);
    auto out = _Eval("ND_screen_float", in);
    return Test_IsClose(_GetFloat(out), 0.65f);
}

static bool TestCompositingOverlay() {
    ParamMap in;
    in["fg"] = Value(0.6f);
    in["bg"] = Value(0.3f);
    in["mix"] = Value(1.0f);
    auto out = _Eval("ND_overlay_float", in);
    if (!Test_IsClose(_GetFloat(out), 0.36f)) return false;

    in["bg"] = Value(0.7f);
    out = _Eval("ND_overlay_float", in);
    return Test_IsClose(_GetFloat(out), 0.76f);
}

static bool TestCompositingMixParam() {
    ParamMap in;
    in["fg"] = Value(0.8f);
    in["bg"] = Value(0.2f);
    in["mix"] = Value(0.0f);
    auto out = _Eval("ND_plus_float", in);
    return Test_IsClose(_GetFloat(out), 0.2f);
}

static bool TestPorterDuffOver() {
    ParamMap in;
    in["fg"] = Value(Vec4f(1.0f, 0.0f, 0.0f, 0.5f));
    in["bg"] = Value(Vec4f(0.0f, 1.0f, 0.0f, 0.8f));
    in["mix"] = Value(1.0f);
    auto out = _Eval("ND_over_color4", in);
    Vec4f result = _GetVec4(out);
    return Test_IsClose(result, Vec4f(1.0f, 0.5f, 0.0f, 0.9f));
}

static bool TestPorterDuffIn() {
    ParamMap in;
    in["fg"] = Value(Vec4f(1.0f, 0.5f, 0.0f, 0.8f));
    in["bg"] = Value(Vec4f(0.0f, 1.0f, 0.0f, 0.6f));
    in["mix"] = Value(1.0f);
    auto out = _Eval("ND_in_color4", in);
    Vec4f result = _GetVec4(out);
    return Test_IsClose(result, Vec4f(0.6f, 0.3f, 0.0f, 0.48f));
}

static bool TestPorterDuffDisjointover() {
    ParamMap in;
    in["fg"] = Value(Vec4f(0.5f, 0.0f, 0.0f, 0.3f));
    in["bg"] = Value(Vec4f(0.0f, 0.5f, 0.0f, 0.4f));
    in["mix"] = Value(1.0f);
    auto out = _Eval("ND_disjointover_color4", in);
    Vec4f result = _GetVec4(out);
    if (!Test_IsClose(result, Vec4f(0.5f, 0.5f, 0.0f, 0.7f)))
        return false;

    in["fg"] = Value(Vec4f(0.8f, 0.0f, 0.0f, 0.7f));
    in["bg"] = Value(Vec4f(0.0f, 0.6f, 0.0f, 0.5f));
    out = _Eval("ND_disjointover_color4", in);
    result = _GetVec4(out);
    return Test_IsClose(result, Vec4f(0.8f, 0.36f, 0.0f, 1.0f));
}

static bool TestPorterDuffMatte() {
    ParamMap in;
    in["fg"] = Value(Vec4f(1.0f, 0.0f, 0.0f, 0.6f));
    in["bg"] = Value(Vec4f(0.0f, 1.0f, 0.0f, 0.8f));
    in["mix"] = Value(1.0f);
    auto out = _Eval("ND_matte_color4", in);
    Vec4f result = _GetVec4(out);
    return Test_IsClose(result, Vec4f(0.6f, 0.4f, 0.0f, 0.92f));
}

static bool TestPorterDuffOut() {
    ParamMap in;
    in["fg"] = Value(Vec4f(1.0f, 0.5f, 0.0f, 0.8f));
    in["bg"] = Value(Vec4f(0.0f, 1.0f, 0.0f, 0.6f));
    in["mix"] = Value(1.0f);
    auto out = _Eval("ND_out_color4", in);
    Vec4f result = _GetVec4(out);
    return Test_IsClose(result, Vec4f(0.4f, 0.2f, 0.0f, 0.32f));
}

static bool TestPremultUnpremult() {
    ParamMap in;
    in["in"] = Value(Vec4f(1.0f, 0.5f, 0.0f, 0.5f));
    auto out = _Eval("ND_premult_color4", in);
    Vec4f result = _GetVec4(out);
    if (!Test_IsClose(result, Vec4f(0.5f, 0.25f, 0.0f, 0.5f)))
        return false;

    in["in"] = Value(Vec4f(0.5f, 0.25f, 0.0f, 0.5f));
    out = _Eval("ND_unpremult_color4", in);
    result = _GetVec4(out);
    return Test_IsClose(result, Vec4f(1.0f, 0.5f, 0.0f, 0.5f));
}

static bool TestInsideOutside() {
    ParamMap in;
    in["in"] = Value(Vec3f(0.5f, 0.8f, 1.0f));
    in["mask"] = Value(0.5f);
    auto out = _Eval("ND_inside_color3", in);
    if (!Test_IsClose(_GetVec3(out), Vec3f(0.25f, 0.4f, 0.5f)))
        return false;

    out = _Eval("ND_outside_color3", in);
    return Test_IsClose(_GetVec3(out), Vec3f(0.25f, 0.4f, 0.5f));
}

static bool TestMixVecVariant() {
    ParamMap in;
    in["fg"] = Value(Vec3f(1.0f, 0.0f, 0.0f));
    in["bg"] = Value(Vec3f(0.0f, 1.0f, 0.0f));
    in["mix"] = Value(Vec3f(1.0f, 0.0f, 0.5f));
    auto out = _Eval("ND_mix_color3_color3", in);
    return Test_IsClose(_GetVec3(out), Vec3f(1.0f, 1.0f, 0.0f));
}

// ---------------------------------------------------------------------------
// Procedural node tests
// ---------------------------------------------------------------------------

static bool TestProceduralWorleyNoise2d() {
    ParamMap in;
    in["texcoord"] = Value(Vec2f(0.25f, 0.75f));
    in["jitter"] = Value(0.0f);

    auto outFloat = _Eval("ND_worleynoise2d_float", in);
    if (!Test_IsClose(_GetFloat(outFloat), 0.35355339f, 1e-5f)) {
        return false;
    }

    auto outVec2 = _Eval("ND_worleynoise2d_vector2", in);
    return Test_IsClose(_GetVec2(outVec2),
                        Vec2f(0.35355339f, 0.79056942f),
                        1e-5f);
}

static bool TestProceduralWorleyNoise3d() {
    ParamMap in;
    in["position"] = Value(Vec3f(0.25f, 0.75f, 0.5f));
    in["jitter"] = Value(0.0f);

    auto outFloat = _Eval("ND_worleynoise3d_float", in);
    if (!Test_IsClose(_GetFloat(outFloat), 0.35355339f, 1e-5f)) {
        return false;
    }

    auto outVec2 = _Eval("ND_worleynoise3d_vector2", in);
    if (!Test_IsClose(_GetVec2(outVec2),
                      Vec2f(0.35355339f, 0.79056942f),
                      1e-5f)) {
        return false;
    }

    auto outVec3 = _Eval("ND_worleynoise3d_vector3", in);
    return Test_IsClose(_GetVec3(outVec3),
                        Vec3f(0.35355339f, 0.79056942f, 0.79056942f),
                        1e-5f);
}

static bool TestProceduralFractal2dSingleOctave() {
    ParamMap in;
    in["texcoord"] = Value(Vec2f(0.37f, 0.81f));
    in["amplitude"] = Value(1.0f);
    in["octaves"] = Value(1);
    in["lacunarity"] = Value(2.0f);
    in["diminish"] = Value(0.5f);

    auto fractal = _Eval("ND_fractal2d_float", in);

    ParamMap noiseIn;
    noiseIn["texcoord"] = Value(Vec2f(0.37f, 0.81f));
    noiseIn["amplitude"] = Value(1.0f);
    noiseIn["pivot"] = Value(0.0f);
    auto noise = _Eval("ND_noise2d_float", noiseIn);
    return Test_IsClose(_GetFloat(fractal), _GetFloat(noise), 1e-5f);
}

static bool TestProceduralUnifiedNoise2dCellRemap() {
    ParamMap in;
    in["texcoord"] = Value(Vec2f(1.2f, 2.7f));
    in["freq"] = Value(Vec2f(1.0f, 1.0f));
    in["offset"] = Value(Vec2f(0.0f, 0.0f));
    in["jitter"] = Value(1.0f);
    in["type"] = Value(1);
    in["outmin"] = Value(2.0f);
    in["outmax"] = Value(4.0f);
    in["clampoutput"] = Value(true);

    auto unified = _Eval("ND_unifiednoise2d_float", in);

    ParamMap cellIn;
    cellIn["texcoord"] = Value(Vec2f(1.2f, 2.7f));
    auto cell = _Eval("ND_cellnoise2d_float", cellIn);
    const float expected = 2.0f + _GetFloat(cell) * 2.0f;
    return Test_IsClose(_GetFloat(unified), expected, 1e-5f);
}

static bool TestProceduralUnifiedNoise3dCellRemap() {
    ParamMap in;
    in["position"] = Value(Vec3f(1.2f, 2.7f, 0.4f));
    in["freq"] = Value(Vec3f(1.0f, 1.0f, 1.0f));
    in["offset"] = Value(Vec3f(0.0f, 0.0f, 0.0f));
    in["jitter"] = Value(1.0f);
    in["type"] = Value(1);
    in["outmin"] = Value(2.0f);
    in["outmax"] = Value(4.0f);
    in["clampoutput"] = Value(true);

    auto unified = _Eval("ND_unifiednoise3d_float", in);

    ParamMap cellIn;
    cellIn["position"] = Value(Vec3f(1.2f, 2.7f, 0.4f));
    auto cell = _Eval("ND_cellnoise3d_float", cellIn);
    const float expected = 2.0f + _GetFloat(cell) * 2.0f;
    return Test_IsClose(_GetFloat(unified), expected, 1e-5f);
}

static bool TestProceduralRampLrAndSplitLr() {
    {
        ParamMap in;
        in["valuel"] = Value(1.0f);
        in["valuer"] = Value(5.0f);
        in["texcoord"] = Value(Vec2f(0.25f, 0.0f));
        auto out = _Eval("ND_ramplr_float", in);
        if (!Test_IsClose(_GetFloat(out), 2.0f, 1e-5f)) {
            return false;
        }
    }

    {
        ParamMap in;
        in["valuel"] = Value(0.0f);
        in["valuer"] = Value(1.0f);
        in["center"] = Value(0.5f);
        in["texcoord"] = Value(Vec2f(0.5f, 0.0f));
        ShadingContext ctx;
        ctx.texcoord = Vec2f(0.5f, 0.0f);
        ctx.dudx = 1.0f;
        auto out = _EvalWithCtx("ND_splitlr_float", in, ctx);
        if (!Test_IsClose(_GetFloat(out), 0.5f, 1e-5f)) {
            return false;
        }
    }

    return true;
}

static bool TestProceduralRamp4AndRamp() {
    {
        ParamMap in;
        in["valuetl"] = Value(0.0f);
        in["valuetr"] = Value(1.0f);
        in["valuebl"] = Value(2.0f);
        in["valuebr"] = Value(3.0f);
        in["texcoord"] = Value(Vec2f(0.25f, 0.25f));
        auto out = _Eval("ND_ramp4_float", in);
        if (!Test_IsClose(_GetFloat(out), 0.75f, 1e-5f)) {
            return false;
        }
    }

    {
        ParamMap in;
        in["texcoord"] = Value(Vec2f(0.25f, 0.0f));
        in["type"] = Value(0);
        in["interpolation"] = Value(0);
        in["num_intervals"] = Value(2);
        in["interval1"] = Value(0.0f);
        in["interval2"] = Value(1.0f);
        in["color1"] = Value(Vec4f(0.0f, 0.0f, 0.0f, 1.0f));
        in["color2"] = Value(Vec4f(1.0f, 1.0f, 1.0f, 1.0f));
        auto out = _Eval("ND_ramp", in);
        return Test_IsClose(_GetVec4(out), Vec4f(0.25f, 0.25f, 0.25f, 1.0f));
    }
}

static bool TestProceduralPatterns() {
    {
        ParamMap in;
        in["texcoord"] = Value(Vec2f(0.2f, 0.1f));
        auto out = _Eval("ND_checkerboard_color3", in);
        if (!Test_IsClose(_GetVec3(out), Vec3f(1.0f), 1e-5f)) {
            return false;
        }
    }
    {
        ParamMap in;
        in["texcoord"] = Value(Vec2f(0.5f, 0.05f));
        in["center"] = Value(Vec2f(0.0f));
        in["radius"] = Value(0.1f);
        in["point1"] = Value(Vec2f(0.0f, 0.0f));
        in["point2"] = Value(Vec2f(1.0f, 0.0f));
        auto out = _Eval("ND_line_float", in);
        if (!Test_IsClose(_GetFloat(out), 1.0f, 1e-5f)) {
            return false;
        }
    }
    {
        ParamMap in;
        in["texcoord"] = Value(Vec2f(0.2f, 0.2f));
        in["center"] = Value(Vec2f(0.0f));
        in["radius"] = Value(0.5f);
        auto out = _Eval("ND_circle_float", in);
        if (!Test_IsClose(_GetFloat(out), 1.0f, 1e-5f)) {
            return false;
        }
    }
    {
        ParamMap in;
        in["texcoord"] = Value(Vec2f(0.0f, 0.0f));
        in["center"] = Value(Vec2f(0.0f));
        in["radius"] = Value(0.5f);
        auto out = _Eval("ND_hexagon_float", in);
        if (!Test_IsClose(_GetFloat(out), 1.0f, 1e-5f)) {
            return false;
        }
    }
    return true;
}

static bool TestProceduralGridAndCrosshatch() {
    {
        ParamMap in;
        in["texcoord"] = Value(Vec2f(0.0f, 0.0f));
        auto out = _Eval("ND_grid_color3", in);
        if (!Test_IsClose(_GetVec3(out), Vec3f(1.0f), 1e-5f)) {
            return false;
        }
    }
    {
        ParamMap in;
        in["texcoord"] = Value(Vec2f(0.5f, 0.5f));
        auto out = _Eval("ND_crosshatch_color3", in);
        if (!Test_IsClose(_GetVec3(out), Vec3f(1.0f), 1e-5f)) {
            return false;
        }
    }
    return true;
}

static bool TestProceduralRandomNodes() {
    ParamMap rf;
    rf["in"] = Value(0.25f);
    rf["min"] = Value(2.0f);
    rf["max"] = Value(4.0f);
    rf["seed"] = Value(7);
    auto out1 = _Eval("ND_randomfloat_float", rf);
    auto out2 = _Eval("ND_randomfloat_float", rf);
    const float randomFloat = _GetFloat(out1);
    if (randomFloat < 2.0f || randomFloat > 4.0f) {
        return false;
    }
    if (!Test_IsClose(randomFloat, _GetFloat(out2), 1e-6f)) {
        return false;
    }

    ParamMap rc;
    rc["in"] = Value(0.25f);
    rc["seed"] = Value(11);
    auto color1 = _Eval("ND_randomcolor_float", rc);
    auto color2 = _Eval("ND_randomcolor_float", rc);
    return Test_IsClose(_GetVec3(color1), _GetVec3(color2), 1e-6f);
}

static bool TestProceduralFlake2dCoverageZero() {
    ParamMap in;
    in["coverage"] = Value(0.0f);
    in["texcoord"] = Value(Vec2f(0.3f, 0.7f));
    in["normal"] = Value(Vec3f(0.0f, 0.0f, 1.0f));
    in["tangent"] = Value(Vec3f(1.0f, 0.0f, 0.0f));
    in["bitangent"] = Value(Vec3f(0.0f, 1.0f, 0.0f));

    auto out = _Eval("ND_flake2d", in);
    return _GetInt(out, "id") == 0 &&
           Test_IsClose(_GetFloat(out, "rand"), 0.0f, 1e-6f) &&
           Test_IsClose(_GetFloat(out, "presence"), 0.0f, 1e-6f) &&
           Test_IsClose(_GetVec3(out, "flakenormal"), Vec3f(0.0f, 0.0f, 1.0f));
}

static bool TestProceduralFlake3dCoverageZero() {
    ParamMap in;
    in["coverage"] = Value(0.0f);
    in["position"] = Value(Vec3f(0.3f, 0.7f, 0.2f));
    in["normal"] = Value(Vec3f(0.0f, 0.0f, 1.0f));
    in["tangent"] = Value(Vec3f(1.0f, 0.0f, 0.0f));
    in["bitangent"] = Value(Vec3f(0.0f, 1.0f, 0.0f));

    auto out = _Eval("ND_flake3d", in);
    return _GetInt(out, "id") == 0 &&
           Test_IsClose(_GetFloat(out, "rand"), 0.0f, 1e-6f) &&
           Test_IsClose(_GetFloat(out, "presence"), 0.0f, 1e-6f) &&
           Test_IsClose(_GetVec3(out, "flakenormal"), Vec3f(0.0f, 0.0f, 1.0f));
}

// ---------------------------------------------------------------------------
// Test helpers for geompropvalue
// ---------------------------------------------------------------------------

struct _TestGeomPropData {
    std::unordered_map<std::string, Value> props;
};

static Value
_TestGeomPropLookup(const void* userData, const std::string& name)
{
    auto* data = static_cast<const _TestGeomPropData*>(userData);
    auto it = data->props.find(name);
    if (it != data->props.end()) return it->second;
    return Value();
}

// ---------------------------------------------------------------------------
// Geompropvalue node tests
// ---------------------------------------------------------------------------

static bool TestGeomPropValueFloat() {
    _TestGeomPropData gpData;
    gpData.props["myFloat"] = Value(3.14f);

    ShadingContext ctx;
    ctx.geomPropLookup = &_TestGeomPropLookup;
    ctx.geomPropUserData = &gpData;

    ParamMap in;
    in["geomprop"] = Value(std::string("myFloat"));
    in["default"] = Value(0.0f);

    NodeRegistry::RegisterBuiltinNodes();
    auto fn = NodeRegistry::GetInstance().Find(
        std::string("ND_geompropvalue_float"));
    if (!fn) return false;

    NodeOutputMap out;
    fn(in, ctx, &out);
    return Test_IsClose(_GetFloat(out), 3.14f);
}

static bool TestGeomPropValueColor3() {
    _TestGeomPropData gpData;
    gpData.props["Cd"] = Value(Vec3f(1.0f, 0.0f, 0.5f));

    ShadingContext ctx;
    ctx.geomPropLookup = &_TestGeomPropLookup;
    ctx.geomPropUserData = &gpData;

    ParamMap in;
    in["geomprop"] = Value(std::string("Cd"));
    in["default"] = Value(Vec3f(0.0f));

    NodeRegistry::RegisterBuiltinNodes();
    auto fn = NodeRegistry::GetInstance().Find(
        std::string("ND_geompropvalue_color3"));
    if (!fn) return false;

    NodeOutputMap out;
    fn(in, ctx, &out);
    return Test_IsClose(_GetVec3(out), Vec3f(1.0f, 0.0f, 0.5f));
}

static bool TestGeomPropValueDefault() {
    // No callback set — should return default value.
    ShadingContext ctx;

    ParamMap in;
    in["geomprop"] = Value(std::string("missing"));
    in["default"] = Value(42.0f);

    NodeRegistry::RegisterBuiltinNodes();
    auto fn = NodeRegistry::GetInstance().Find(
        std::string("ND_geompropvalue_float"));
    if (!fn) return false;

    NodeOutputMap out;
    fn(in, ctx, &out);
    return Test_IsClose(_GetFloat(out), 42.0f);
}

static bool TestGeomPropValueTypeMismatch() {
    // Callback returns float but node expects Vec3f — should return default.
    _TestGeomPropData gpData;
    gpData.props["wrongType"] = Value(1.0f);

    ShadingContext ctx;
    ctx.geomPropLookup = &_TestGeomPropLookup;
    ctx.geomPropUserData = &gpData;

    ParamMap in;
    in["geomprop"] = Value(std::string("wrongType"));
    in["default"] = Value(Vec3f(0.5f));

    NodeRegistry::RegisterBuiltinNodes();
    auto fn = NodeRegistry::GetInstance().Find(
        std::string("ND_geompropvalue_color3"));
    if (!fn) return false;

    NodeOutputMap out;
    fn(in, ctx, &out);
    return Test_IsClose(_GetVec3(out), Vec3f(0.5f));
}

static bool TestGeomPropValueEmptyName() {
    // Empty property name should return default.
    _TestGeomPropData gpData;
    gpData.props["something"] = Value(99.0f);

    ShadingContext ctx;
    ctx.geomPropLookup = &_TestGeomPropLookup;
    ctx.geomPropUserData = &gpData;

    ParamMap in;
    in["geomprop"] = Value(std::string(""));
    in["default"] = Value(7.0f);

    NodeRegistry::RegisterBuiltinNodes();
    auto fn = NodeRegistry::GetInstance().Find(
        std::string("ND_geompropvalue_float"));
    if (!fn) return false;

    NodeOutputMap out;
    fn(in, ctx, &out);
    return Test_IsClose(_GetFloat(out), 7.0f);
}

static bool TestGeomPropValueNotFound() {
    // Callback set but property name not in data — should return default.
    _TestGeomPropData gpData;
    gpData.props["exists"] = Value(1.0f);

    ShadingContext ctx;
    ctx.geomPropLookup = &_TestGeomPropLookup;
    ctx.geomPropUserData = &gpData;

    ParamMap in;
    in["geomprop"] = Value(std::string("doesNotExist"));
    in["default"] = Value(99.0f);

    NodeRegistry::RegisterBuiltinNodes();
    auto fn = NodeRegistry::GetInstance().Find(
        std::string("ND_geompropvalue_float"));
    if (!fn) return false;

    NodeOutputMap out;
    fn(in, ctx, &out);
    return Test_IsClose(_GetFloat(out), 99.0f);
}

static bool TestGeomPropValueUniformString() {
    std::unordered_map<std::string, Value> uniformMap;
    uniformMap["textureset"] = Value(std::string("/path/to/texture.png"));

    ShadingContext ctx;
    ctx.uniformProps = &uniformMap;

    ParamMap in;
    in["geomprop"] = Value(std::string("textureset"));
    in["default"] = Value(std::string(""));

    NodeRegistry::RegisterBuiltinNodes();
    auto fn = NodeRegistry::GetInstance().Find(
        std::string("ND_geompropvalueuniform_string"));
    if (!fn) return false;

    NodeOutputMap out;
    fn(in, ctx, &out);
    const Value* val = out.Find("out");
    if (!val || !ValueHolds<std::string>(*val)) return false;
    return ValueGet<std::string>(*val) == "/path/to/texture.png";
}

static bool TestGeomPropValueUniformDefault() {
    // No uniformProps set — should return default.
    ShadingContext ctx;

    ParamMap in;
    in["geomprop"] = Value(std::string("missing"));
    in["default"] = Value(std::string("fallback.png"));

    NodeRegistry::RegisterBuiltinNodes();
    auto fn = NodeRegistry::GetInstance().Find(
        std::string("ND_geompropvalueuniform_filename"));
    if (!fn) return false;

    NodeOutputMap out;
    fn(in, ctx, &out);
    const Value* val = out.Find("out");
    if (!val || !ValueHolds<std::string>(*val)) return false;
    return ValueGet<std::string>(*val) == "fallback.png";
}

// ---------------------------------------------------------------------------
// Geometric node tests
// ---------------------------------------------------------------------------

static bool TestGeometricPosition() {
    NodeRegistry::RegisterBuiltinNodes();
    auto fn = NodeRegistry::GetInstance().Find(std::string("ND_position_vector3"));
    if (!fn) return false;

    ParamMap in;
    ShadingContext ctx;
    ctx.position = Vec3f(1.0f, 2.0f, 3.0f);
    NodeOutputMap out;
    fn(in, ctx, &out);
    return Test_IsClose(_GetVec3(out), Vec3f(1.0f, 2.0f, 3.0f));
}

static bool TestGeometricPositionWorldSpace() {
    NodeRegistry::RegisterBuiltinNodes();
    auto fn = NodeRegistry::GetInstance().Find(std::string("ND_position_vector3"));
    if (!fn) return false;

    ParamMap in;
    in["space"] = Value(std::string("world"));
    ShadingContext ctx;
    ctx.position = Vec3f(1.0f, 2.0f, 3.0f);
    _SetObjectWorldTransform(&ctx, _MakeTranslationMatrix(Vec3f(5.0f, 0.0f, -2.0f)));
    NodeOutputMap out;
    fn(in, ctx, &out);
    return Test_IsClose(_GetVec3(out), Vec3f(6.0f, 2.0f, 1.0f));
}

static bool TestGeometricNormal() {
    NodeRegistry::RegisterBuiltinNodes();
    auto fn = NodeRegistry::GetInstance().Find(std::string("ND_normal_vector3"));
    if (!fn) return false;

    ParamMap in;
    ShadingContext ctx;
    ctx.normal = Vec3f(0.0f, 1.0f, 0.0f);
    _SetObjectWorldTransform(&ctx, Mat4f(1.0f));
    NodeOutputMap out;
    fn(in, ctx, &out);
    return Test_IsClose(_GetVec3(out), Vec3f(0.0f, 1.0f, 0.0f));
}

static bool TestApplicationFrame() {
    NodeRegistry::RegisterBuiltinNodes();
    auto fn = NodeRegistry::GetInstance().Find(std::string("ND_frame_float"));
    if (!fn) return false;

    ParamMap in;
    ShadingContext ctx;
    ctx.frame = 42.5f;
    NodeOutputMap out;
    fn(in, ctx, &out);
    return Test_IsClose(_GetFloat(out), 42.5f);
}

static bool TestApplicationTime() {
    NodeRegistry::RegisterBuiltinNodes();
    auto fn = NodeRegistry::GetInstance().Find(std::string("ND_time_float"));
    if (!fn) return false;

    ParamMap in;
    ShadingContext ctx;
    ctx.time = 1.75f;
    NodeOutputMap out;
    fn(in, ctx, &out);
    return Test_IsClose(_GetFloat(out), 1.75f);
}

static bool TestTransformPointObjectToWorld() {
    ParamMap in;
    in["in"] = Value(Vec3f(1.0f, 2.0f, 3.0f));
    in["fromspace"] = Value(std::string("object"));
    in["tospace"] = Value(std::string("world"));

    ShadingContext ctx;
    _SetObjectWorldTransform(&ctx, _MakeTranslationMatrix(Vec3f(3.0f, -1.0f, 2.0f)));

    auto out = _EvalWithCtx("ND_transformpoint_vector3", in, ctx);
    return Test_IsClose(_GetVec3(out), Vec3f(4.0f, 1.0f, 5.0f));
}

static bool TestHeightToNormalDefaultTexcoord() {
    NodeRegistry::RegisterBuiltinNodes();
    auto fn = NodeRegistry::GetInstance().Find(
        std::string("ND_heighttonormal_vector3"));
    if (!fn) return false;

    ParamMap in;
    in.Add(
        AsSlotId("in"),
        nullptr,
        &_EvalHeightFromTexcoordX,
        nullptr,
        -1,
        InvalidSlotId);

    ShadingContext ctx;
    ctx.texcoord = Vec2f(0.25f, 0.5f);
    ctx.dudx = 1.0f;
    ctx.dvdy = 1.0f;

    NodeOutputMap out;
    fn(in, ctx, &out);

    Vec3f expected = Vec3f(-1.0f / 16.0f, 0.0f, 1.0f).normalized();
    expected = expected * 0.5f + Vec3f(0.5f, 0.5f, 0.5f);
    return Test_IsClose(_GetVec3(out), expected, 1e-5f);
}

static bool TestBumpDefaultBasis() {
    NodeRegistry::RegisterBuiltinNodes();
    auto fn = NodeRegistry::GetInstance().Find(std::string("ND_bump_vector3"));
    if (!fn) return false;

    ParamMap in;
    in.Add(
        AsSlotId("height"),
        nullptr,
        &_EvalHeightFromTexcoordX,
        nullptr,
        -1,
        InvalidSlotId);

    ShadingContext ctx;
    ctx.normal = Vec3f(0.0f, 0.0f, 1.0f);
    ctx.tangent = Vec3f(1.0f, 0.0f, 0.0f);
    ctx.bitangent = Vec3f(0.0f, 1.0f, 0.0f);
    ctx.dPdu = Vec3f(1.0f, 0.0f, 0.0f);
    ctx.dPdv = Vec3f(0.0f, 1.0f, 0.0f);
    ctx.texcoord = Vec2f(0.25f, 0.5f);
    ctx.dudx = 1.0f;
    ctx.dvdy = 1.0f;

    NodeOutputMap out;
    fn(in, ctx, &out);

    Vec3f expected = Vec3f(-1.0f, 0.0f, 1.0f).normalized();
    return Test_IsClose(_GetVec3(out), expected, 1e-5f);
}

// ---------------------------------------------------------------------------
// Color node tests
// ---------------------------------------------------------------------------

static bool TestLuminance() {
    NodeRegistry::RegisterBuiltinNodes();
    auto fn = NodeRegistry::GetInstance().Find(std::string("ND_luminance_color3"));
    if (!fn) {
        printf("    ND_luminance_color3 not registered\n");
        return false;
    }
    ParamMap in;
    in["in"] = Value(Vec3f(1.0f, 1.0f, 1.0f));
    NodeOutputMap out;
    ShadingContext ctx;
    fn(in, ctx, &out);
    float result = _GetFloat(out);
    // Rec.709: 0.2126 + 0.7152 + 0.0722 = 1.0
    if (!Test_IsClose(result, 1.0f, 0.01f)) {
        printf("    luminance of white: %f (expected ~1.0)\n", result);
        return false;
    }

    // Non-trivial color
    in["in"] = Value(Vec3f(0.5f, 0.0f, 0.0f));
    fn(in, ctx, &out);
    result = _GetFloat(out);
    float expected = 0.2126f * 0.5f;
    if (!Test_IsClose(result, expected, 0.001f)) {
        printf("    luminance of (0.5,0,0): %f (expected %f)\n",
               result, expected);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Param map tests
// ---------------------------------------------------------------------------

static bool TestParamMapCopyOnWriteForBorrowedValue() {
    ParamMap params;
    Value borrowed(1.0f);
    params.Add(AsSlotId("in"), &borrowed);

    params["in"] = Value(2.0f);

    if (!ValueHolds<float>(borrowed) ||
        !Test_IsClose(ValueGet<float>(borrowed), 1.0f)) {
        printf("    borrowed value was mutated\n");
        return false;
    }

    const Value* value = params.Find("in");
    if (!value || !ValueHolds<float>(*value)) {
        printf("    params['in'] missing after overwrite\n");
        return false;
    }

    return Test_IsClose(ValueGet<float>(*value), 2.0f);
}

// ---------------------------------------------------------------------------
// Registry tests
// ---------------------------------------------------------------------------

static bool TestNodeRegistryLookup() {
    NodeRegistry::RegisterBuiltinNodes();
    auto fn = NodeRegistry::GetInstance().Find(std::string("ND_add_float"));
    return fn != nullptr;
}

static bool TestNodeRegistryMissing() {
    NodeRegistry::RegisterBuiltinNodes();
    auto fn = NodeRegistry::GetInstance().Find(
        std::string("ND_nonexistent_node_xyz"));
    return fn == nullptr;
}

// ---------------------------------------------------------------------------

void
Test_RegisterNodeTests()
{
    _REG(TestAddFloat);
    _REG(TestAddColor3);
    _REG(TestMultiplyFloat);
    _REG(TestSubtractFloat);
    _REG(TestDivideFloat);
    _REG(TestClamp);
    _REG(TestMix);
    _REG(TestSmoothstep);
    _REG(TestRemap);
    _REG(TestCombineSeparateRoundtrip);
    _REG(TestConvertColor4ToColor3);
    _REG(TestIfgreater);
    // Conditional (expanded)
    _REG(TestIfgreaterInteger);
    _REG(TestIfgreaterVector2);
    _REG(TestIfgreaterBoolOutput);
    _REG(TestIfgreaterIntComparison);
    _REG(TestIfEqualBoolComparison);
    _REG(TestSwitchFloat);
    _REG(TestSwitchIntegerWhich);
    _REG(TestLogicalOps);

    // Compositing
    _REG(TestCompositingPlus);
    _REG(TestCompositingMinus);
    _REG(TestCompositingBurn);
    _REG(TestCompositingDodge);
    _REG(TestCompositingScreen);
    _REG(TestCompositingOverlay);
    _REG(TestCompositingMixParam);
    _REG(TestPorterDuffOver);
    _REG(TestPorterDuffIn);
    _REG(TestPorterDuffDisjointover);
    _REG(TestPorterDuffMatte);
    _REG(TestPorterDuffOut);
    _REG(TestPremultUnpremult);
    _REG(TestInsideOutside);
    _REG(TestMixVecVariant);
    _REG(TestProceduralWorleyNoise2d);
    _REG(TestProceduralWorleyNoise3d);
    _REG(TestProceduralFractal2dSingleOctave);
    _REG(TestProceduralUnifiedNoise2dCellRemap);
    _REG(TestProceduralUnifiedNoise3dCellRemap);
    _REG(TestProceduralRampLrAndSplitLr);
    _REG(TestProceduralRamp4AndRamp);
    _REG(TestProceduralPatterns);
    _REG(TestProceduralGridAndCrosshatch);
    _REG(TestProceduralRandomNodes);
    _REG(TestProceduralFlake2dCoverageZero);
    _REG(TestProceduralFlake3dCoverageZero);
    _REG(TestGeometricPosition);
    _REG(TestGeometricPositionWorldSpace);
    _REG(TestGeometricNormal);
    _REG(TestApplicationFrame);
    _REG(TestApplicationTime);
    _REG(TestTransformPointObjectToWorld);
    _REG(TestHeightToNormalDefaultTexcoord);
    _REG(TestBumpDefaultBasis);
    _REG(TestLuminance);
    _REG(TestParamMapCopyOnWriteForBorrowedValue);
    _REG(TestNodeRegistryLookup);
    _REG(TestNodeRegistryMissing);

    // Geompropvalue
    _REG(TestGeomPropValueFloat);
    _REG(TestGeomPropValueColor3);
    _REG(TestGeomPropValueDefault);
    _REG(TestGeomPropValueTypeMismatch);
    _REG(TestGeomPropValueEmptyName);
    _REG(TestGeomPropValueNotFound);
    _REG(TestGeomPropValueUniformString);
    _REG(TestGeomPropValueUniformDefault);
}

#undef _REG
