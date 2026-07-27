//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "../graph.h"
#include "../nodeRegistry.h"
#include "../surfaceShaderUtils.h"

#include <cstdio>
#include <functional>
#include <variant>

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
    CompileResult compileResult = EvalGraph::Compile(network);
    return compileResult.status == CompileStatus::AbsentTerminal &&
           !compileResult.graph && compileResult.diagnostic.empty();
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

    CompileResult compileResult = EvalGraph::Compile(network);

    std::unique_ptr<EvalGraph>& graph = compileResult.graph;
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

    CompileResult compileResult = EvalGraph::Compile(network);

    std::unique_ptr<EvalGraph>& graph = compileResult.graph;
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

    CompileResult compileResult = EvalGraph::Compile(network);

    std::unique_ptr<EvalGraph>& graph = compileResult.graph;
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

    CompileResult compileResult = EvalGraph::Compile(network);

    std::unique_ptr<EvalGraph>& graph = compileResult.graph;
    if (!graph || !graph->IsValid()) {
        return false;
    }

    const SurfaceClosure closure = graph->Evaluate(ShadingContext{});
    return Test_IsClose(closure.baseColor, Vec3f(0.7f, 0.6f, 0.5f), 1e-4f) &&
           Test_IsClose(closure.opacity, 0.0f) &&
           closure.HasBsdfTree();
}

static bool
TestOpenPbrEvalOptionsSelectAdobeBackend()
{
    MaterialGraph network;

    const std::string termPath = "/Material/OpenPBR";
    GraphNode termNode;
    termNode.nodeTypeId = "ND_open_pbr_surface_surfaceshader";
    termNode.parameters["base_color"] = Value(Vec3f(0.2f, 0.4f, 0.6f));
    network.nodes[termPath] = termNode;

    GraphConnection termConn;
    termConn.upstreamNode = termPath;
    termConn.upstreamOutputName = "out";
    network.terminals["surface"] = termConn;

    CompileResult compileResult = EvalGraph::Compile(network);

    std::unique_ptr<EvalGraph>& graph = compileResult.graph;
    if (!graph || !graph->IsValid()) {
        return false;
    }

    const SurfaceClosure nativeClosure = graph->Evaluate(ShadingContext{});
    const auto* nativeRoot =
        nativeClosure.bsdfTree.Get(nativeClosure.bsdfTree.root);
    if (!nativeRoot ||
        std::holds_alternative<Bsdf::AdobeOpenPbrData>(nativeRoot->data)) {
        printf("    Expected default OpenPBR graph evaluation to stay native\n");
        return false;
    }

    EvalOptions options;
    options.useAdobeOpenPBR = true;
    const SurfaceClosure adobeClosure =
        graph->Evaluate(ShadingContext{}, options);
    const auto* adobeRoot =
        adobeClosure.bsdfTree.Get(adobeClosure.bsdfTree.root);
    if (!adobeRoot) {
        return false;
    }

#ifdef PXR_HDEMBREE_ENABLE_ADOBE_OPENPBR
    if (!std::holds_alternative<Bsdf::AdobeOpenPbrData>(adobeRoot->data)) {
        printf("    Expected OpenPBR option to select Adobe backend\n");
        return false;
    }
#else
    if (std::holds_alternative<Bsdf::AdobeOpenPbrData>(adobeRoot->data)) {
        printf("    Adobe backend node should not appear without support\n");
        return false;
    }
#endif

    return Test_IsClose(
        adobeClosure.baseColor, Vec3f(0.2f, 0.4f, 0.6f), 1e-4f);
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

    CompileResult compileResult = EvalGraph::Compile(network);

    std::unique_ptr<EvalGraph>& graph = compileResult.graph;
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

    CompileResult compileResult = EvalGraph::Compile(network);

    std::unique_ptr<EvalGraph>& graph = compileResult.graph;
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

    CompileResult compileResult = EvalGraph::Compile(network);

    std::unique_ptr<EvalGraph>& graph = compileResult.graph;
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

    CompileResult compileResult = EvalGraph::Compile(network);

    std::unique_ptr<EvalGraph>& graph = compileResult.graph;
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

    CompileResult compileResult = EvalGraph::Compile(network);

    std::unique_ptr<EvalGraph>& graph = compileResult.graph;
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

    CompileResult compileResult = EvalGraph::Compile(network);

    std::unique_ptr<EvalGraph>& graph = compileResult.graph;
    if (compileResult.status != CompileStatus::Invalid ||
        graph ||
        compileResult.diagnostic !=
            "no evaluator registered for node type "
            "ND_totally_unknown_float at /Material/Unknown") {
        return false;
    }

    network.terminals["surface"] = unknownConn;
    CompileResult terminalResult = EvalGraph::Compile(network);
    return terminalResult.status == CompileStatus::Invalid &&
           !terminalResult.graph &&
           terminalResult.diagnostic ==
               "no evaluator registered for node type "
               "ND_totally_unknown_float at /Material/Unknown";
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

    CompileResult compileResult = EvalGraph::Compile(network);

    std::unique_ptr<EvalGraph>& graph = compileResult.graph;
    return compileResult.status == CompileStatus::Invalid &&
           !graph &&
           compileResult.diagnostic ==
               "cycle detected through node /Material/A";
}

static bool
TestCompileRejectsMissingUpstreamNode()
{
    MaterialGraph network;

    GraphNode add;
    add.nodeTypeId = "ND_add_float";
    add.parameters["in2"] = Value(1.0f);
    add.inputConnections["in1"] =
        {{"/Material/Missing", "out"}};
    network.nodes["/Material/Add"] = add;

    GraphNode surface;
    surface.nodeTypeId = "UsdPreviewSurface";
    surface.inputConnections["roughness"] =
        {{"/Material/Add", "out"}};
    network.nodes["/Material/Surface"] = surface;
    network.terminals["surface"] = {"/Material/Surface", "out"};

    CompileResult result = EvalGraph::Compile(network);
    return result.status == CompileStatus::Invalid &&
           !result.graph &&
           result.diagnostic ==
               "/Material/Add (ND_add_float) input in1 references missing "
               "node /Material/Missing";
}

static bool
TestCompileRejectsTerminalCycles()
{
    MaterialGraph network;

    GraphNode upstream;
    upstream.nodeTypeId = "ND_add_float";
    upstream.parameters["in2"] = Value(1.0f);
    upstream.inputConnections["in1"] =
        {{"/Material/Surface", "out"}};
    network.nodes["/Material/Upstream"] = upstream;

    GraphNode surface;
    surface.nodeTypeId = "UsdPreviewSurface";
    surface.inputConnections["roughness"] =
        {{"/Material/Upstream", "out"}};
    network.nodes["/Material/Surface"] = surface;
    network.terminals["surface"] = {"/Material/Surface", "out"};

    CompileResult upstreamCycle = EvalGraph::Compile(network);
    if (upstreamCycle.status != CompileStatus::Invalid ||
        upstreamCycle.graph ||
        upstreamCycle.diagnostic !=
            "cycle detected through node /Material/Surface") {
        return false;
    }

    network.nodes["/Material/Surface"].inputConnections["roughness"] =
        {{"/Material/Surface", "out"}};
    CompileResult selfCycle = EvalGraph::Compile(network);
    return selfCycle.status == CompileStatus::Invalid &&
           !selfCycle.graph &&
           selfCycle.diagnostic ==
               "cycle detected through node /Material/Surface";
}

static bool
TestCompileRejectsMissingTerminalNode()
{
    MaterialGraph network;
    network.terminals["displacement"] =
        {"/Material/Disp", "out"};

    CompileResult result =
        EvalGraph::Compile(network, "displacement");
    return result.status == CompileStatus::Invalid &&
           !result.graph &&
           result.diagnostic ==
               "terminal \"displacement\" references missing node "
               "/Material/Disp";
}

static bool
TestCompileMissingDisplacementTerminal()
{
    MaterialGraph network;
    GraphNode surface;
    surface.nodeTypeId = "UsdPreviewSurface";
    network.nodes["/Material/Surface"] = surface;
    network.terminals["surface"] = {"/Material/Surface", "out"};

    CompileResult compileResult = EvalGraph::Compile(network, "displacement");

    return compileResult.status == CompileStatus::AbsentTerminal &&
           !compileResult.graph && compileResult.diagnostic.empty();
}

static bool
TestCompileVolumeOnlyMaterial()
{
    MaterialGraph network;

    GraphNode vdf;
    vdf.nodeTypeId = "ND_anisotropic_vdf";
    vdf.parameters["absorption"] = Value(Vec3f(0.1f, 0.2f, 0.3f));
    vdf.parameters["scattering"] = Value(Vec3f(0.4f, 0.5f, 0.6f));
    vdf.parameters["anisotropy"] = Value(0.25f);
    network.nodes["/Material/Vdf"] = vdf;

    GraphNode volume;
    volume.nodeTypeId = "ND_volume";
    volume.inputConnections["vdf"] = {{"/Material/Vdf", "out"}};
    network.nodes["/Material/Volume"] = volume;
    network.terminals["volume"] = {"/Material/Volume", "out"};

    CompileResult result = EvalGraph::Compile(network);
    if (result.status != CompileStatus::Valid ||
        !result.graph ||
        !result.diagnostic.empty()) {
        return false;
    }

    const SurfaceClosure closure =
        result.graph->Evaluate(ShadingContext{});
    CompileResult explicitResult =
        EvalGraph::Compile(network, "surface");
    if (explicitResult.status != CompileStatus::Valid ||
        !explicitResult.graph ||
        !explicitResult.diagnostic.empty()) {
        return false;
    }
    const SurfaceClosure explicitClosure =
        explicitResult.graph->Evaluate(ShadingContext{});

    GraphNode vacuumVolume;
    vacuumVolume.nodeTypeId = "ND_volume";
    network.nodes.clear();
    network.nodes["/Material/Volume"] = vacuumVolume;
    network.terminals["volume"] = {"/Material/Volume", "out"};
    CompileResult vacuumResult = EvalGraph::Compile(network);
    if (vacuumResult.status != CompileStatus::Valid ||
        !vacuumResult.graph) {
        return false;
    }
    const SurfaceClosure vacuumClosure =
        vacuumResult.graph->Evaluate(ShadingContext{});

    return closure.opacity == 0.0f && closure.isVolumeBoundary &&
           closure.hasInteriorMedium &&
           Test_IsClose(closure.interiorMedium.absorption,
                        Vec3f(0.1f, 0.2f, 0.3f)) &&
           Test_IsClose(closure.interiorMedium.scattering,
                        Vec3f(0.4f, 0.5f, 0.6f)) &&
           Test_IsClose(closure.interiorMedium.anisotropy, 0.25f) &&
           explicitClosure.opacity == closure.opacity &&
           explicitClosure.isVolumeBoundary &&
           explicitClosure.hasInteriorMedium == closure.hasInteriorMedium &&
           Test_IsClose(explicitClosure.interiorMedium.absorption,
                        closure.interiorMedium.absorption) &&
           Test_IsClose(explicitClosure.interiorMedium.scattering,
                        closure.interiorMedium.scattering) &&
           Test_IsClose(explicitClosure.interiorMedium.anisotropy,
                        closure.interiorMedium.anisotropy) &&
           vacuumClosure.opacity == 0.0f && vacuumClosure.isVolumeBoundary &&
           !vacuumClosure.hasInteriorMedium;
}

static bool
TestMixSurfaceClosuresPreservesVolumeBoundaryIdentity()
{
    MaterialGraph network;
    GraphNode bgVolume;
    bgVolume.nodeTypeId = "ND_volume";
    network.nodes["/Material/BgVolume"] = bgVolume;
    GraphNode fgVolume;
    fgVolume.nodeTypeId = "ND_volume";
    network.nodes["/Material/FgVolume"] = fgVolume;
    GraphNode mixNode;
    mixNode.nodeTypeId = "ND_mix_surfaceshader";
    mixNode.parameters["mix"] = Value(0.5f);
    mixNode.inputConnections["bg"] = {
        {"/Material/BgVolume", "out"}};
    mixNode.inputConnections["fg"] = {
        {"/Material/FgVolume", "out"}};
    network.nodes["/Material/Mix"] = mixNode;
    network.terminals["surface"] = {"/Material/Mix", "out"};

    CompileResult compileResult = EvalGraph::Compile(network);
    if (compileResult.status != CompileStatus::Valid ||
        !compileResult.graph) {
        return false;
    }
    const SurfaceClosure graphVolumeMix =
        compileResult.graph->Evaluate(ShadingContext{});

    GraphNode dielectric;
    dielectric.nodeTypeId = "ND_dielectric_bsdf";
    network.nodes["/Material/Dielectric"] = dielectric;
    GraphNode surface;
    surface.nodeTypeId = "ND_surface";
    surface.inputConnections["bsdf"] = {
        {"/Material/Dielectric", "out"}};
    network.nodes["/Material/Surface"] = surface;

    network.nodes["/Material/Mix"].parameters["mix"] = Value(0.0f);
    network.nodes["/Material/Mix"].inputConnections["fg"] = {
        {"/Material/Surface", "out"}};
    CompileResult bgEndpointResult = EvalGraph::Compile(network);
    if (bgEndpointResult.status != CompileStatus::Valid ||
        !bgEndpointResult.graph) {
        return false;
    }
    const SurfaceClosure graphBgEndpoint =
        bgEndpointResult.graph->Evaluate(ShadingContext{});

    GraphNode vdf;
    vdf.nodeTypeId = "ND_anisotropic_vdf";
    vdf.parameters["absorption"] = Value(Vec3f(0.25f));
    network.nodes["/Material/Vdf"] = vdf;
    GraphNode mediumVolume;
    mediumVolume.nodeTypeId = "ND_volume";
    mediumVolume.inputConnections["vdf"] = {
        {"/Material/Vdf", "out"}};
    network.nodes["/Material/MediumVolume"] = mediumVolume;
    network.nodes["/Material/Mix"].parameters["mix"] = Value(1.0f);
    network.nodes["/Material/Mix"].inputConnections["bg"] = {
        {"/Material/Surface", "out"}};
    network.nodes["/Material/Mix"].inputConnections["fg"] = {
        {"/Material/MediumVolume", "out"}};
    CompileResult fgEndpointResult = EvalGraph::Compile(network);
    if (fgEndpointResult.status != CompileStatus::Valid ||
        !fgEndpointResult.graph) {
        return false;
    }
    const SurfaceClosure graphFgEndpoint =
        fgEndpointResult.graph->Evaluate(ShadingContext{});

    const SurfaceClosure volume =
        MakeVolumeSurfaceClosure(VdfClosure{});
    const SurfaceClosure opaque =
        MakeUnlitSurfaceClosure(Vec3f(1.0f));

    const SurfaceClosure volumeMix =
        MixSurfaceClosures(volume, volume, 0.5f);
    const SurfaceClosure bgEndpoint =
        MixSurfaceClosures(volume, opaque, 0.0f);
    const SurfaceClosure fgEndpoint =
        MixSurfaceClosures(opaque, volume, 1.0f);
    const SurfaceClosure surfaceMix =
        MixSurfaceClosures(volume, opaque, 0.5f);

    return graphVolumeMix.isVolumeBoundary &&
           graphBgEndpoint.isVolumeBoundary && !graphBgEndpoint.HasBsdfTree() &&
           graphBgEndpoint.opacity == 0.0f &&
           !graphBgEndpoint.hasInteriorMedium &&
           graphFgEndpoint.isVolumeBoundary && !graphFgEndpoint.HasBsdfTree() &&
           graphFgEndpoint.opacity == 0.0f &&
           graphFgEndpoint.hasInteriorMedium &&
           Test_IsClose(graphFgEndpoint.interiorMedium.absorption,
                        Vec3f(0.25f)) &&
           volumeMix.isVolumeBoundary && bgEndpoint.isVolumeBoundary &&
           fgEndpoint.isVolumeBoundary && !surfaceMix.isVolumeBoundary;
}

static bool
TestEvaluateConstantDisplacement()
{
    MaterialGraph network;
    GraphNode terminal;
    terminal.nodeTypeId = "ND_displacement_float";
    terminal.parameters["displacement"] = Value(0.2f);
    terminal.parameters["scale"] = Value(3.0f);
    network.nodes["/Material/Displacement"] = terminal;
    network.terminals["displacement"] =
        {"/Material/Displacement", "out"};

    CompileResult compileResult = EvalGraph::Compile(network, "displacement");

    std::unique_ptr<EvalGraph>& graph = compileResult.graph;
    float displacement = 0.0f;
    return graph && graph->IsValid() &&
           graph->EvaluateDisplacement(ShadingContext{}, &displacement) &&
           Test_IsClose(displacement, 0.6f);
}

static bool
TestEvaluatePositionSineDisplacement()
{
    MaterialGraph network;

    GraphNode position;
    position.nodeTypeId = "ND_position_vector3";
    network.nodes["/Material/Position"] = position;

    GraphNode extract;
    extract.nodeTypeId = "ND_extract_vector3";
    extract.parameters["index"] = Value(0);
    extract.inputConnections["in"] =
        {{"/Material/Position", "out"}};
    network.nodes["/Material/ExtractX"] = extract;

    GraphNode frequency;
    frequency.nodeTypeId = "ND_multiply_float";
    frequency.parameters["in2"] = Value(2.0f);
    frequency.inputConnections["in1"] =
        {{"/Material/ExtractX", "out"}};
    network.nodes["/Material/Frequency"] = frequency;

    GraphNode sine;
    sine.nodeTypeId = "ND_sin_float";
    sine.inputConnections["in"] =
        {{"/Material/Frequency", "out"}};
    network.nodes["/Material/Sine"] = sine;

    GraphNode terminal;
    terminal.nodeTypeId = "ND_displacement_float";
    terminal.parameters["scale"] = Value(0.25f);
    terminal.inputConnections["displacement"] =
        {{"/Material/Sine", "out"}};
    network.nodes["/Material/Displacement"] = terminal;
    network.terminals["displacement"] =
        {"/Material/Displacement", "out"};

    CompileResult compileResult = EvalGraph::Compile(network, "displacement");

    std::unique_ptr<EvalGraph>& graph = compileResult.graph;
    ShadingContext context;
    context.position = Vec3f(0.78539816339f, 0.0f, 0.0f);
    float displacement = 0.0f;
    return graph && graph->IsValid() &&
           graph->EvaluateDisplacement(context, &displacement) &&
           Test_IsClose(displacement, 0.25f, 1.0e-4f);
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

    CompileResult compileResult = EvalGraph::Compile(network);

    std::unique_ptr<EvalGraph>& graph = compileResult.graph;
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
    _REG(TestOpenPbrEvalOptionsSelectAdobeBackend);
    _REG(TestCompileLinearChain);
    _REG(TestCompileDiamondDAG);
    _REG(TestEvalWithConstantInputs);
    _REG(TestEvalMultiplyChain);
    _REG(TestEvalGeometricInput);
    _REG(TestCompileUnknownNodeTypeFails);
    _REG(TestCompileRejectsCycle);
    _REG(TestCompileRejectsMissingUpstreamNode);
    _REG(TestCompileRejectsTerminalCycles);
    _REG(TestCompileRejectsMissingTerminalNode);
    _REG(TestCompileMissingDisplacementTerminal);
    _REG(TestCompileVolumeOnlyMaterial);
    _REG(TestMixSurfaceClosuresPreservesVolumeBoundaryIdentity);
    _REG(TestEvaluateConstantDisplacement);
    _REG(TestEvaluatePositionSineDisplacement);
    _REG(TestInputReevaluationUsesModifiedContext);
}

#undef _REG
