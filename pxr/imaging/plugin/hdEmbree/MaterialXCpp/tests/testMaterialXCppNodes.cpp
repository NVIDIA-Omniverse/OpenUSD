//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/pxr.h"
#include "pxr/imaging/plugin/hdEmbree/MaterialXCpp/nodeRegistry.h"
#include "pxr/imaging/plugin/hdEmbree/MaterialXCpp/types.h"

#include <cmath>
#include <cstdio>
#include <functional>

PXR_NAMESPACE_USING_DIRECTIVE

void Test_Register(const char* name, std::function<bool()> fn);
bool Test_IsClose(float a, float b, float eps = 1e-5f);
bool Test_IsClose(const GfVec3f& a, const GfVec3f& b, float eps = 1e-5f);

#define _REG(name) Test_Register("Nodes." #name, &name)

// Helper: evaluate a registered node with given inputs.
static NodeOutputMap
_Eval(const char* nodeTypeId, const ParamMap& inputs)
{
    NodeRegistry::RegisterBuiltinNodes();
    auto fn = NodeRegistry::GetInstance().Find(TfToken(nodeTypeId));
    NodeOutputMap outputs;
    ShadingContext ctx;
    if (fn) {
        fn(inputs, ctx, &outputs);
    }
    return outputs;
}

static float _GetFloat(const NodeOutputMap& o, const char* name = "out") {
    auto it = o.find(TfToken(name));
    if (it != o.end() && it->second.IsHolding<float>())
        return it->second.UncheckedGet<float>();
    return -9999.0f;
}

static GfVec3f _GetVec3(const NodeOutputMap& o, const char* name = "out") {
    auto it = o.find(TfToken(name));
    if (it != o.end() && it->second.IsHolding<GfVec3f>())
        return it->second.UncheckedGet<GfVec3f>();
    return GfVec3f(-9999.0f);
}

static GfVec2f _GetVec2(const NodeOutputMap& o, const char* name = "out") {
    auto it = o.find(TfToken(name));
    if (it != o.end() && it->second.IsHolding<GfVec2f>())
        return it->second.UncheckedGet<GfVec2f>();
    return GfVec2f(-9999.0f);
}

// ---------------------------------------------------------------------------
// Math node tests
// ---------------------------------------------------------------------------

static bool TestAddFloat() {
    ParamMap in;
    in[TfToken("in1")] = VtValue(2.0f);
    in[TfToken("in2")] = VtValue(3.0f);
    auto out = _Eval("ND_add_float", in);
    return Test_IsClose(_GetFloat(out), 5.0f);
}

static bool TestAddColor3() {
    ParamMap in;
    in[TfToken("in1")] = VtValue(GfVec3f(1, 2, 3));
    in[TfToken("in2")] = VtValue(GfVec3f(4, 5, 6));
    auto out = _Eval("ND_add_color3", in);
    return Test_IsClose(_GetVec3(out), GfVec3f(5, 7, 9));
}

static bool TestMultiplyFloat() {
    ParamMap in;
    in[TfToken("in1")] = VtValue(3.0f);
    in[TfToken("in2")] = VtValue(4.0f);
    auto out = _Eval("ND_multiply_float", in);
    return Test_IsClose(_GetFloat(out), 12.0f);
}

static bool TestSubtractFloat() {
    ParamMap in;
    in[TfToken("in1")] = VtValue(10.0f);
    in[TfToken("in2")] = VtValue(3.0f);
    auto out = _Eval("ND_subtract_float", in);
    return Test_IsClose(_GetFloat(out), 7.0f);
}

static bool TestDivideFloat() {
    ParamMap in;
    in[TfToken("in1")] = VtValue(10.0f);
    in[TfToken("in2")] = VtValue(4.0f);
    auto out = _Eval("ND_divide_float", in);
    if (!Test_IsClose(_GetFloat(out), 2.5f)) return false;

    // Division by zero should be safe.
    in[TfToken("in2")] = VtValue(0.0f);
    out = _Eval("ND_divide_float", in);
    return Test_IsClose(_GetFloat(out), 0.0f);
}

static bool TestClamp() {
    ParamMap in;
    in[TfToken("in")] = VtValue(1.5f);
    in[TfToken("low")] = VtValue(0.0f);
    in[TfToken("high")] = VtValue(1.0f);
    auto out = _Eval("ND_clamp_float", in);
    if (!Test_IsClose(_GetFloat(out), 1.0f)) return false;

    in[TfToken("in")] = VtValue(-0.5f);
    out = _Eval("ND_clamp_float", in);
    return Test_IsClose(_GetFloat(out), 0.0f);
}

static bool TestMix() {
    ParamMap in;
    in[TfToken("fg")] = VtValue(10.0f);
    in[TfToken("bg")] = VtValue(0.0f);

    in[TfToken("mix")] = VtValue(0.0f);
    auto out = _Eval("ND_mix_float", in);
    if (!Test_IsClose(_GetFloat(out), 0.0f)) return false;

    in[TfToken("mix")] = VtValue(1.0f);
    out = _Eval("ND_mix_float", in);
    if (!Test_IsClose(_GetFloat(out), 10.0f)) return false;

    in[TfToken("mix")] = VtValue(0.5f);
    out = _Eval("ND_mix_float", in);
    return Test_IsClose(_GetFloat(out), 5.0f);
}

static bool TestSmoothstep() {
    ParamMap in;
    in[TfToken("low")] = VtValue(0.0f);
    in[TfToken("high")] = VtValue(1.0f);

    in[TfToken("in")] = VtValue(0.0f);
    auto out = _Eval("ND_smoothstep_float", in);
    if (!Test_IsClose(_GetFloat(out), 0.0f)) return false;

    in[TfToken("in")] = VtValue(1.0f);
    out = _Eval("ND_smoothstep_float", in);
    if (!Test_IsClose(_GetFloat(out), 1.0f)) return false;

    in[TfToken("in")] = VtValue(0.5f);
    out = _Eval("ND_smoothstep_float", in);
    return Test_IsClose(_GetFloat(out), 0.5f);
}

static bool TestRemap() {
    ParamMap in;
    in[TfToken("in")] = VtValue(0.5f);
    in[TfToken("inlow")] = VtValue(0.0f);
    in[TfToken("inhigh")] = VtValue(1.0f);
    in[TfToken("outlow")] = VtValue(10.0f);
    in[TfToken("outhigh")] = VtValue(20.0f);
    auto out = _Eval("ND_remap_float", in);
    return Test_IsClose(_GetFloat(out), 15.0f);
}

// ---------------------------------------------------------------------------
// Channel node tests
// ---------------------------------------------------------------------------

static bool TestCombineSeparateRoundtrip() {
    ParamMap in;
    in[TfToken("in1")] = VtValue(0.2f);
    in[TfToken("in2")] = VtValue(0.4f);
    in[TfToken("in3")] = VtValue(0.6f);
    auto combOut = _Eval("ND_combine3_color3", in);
    GfVec3f combined = _GetVec3(combOut);

    ParamMap in2;
    in2[TfToken("in")] = VtValue(combined);
    auto sepOut = _Eval("ND_separate3_color3", in2);
    float x = _GetFloat(sepOut, "outx");
    float y = _GetFloat(sepOut, "outy");
    float z = _GetFloat(sepOut, "outz");
    return Test_IsClose(x, 0.2f) &&
           Test_IsClose(y, 0.4f) &&
           Test_IsClose(z, 0.6f);
}

// ---------------------------------------------------------------------------
// Conditional node tests
// ---------------------------------------------------------------------------

static bool TestIfgreater() {
    NodeRegistry::RegisterBuiltinNodes();
    auto fn = NodeRegistry::GetInstance().Find(TfToken("ND_ifgreater_float"));
    if (!fn) {
        printf("    ND_ifgreater_float not registered\n");
        return false;
    }
    // Just verify the node exists and is callable.
    ParamMap in;
    in[TfToken("value1")] = VtValue(2.0f);
    in[TfToken("value2")] = VtValue(1.0f);
    in[TfToken("in1")] = VtValue(10.0f);
    in[TfToken("in2")] = VtValue(20.0f);
    NodeOutputMap out;
    ShadingContext ctx;
    fn(in, ctx, &out);
    return Test_IsClose(_GetFloat(out), 10.0f);
}

// ---------------------------------------------------------------------------
// Geometric node tests
// ---------------------------------------------------------------------------

static bool TestGeometricPosition() {
    NodeRegistry::RegisterBuiltinNodes();
    auto fn = NodeRegistry::GetInstance().Find(TfToken("ND_position_vector3"));
    if (!fn) return false;

    ParamMap in;
    ShadingContext ctx;
    ctx.position = GfVec3f(1.0f, 2.0f, 3.0f);
    NodeOutputMap out;
    fn(in, ctx, &out);
    return Test_IsClose(_GetVec3(out), GfVec3f(1.0f, 2.0f, 3.0f));
}

static bool TestGeometricNormal() {
    NodeRegistry::RegisterBuiltinNodes();
    auto fn = NodeRegistry::GetInstance().Find(TfToken("ND_normal_vector3"));
    if (!fn) return false;

    ParamMap in;
    ShadingContext ctx;
    ctx.normal = GfVec3f(0.0f, 1.0f, 0.0f);
    NodeOutputMap out;
    fn(in, ctx, &out);
    return Test_IsClose(_GetVec3(out), GfVec3f(0.0f, 1.0f, 0.0f));
}

// ---------------------------------------------------------------------------
// Color node tests
// ---------------------------------------------------------------------------

static bool TestLuminance() {
    NodeRegistry::RegisterBuiltinNodes();
    auto fn = NodeRegistry::GetInstance().Find(TfToken("ND_luminance_color3"));
    if (!fn) {
        printf("    ND_luminance_color3 not registered\n");
        return false;
    }
    ParamMap in;
    in[TfToken("in")] = VtValue(GfVec3f(1.0f, 1.0f, 1.0f));
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
    in[TfToken("in")] = VtValue(GfVec3f(0.5f, 0.0f, 0.0f));
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
// Registry tests
// ---------------------------------------------------------------------------

static bool TestNodeRegistryLookup() {
    NodeRegistry::RegisterBuiltinNodes();
    auto fn = NodeRegistry::GetInstance().Find(TfToken("ND_add_float"));
    return fn != nullptr;
}

static bool TestNodeRegistryMissing() {
    NodeRegistry::RegisterBuiltinNodes();
    auto fn = NodeRegistry::GetInstance().Find(
        TfToken("ND_nonexistent_node_xyz"));
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
    _REG(TestIfgreater);
    _REG(TestGeometricPosition);
    _REG(TestGeometricNormal);
    _REG(TestLuminance);
    _REG(TestNodeRegistryLookup);
    _REG(TestNodeRegistryMissing);
}

#undef _REG
