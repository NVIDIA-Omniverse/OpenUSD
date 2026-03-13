//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/pxr.h"
#include "pxr/imaging/plugin/hdEmbree/MaterialXCpp/graph.h"

#include "pxr/imaging/hd/material.h"
#include "pxr/usd/sdf/path.h"
#include "pxr/base/tf/token.h"
#include "pxr/base/vt/value.h"

#include <cstdio>
#include <functional>

PXR_NAMESPACE_USING_DIRECTIVE

void Test_Register(const char* name, std::function<bool()> fn);
bool Test_IsClose(float a, float b, float eps = 1e-5f);
bool Test_IsClose(const GfVec3f& a, const GfVec3f& b, float eps = 1e-5f);

#define _REG(name) Test_Register("Graph." #name, &name)

// ---------------------------------------------------------------------------

static bool
TestCompileEmptyNetwork()
{
    HdMaterialNetwork2 network;
    auto graph = EvalGraph::Compile(network);
    // Empty network should produce an invalid graph.
    if (!graph) return false;
    return !graph->IsValid();
}

static bool
TestCompileSingleTerminal()
{
    HdMaterialNetwork2 network;

    SdfPath termPath("/Material/UsdPreviewSurface");
    HdMaterialNode2 termNode;
    termNode.nodeTypeId = TfToken("UsdPreviewSurface");
    termNode.parameters[TfToken("diffuseColor")] =
        VtValue(GfVec3f(1, 0, 0));
    termNode.parameters[TfToken("roughness")] = VtValue(0.3f);
    network.nodes[termPath] = termNode;

    HdMaterialConnection2 termConn;
    termConn.upstreamNode = termPath;
    termConn.upstreamOutputName = TfToken("out");
    network.terminals[TfToken("surface")] = termConn;

    auto graph = EvalGraph::Compile(network);
    if (!graph || !graph->IsValid()) {
        printf("    Graph compilation failed\n");
        return false;
    }

    ShadingContext ctx;
    SurfaceClosure closure = graph->Evaluate(ctx);

    if (!Test_IsClose(closure.baseColor, GfVec3f(1, 0, 0), 1e-4f)) {
        printf("    baseColor: (%f,%f,%f)\n",
               closure.baseColor[0], closure.baseColor[1], closure.baseColor[2]);
        return false;
    }
    return Test_IsClose(closure.roughness, 0.3f);
}

static bool
TestCompileLinearChain()
{
    // constant(2.0) → multiply(*, 3.0) → terminal(roughness)
    HdMaterialNetwork2 network;

    // Node A: constant float producing 2.0
    SdfPath pathA("/Material/Constant");
    HdMaterialNode2 nodeA;
    nodeA.nodeTypeId = TfToken("ND_constant_float");
    nodeA.parameters[TfToken("value")] = VtValue(2.0f);
    network.nodes[pathA] = nodeA;

    // Terminal: UsdPreviewSurface (roughness connected to A)
    SdfPath termPath("/Material/Surface");
    HdMaterialNode2 termNode;
    termNode.nodeTypeId = TfToken("UsdPreviewSurface");
    termNode.parameters[TfToken("diffuseColor")] =
        VtValue(GfVec3f(0.5f));

    HdMaterialConnection2 connToA;
    connToA.upstreamNode = pathA;
    connToA.upstreamOutputName = TfToken("out");
    termNode.inputConnections[TfToken("roughness")]
        .push_back(connToA);

    network.nodes[termPath] = termNode;

    HdMaterialConnection2 termConn;
    termConn.upstreamNode = termPath;
    termConn.upstreamOutputName = TfToken("out");
    network.terminals[TfToken("surface")] = termConn;

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
    HdMaterialNetwork2 network;

    SdfPath pathA("/Material/A");
    HdMaterialNode2 nodeA;
    nodeA.nodeTypeId = TfToken("ND_constant_float");
    nodeA.parameters[TfToken("value")] = VtValue(2.0f);
    network.nodes[pathA] = nodeA;

    SdfPath pathB("/Material/B");
    HdMaterialNode2 nodeB;
    nodeB.nodeTypeId = TfToken("ND_constant_float");
    nodeB.parameters[TfToken("value")] = VtValue(3.0f);
    network.nodes[pathB] = nodeB;

    SdfPath pathC("/Material/C");
    HdMaterialNode2 nodeC;
    nodeC.nodeTypeId = TfToken("ND_add_float");
    HdMaterialConnection2 connA;
    connA.upstreamNode = pathA;
    connA.upstreamOutputName = TfToken("out");
    HdMaterialConnection2 connB;
    connB.upstreamNode = pathB;
    connB.upstreamOutputName = TfToken("out");
    nodeC.inputConnections[TfToken("in1")].push_back(connA);
    nodeC.inputConnections[TfToken("in2")].push_back(connB);
    network.nodes[pathC] = nodeC;

    SdfPath termPath("/Material/Surface");
    HdMaterialNode2 termNode;
    termNode.nodeTypeId = TfToken("UsdPreviewSurface");
    HdMaterialConnection2 connC;
    connC.upstreamNode = pathC;
    connC.upstreamOutputName = TfToken("out");
    termNode.inputConnections[TfToken("roughness")].push_back(connC);
    network.nodes[termPath] = termNode;

    HdMaterialConnection2 termConn;
    termConn.upstreamNode = termPath;
    termConn.upstreamOutputName = TfToken("out");
    network.terminals[TfToken("surface")] = termConn;

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
    HdMaterialNetwork2 network;

    SdfPath termPath("/Material/Surface");
    HdMaterialNode2 termNode;
    termNode.nodeTypeId = TfToken("ND_standard_surface_surfaceshader");
    termNode.parameters[TfToken("base")] = VtValue(1.0f);
    termNode.parameters[TfToken("base_color")] = VtValue(GfVec3f(0, 1, 0));
    termNode.parameters[TfToken("specular_roughness")] = VtValue(0.4f);
    termNode.parameters[TfToken("metalness")] = VtValue(0.5f);
    network.nodes[termPath] = termNode;

    HdMaterialConnection2 termConn;
    termConn.upstreamNode = termPath;
    termConn.upstreamOutputName = TfToken("out");
    network.terminals[TfToken("surface")] = termConn;

    auto graph = EvalGraph::Compile(network);
    if (!graph || !graph->IsValid()) return false;

    ShadingContext ctx;
    SurfaceClosure closure = graph->Evaluate(ctx);

    if (!Test_IsClose(closure.baseColor, GfVec3f(0, 1, 0), 1e-4f)) {
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
    HdMaterialNetwork2 network;

    SdfPath pathMul("/Material/Mul");
    HdMaterialNode2 nodeMul;
    nodeMul.nodeTypeId = TfToken("ND_multiply_float");
    nodeMul.parameters[TfToken("in1")] = VtValue(2.0f);
    nodeMul.parameters[TfToken("in2")] = VtValue(3.0f);
    network.nodes[pathMul] = nodeMul;

    SdfPath pathCombine("/Material/Combine");
    HdMaterialNode2 nodeCombine;
    nodeCombine.nodeTypeId = TfToken("ND_combine3_color3");
    HdMaterialConnection2 mulConn;
    mulConn.upstreamNode = pathMul;
    mulConn.upstreamOutputName = TfToken("out");
    nodeCombine.inputConnections[TfToken("in1")].push_back(mulConn);
    nodeCombine.inputConnections[TfToken("in2")].push_back(mulConn);
    nodeCombine.inputConnections[TfToken("in3")].push_back(mulConn);
    network.nodes[pathCombine] = nodeCombine;

    SdfPath termPath("/Material/Surface");
    HdMaterialNode2 termNode;
    termNode.nodeTypeId = TfToken("UsdPreviewSurface");
    HdMaterialConnection2 combConn;
    combConn.upstreamNode = pathCombine;
    combConn.upstreamOutputName = TfToken("out");
    termNode.inputConnections[TfToken("diffuseColor")].push_back(combConn);
    network.nodes[termPath] = termNode;

    HdMaterialConnection2 termConn;
    termConn.upstreamNode = termPath;
    termConn.upstreamOutputName = TfToken("out");
    network.terminals[TfToken("surface")] = termConn;

    auto graph = EvalGraph::Compile(network);
    if (!graph || !graph->IsValid()) return false;

    ShadingContext ctx;
    SurfaceClosure closure = graph->Evaluate(ctx);

    if (!Test_IsClose(closure.baseColor, GfVec3f(6, 6, 6), 1e-4f)) {
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
    HdMaterialNetwork2 network;

    SdfPath pathGeom("/Material/Geomcolor");
    HdMaterialNode2 nodeGeom;
    nodeGeom.nodeTypeId = TfToken("ND_geomcolor_color3");
    network.nodes[pathGeom] = nodeGeom;

    SdfPath termPath("/Material/Surface");
    HdMaterialNode2 termNode;
    termNode.nodeTypeId = TfToken("UsdPreviewSurface");
    HdMaterialConnection2 geomConn;
    geomConn.upstreamNode = pathGeom;
    geomConn.upstreamOutputName = TfToken("out");
    termNode.inputConnections[TfToken("diffuseColor")].push_back(geomConn);
    network.nodes[termPath] = termNode;

    HdMaterialConnection2 termConn;
    termConn.upstreamNode = termPath;
    termConn.upstreamOutputName = TfToken("out");
    network.terminals[TfToken("surface")] = termConn;

    auto graph = EvalGraph::Compile(network);
    if (!graph || !graph->IsValid()) return false;

    ShadingContext ctx;
    ctx.displayColor = GfVec3f(0.3f, 0.6f, 0.9f);
    SurfaceClosure closure = graph->Evaluate(ctx);

    // UsdPreviewSurface multiplies baseColor by occlusion (default 1.0),
    // so it should match displayColor.
    if (!Test_IsClose(closure.baseColor, GfVec3f(0.3f, 0.6f, 0.9f), 1e-4f)) {
        printf("    baseColor: (%f,%f,%f)\n",
               closure.baseColor[0], closure.baseColor[1], closure.baseColor[2]);
        return false;
    }
    return true;
}

static bool
TestInvalidGraphEvaluate()
{
    HdMaterialNetwork2 network;
    auto graph = EvalGraph::Compile(network);
    if (!graph) return false;

    ShadingContext ctx;
    SurfaceClosure closure = graph->Evaluate(ctx);
    // Should return a default closure without crashing.
    return Test_IsClose(closure.baseColor, GfVec3f(0.8f), 1e-4f);
}

// ---------------------------------------------------------------------------

void
Test_RegisterGraphTests()
{
    _REG(TestCompileEmptyNetwork);
    _REG(TestCompileSingleTerminal);
    _REG(TestCompileLinearChain);
    _REG(TestCompileDiamondDAG);
    _REG(TestEvalWithConstantInputs);
    _REG(TestEvalMultiplyChain);
    _REG(TestEvalGeometricInput);
    _REG(TestInvalidGraphEvaluate);
}

#undef _REG
