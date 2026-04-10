//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "../graph.h"
#include "../nodeRegistry.h"

#include <cstdio>
#include <functional>

using namespace mxcpp;

void Test_Register(const char* name, std::function<bool()> fn);
bool Test_IsClose(float a, float b, float eps = 1e-5f);
bool Test_IsClose(const Vec3f& a, const Vec3f& b, float eps = 1e-5f);

#define _REG(name) Test_Register("Graph." #name, &name)

// ---------------------------------------------------------------------------

static const SlotName _kIn("in");
static const SlotName _kOut("out");

static void
_EvalOffsetReevaluate(const ParamMap& inputs,
                      const ShadingContext& ctx,
                      NodeOutputMap* outputs)
{
    Value value;
    ShadingContext shiftedCtx = ctx;
    shiftedCtx.texcoord[0] += 0.5f;

    if (inputs.Evaluate(_kIn, shiftedCtx, &value) &&
        ValueHolds<float>(value)) {
        (*outputs)[_kOut] = value;
        return;
    }

    (*outputs)[_kOut] = Value(-1.0f);
}

static bool
TestCompileEmptyNetwork()
{
    MaterialGraph network;
    auto graph = EvalGraph::Compile(network);
    // Empty network should produce an invalid graph.
    if (!graph) return false;
    return !graph->IsValid();
}

static bool
TestCompileSingleTerminal()
{
    MaterialGraph network;

    std::string termPath = "/Material/UsdPreviewSurface";
    GraphNode termNode;
    termNode.nodeTypeId = "UsdPreviewSurface";
    termNode.parameters["diffuseColor"] =
        Value(Vec3f(1, 0, 0));
    termNode.parameters["roughness"] = Value(0.3f);
    network.nodes[termPath] = termNode;

    GraphConnection termConn;
    termConn.upstreamNode = termPath;
    termConn.upstreamOutputName = "out";
    network.terminals["surface"] = termConn;

    auto graph = EvalGraph::Compile(network);
    if (!graph || !graph->IsValid()) {
        printf("    Graph compilation failed\n");
        return false;
    }

    ShadingContext ctx;
    SurfaceClosure closure = graph->Evaluate(ctx);

    if (!Test_IsClose(closure.baseColor, Vec3f(1, 0, 0), 1e-4f)) {
        printf("    baseColor: (%f,%f,%f)\n",
               closure.baseColor[0], closure.baseColor[1], closure.baseColor[2]);
        return false;
    }
    return Test_IsClose(closure.roughness, 0.3f) &&
           closure.HasBsdfTree();
}

static bool
TestCompileMaterialXUsdPreviewSurfaceTerminal()
{
    MaterialGraph network;

    const std::string termPath = "/Material/UsdPreviewSurfaceMtlx";
    GraphNode termNode;
    termNode.nodeTypeId = "ND_UsdPreviewSurface_surfaceshader";
    termNode.parameters["diffuseColor"] = Value(Vec3f(0.25f, 0.5f, 0.75f));
    termNode.parameters["opacity"] = Value(0.0f);
    termNode.parameters["opacityMode"] = Value(1);
    network.nodes[termPath] = termNode;

    GraphConnection termConn;
    termConn.upstreamNode = termPath;
    termConn.upstreamOutputName = "out";
    network.terminals["surface"] = termConn;

    auto graph = EvalGraph::Compile(network);
    if (!graph || !graph->IsValid()) {
        printf("    MaterialX UsdPreviewSurface compilation failed\n");
        return false;
    }

    const ShadingContext ctx;
    const SurfaceClosure closure = graph->Evaluate(ctx);
    return Test_IsClose(closure.baseColor, Vec3f(0.25f, 0.5f, 0.75f), 1e-4f) &&
           closure.HasBsdfTree() &&
           Test_IsClose(closure.opacity, 0.0f) &&
           Test_IsClose(closure.presence, 0.0f) &&
           Test_IsClose(closure.transmission, 0.0f);
}

static bool
TestCompileDisneyPrincipledTerminal()
{
    MaterialGraph network;

    const std::string termPath = "/Material/Disney";
    GraphNode termNode;
    termNode.nodeTypeId = "ND_disney_principled";
    termNode.parameters["baseColor"] = Value(Vec3f(0.2f, 0.4f, 0.6f));
    termNode.parameters["metallic"] = Value(0.25f);
    network.nodes[termPath] = termNode;

    GraphConnection termConn;
    termConn.upstreamNode = termPath;
    termConn.upstreamOutputName = "out";
    network.terminals["surface"] = termConn;

    auto graph = EvalGraph::Compile(network);
    if (!graph || !graph->IsValid()) {
        return false;
    }

    const SurfaceClosure closure = graph->Evaluate(ShadingContext{});
    return Test_IsClose(closure.baseColor, Vec3f(0.2f, 0.4f, 0.6f), 1e-4f) &&
           Test_IsClose(closure.metallic, 0.25f) &&
           closure.HasBsdfTree();
}

static bool
TestCompileGltfPbrTerminal()
{
    MaterialGraph network;

    const std::string termPath = "/Material/Gltf";
    GraphNode termNode;
    termNode.nodeTypeId = "ND_gltf_pbr_surfaceshader";
    termNode.parameters["base_color"] = Value(Vec3f(0.7f, 0.6f, 0.5f));
    termNode.parameters["alpha"] = Value(0.0f);
    termNode.parameters["alpha_mode"] = Value(1);
    network.nodes[termPath] = termNode;

    GraphConnection termConn;
    termConn.upstreamNode = termPath;
    termConn.upstreamOutputName = "out";
    network.terminals["surface"] = termConn;

    auto graph = EvalGraph::Compile(network);
    if (!graph || !graph->IsValid()) {
        return false;
    }

    const SurfaceClosure closure = graph->Evaluate(ShadingContext{});
    return Test_IsClose(closure.baseColor, Vec3f(0.7f, 0.6f, 0.5f), 1e-4f) &&
           Test_IsClose(closure.opacity, 0.0f) &&
           closure.HasBsdfTree();
}

static bool
TestCompileLinearChain()
{
    // constant(2.0) → multiply(*, 3.0) → terminal(roughness)
    MaterialGraph network;

    // Node A: constant float producing 2.0
    std::string pathA = "/Material/Constant";
    GraphNode nodeA;
    nodeA.nodeTypeId = "ND_constant_float";
    nodeA.parameters["value"] = Value(2.0f);
    network.nodes[pathA] = nodeA;

    // Terminal: UsdPreviewSurface (roughness connected to A)
    std::string termPath = "/Material/Surface";
    GraphNode termNode;
    termNode.nodeTypeId = "UsdPreviewSurface";
    termNode.parameters["diffuseColor"] =
        Value(Vec3f(0.5f));

    GraphConnection connToA;
    connToA.upstreamNode = pathA;
    connToA.upstreamOutputName = "out";
    termNode.inputConnections["roughness"]
        .push_back(connToA);

    network.nodes[termPath] = termNode;

    GraphConnection termConn;
    termConn.upstreamNode = termPath;
    termConn.upstreamOutputName = "out";
    network.terminals["surface"] = termConn;

    auto graph = EvalGraph::Compile(network);
    if (!graph || !graph->IsValid()) return false;

    ShadingContext ctx;
    SurfaceClosure closure = graph->Evaluate(ctx);

    // roughness should come from constant node: 2.0 (clamped by usage).
    if (!Test_IsClose(closure.roughness, 2.0f)) {
        printf("    roughness: %f (expected 2.0)\n", closure.roughness);
        return false;
    }
    return true;
}

static bool
TestCompileDiamondDAG()
{
    // A(const 2.0) ─┐
    //                ├─ C(add) → terminal
    // B(const 3.0) ─┘
    MaterialGraph network;

    std::string pathA = "/Material/A";
    GraphNode nodeA;
    nodeA.nodeTypeId = "ND_constant_float";
    nodeA.parameters["value"] = Value(2.0f);
    network.nodes[pathA] = nodeA;

    std::string pathB = "/Material/B";
    GraphNode nodeB;
    nodeB.nodeTypeId = "ND_constant_float";
    nodeB.parameters["value"] = Value(3.0f);
    network.nodes[pathB] = nodeB;

    std::string pathC = "/Material/C";
    GraphNode nodeC;
    nodeC.nodeTypeId = "ND_add_float";
    GraphConnection connA;
    connA.upstreamNode = pathA;
    connA.upstreamOutputName = "out";
    GraphConnection connB;
    connB.upstreamNode = pathB;
    connB.upstreamOutputName = "out";
    nodeC.inputConnections["in1"].push_back(connA);
    nodeC.inputConnections["in2"].push_back(connB);
    network.nodes[pathC] = nodeC;

    std::string termPath = "/Material/Surface";
    GraphNode termNode;
    termNode.nodeTypeId = "UsdPreviewSurface";
    GraphConnection connC;
    connC.upstreamNode = pathC;
    connC.upstreamOutputName = "out";
    termNode.inputConnections["roughness"].push_back(connC);
    network.nodes[termPath] = termNode;

    GraphConnection termConn;
    termConn.upstreamNode = termPath;
    termConn.upstreamOutputName = "out";
    network.terminals["surface"] = termConn;

    auto graph = EvalGraph::Compile(network);
    if (!graph || !graph->IsValid()) return false;

    ShadingContext ctx;
    SurfaceClosure closure = graph->Evaluate(ctx);

    // 2.0 + 3.0 = 5.0
    if (!Test_IsClose(closure.roughness, 5.0f)) {
        printf("    roughness: %f (expected 5.0)\n", closure.roughness);
        return false;
    }
    return true;
}

static bool
TestEvalWithConstantInputs()
{
    MaterialGraph network;

    std::string termPath = "/Material/Surface";
    GraphNode termNode;
    termNode.nodeTypeId = "ND_standard_surface_surfaceshader";
    termNode.parameters["base"] = Value(1.0f);
    termNode.parameters["base_color"] = Value(Vec3f(0, 1, 0));
    termNode.parameters["specular_roughness"] = Value(0.4f);
    termNode.parameters["metalness"] = Value(0.5f);
    network.nodes[termPath] = termNode;

    GraphConnection termConn;
    termConn.upstreamNode = termPath;
    termConn.upstreamOutputName = "out";
    network.terminals["surface"] = termConn;

    auto graph = EvalGraph::Compile(network);
    if (!graph || !graph->IsValid()) return false;

    ShadingContext ctx;
    SurfaceClosure closure = graph->Evaluate(ctx);

    if (!Test_IsClose(closure.baseColor, Vec3f(0, 1, 0), 1e-4f)) {
        printf("    baseColor: (%f,%f,%f)\n",
               closure.baseColor[0], closure.baseColor[1], closure.baseColor[2]);
        return false;
    }
    if (!Test_IsClose(closure.roughness, 0.4f)) return false;
    if (!Test_IsClose(closure.metallic, 0.5f)) return false;
    return true;
}

static bool
TestEvalMultiplyChain()
{
    // multiply(2.0, 3.0) → combine3(6.0, 6.0, 6.0) → terminal(diffuseColor)
    MaterialGraph network;

    std::string pathMul = "/Material/Mul";
    GraphNode nodeMul;
    nodeMul.nodeTypeId = "ND_multiply_float";
    nodeMul.parameters["in1"] = Value(2.0f);
    nodeMul.parameters["in2"] = Value(3.0f);
    network.nodes[pathMul] = nodeMul;

    std::string pathCombine = "/Material/Combine";
    GraphNode nodeCombine;
    nodeCombine.nodeTypeId = "ND_combine3_color3";
    GraphConnection mulConn;
    mulConn.upstreamNode = pathMul;
    mulConn.upstreamOutputName = "out";
    nodeCombine.inputConnections["in1"].push_back(mulConn);
    nodeCombine.inputConnections["in2"].push_back(mulConn);
    nodeCombine.inputConnections["in3"].push_back(mulConn);
    network.nodes[pathCombine] = nodeCombine;

    std::string termPath = "/Material/Surface";
    GraphNode termNode;
    termNode.nodeTypeId = "UsdPreviewSurface";
    GraphConnection combConn;
    combConn.upstreamNode = pathCombine;
    combConn.upstreamOutputName = "out";
    termNode.inputConnections["diffuseColor"].push_back(combConn);
    network.nodes[termPath] = termNode;

    GraphConnection termConn;
    termConn.upstreamNode = termPath;
    termConn.upstreamOutputName = "out";
    network.terminals["surface"] = termConn;

    auto graph = EvalGraph::Compile(network);
    if (!graph || !graph->IsValid()) return false;

    ShadingContext ctx;
    SurfaceClosure closure = graph->Evaluate(ctx);

    if (!Test_IsClose(closure.baseColor, Vec3f(6, 6, 6), 1e-4f)) {
        printf("    baseColor: (%f,%f,%f)\n",
               closure.baseColor[0], closure.baseColor[1], closure.baseColor[2]);
        return false;
    }
    return true;
}

static bool
TestEvalGeometricInput()
{
    // normal_node → combine3(Nx, Ny, Nz) is too complex;
    // test that geometric nodes pass through to the terminal.
    // geomcolor → terminal(diffuseColor)
    MaterialGraph network;

    std::string pathGeom = "/Material/Geomcolor";
    GraphNode nodeGeom;
    nodeGeom.nodeTypeId = "ND_geomcolor_color3";
    network.nodes[pathGeom] = nodeGeom;

    std::string termPath = "/Material/Surface";
    GraphNode termNode;
    termNode.nodeTypeId = "UsdPreviewSurface";
    GraphConnection geomConn;
    geomConn.upstreamNode = pathGeom;
    geomConn.upstreamOutputName = "out";
    termNode.inputConnections["diffuseColor"].push_back(geomConn);
    network.nodes[termPath] = termNode;

    GraphConnection termConn;
    termConn.upstreamNode = termPath;
    termConn.upstreamOutputName = "out";
    network.terminals["surface"] = termConn;

    auto graph = EvalGraph::Compile(network);
    if (!graph || !graph->IsValid()) return false;

    ShadingContext ctx;
    ctx.displayColor = Vec3f(0.3f, 0.6f, 0.9f);
    SurfaceClosure closure = graph->Evaluate(ctx);

    // UsdPreviewSurface leaves diffuseColor unchanged here because the
    // occlusion input is ignored and defaults do not modify displayColor.
    if (!Test_IsClose(closure.baseColor, Vec3f(0.3f, 0.6f, 0.9f), 1e-4f)) {
        printf("    baseColor: (%f,%f,%f)\n",
               closure.baseColor[0], closure.baseColor[1], closure.baseColor[2]);
        return false;
    }
    return true;
}

static bool
TestCompileUnknownNodeTypeFails()
{
    MaterialGraph network;

    std::string pathUnknown = "/Material/Unknown";
    GraphNode unknownNode;
    unknownNode.nodeTypeId = "ND_totally_unknown_float";
    unknownNode.parameters["value"] = Value(1.0f);
    network.nodes[pathUnknown] = unknownNode;

    std::string termPath = "/Material/Surface";
    GraphNode termNode;
    termNode.nodeTypeId = "UsdPreviewSurface";
    GraphConnection unknownConn;
    unknownConn.upstreamNode = pathUnknown;
    unknownConn.upstreamOutputName = "out";
    termNode.inputConnections["roughness"].push_back(unknownConn);
    network.nodes[termPath] = termNode;

    GraphConnection termConn;
    termConn.upstreamNode = termPath;
    termConn.upstreamOutputName = "out";
    network.terminals["surface"] = termConn;

    auto graph = EvalGraph::Compile(network);
    if (!graph) return false;
    return !graph->IsValid();
}

static bool
TestCompileRejectsCycle()
{
    MaterialGraph network;

    std::string pathA = "/Material/A";
    GraphNode nodeA;
    nodeA.nodeTypeId = "ND_add_float";

    std::string pathB = "/Material/B";
    GraphNode nodeB;
    nodeB.nodeTypeId = "ND_add_float";
    nodeB.parameters["in2"] = Value(1.0f);

    GraphConnection connA;
    connA.upstreamNode = pathA;
    connA.upstreamOutputName = "out";
    GraphConnection connB;
    connB.upstreamNode = pathB;
    connB.upstreamOutputName = "out";

    nodeA.inputConnections["in1"].push_back(connB);
    nodeA.parameters["in2"] = Value(2.0f);
    nodeB.inputConnections["in1"].push_back(connA);

    network.nodes[pathA] = nodeA;
    network.nodes[pathB] = nodeB;

    std::string termPath = "/Material/Surface";
    GraphNode termNode;
    termNode.nodeTypeId = "UsdPreviewSurface";
    termNode.inputConnections["roughness"].push_back(connA);
    network.nodes[termPath] = termNode;

    GraphConnection termConn;
    termConn.upstreamNode = termPath;
    termConn.upstreamOutputName = "out";
    network.terminals["surface"] = termConn;

    auto graph = EvalGraph::Compile(network);
    if (!graph) return false;
    return !graph->IsValid();
}

static bool
TestInvalidGraphEvaluate()
{
    MaterialGraph network;
    auto graph = EvalGraph::Compile(network);
    if (!graph) return false;

    ShadingContext ctx;
    SurfaceClosure closure = graph->Evaluate(ctx);
    // Should return a default closure without crashing.
    return Test_IsClose(closure.baseColor, Vec3f(0.8f), 1e-4f);
}

static bool
TestInputReevaluationUsesModifiedContext()
{
    NodeRegistry::RegisterBuiltinNodes();
    NodeRegistry::GetInstance().Register(
        "ND_test_offset_reevaluate_float", &_EvalOffsetReevaluate);

    MaterialGraph network;

    std::string texcoordPath = "/Material/Texcoord";
    GraphNode texcoordNode;
    texcoordNode.nodeTypeId = "ND_texcoord_vector2";
    network.nodes[texcoordPath] = texcoordNode;

    std::string extractPath = "/Material/ExtractU";
    GraphNode extractNode;
    extractNode.nodeTypeId = "ND_extract_vector2";
    extractNode.parameters["index"] = Value(0);
    GraphConnection texcoordConn;
    texcoordConn.upstreamNode = texcoordPath;
    texcoordConn.upstreamOutputName = "out";
    extractNode.inputConnections["in"].push_back(texcoordConn);
    network.nodes[extractPath] = extractNode;

    std::string reevalPath = "/Material/Reevaluate";
    GraphNode reevalNode;
    reevalNode.nodeTypeId = "ND_test_offset_reevaluate_float";
    GraphConnection extractConn;
    extractConn.upstreamNode = extractPath;
    extractConn.upstreamOutputName = "out";
    reevalNode.inputConnections["in"].push_back(extractConn);
    network.nodes[reevalPath] = reevalNode;

    std::string termPath = "/Material/Surface";
    GraphNode termNode;
    termNode.nodeTypeId = "UsdPreviewSurface";
    GraphConnection reevalConn;
    reevalConn.upstreamNode = reevalPath;
    reevalConn.upstreamOutputName = "out";
    termNode.inputConnections["roughness"].push_back(reevalConn);
    network.nodes[termPath] = termNode;

    GraphConnection termConn;
    termConn.upstreamNode = termPath;
    termConn.upstreamOutputName = "out";
    network.terminals["surface"] = termConn;

    auto graph = EvalGraph::Compile(network);
    if (!graph || !graph->IsValid()) {
        printf("    Graph compilation failed\n");
        return false;
    }

    ShadingContext ctx;
    ctx.texcoord = Vec2f(0.25f, 0.75f);
    SurfaceClosure closure = graph->Evaluate(ctx);
    if (!Test_IsClose(closure.roughness, 0.75f)) {
        printf("    roughness: %f (expected 0.75)\n", closure.roughness);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------

void
Test_RegisterGraphTests()
{
    _REG(TestCompileEmptyNetwork);
    _REG(TestCompileSingleTerminal);
    _REG(TestCompileMaterialXUsdPreviewSurfaceTerminal);
    _REG(TestCompileDisneyPrincipledTerminal);
    _REG(TestCompileGltfPbrTerminal);
    _REG(TestCompileLinearChain);
    _REG(TestCompileDiamondDAG);
    _REG(TestEvalWithConstantInputs);
    _REG(TestEvalMultiplyChain);
    _REG(TestEvalGeometricInput);
    _REG(TestCompileUnknownNodeTypeFails);
    _REG(TestCompileRejectsCycle);
    _REG(TestInvalidGraphEvaluate);
    _REG(TestInputReevaluationUsesModifiedContext);
}

#undef _REG
