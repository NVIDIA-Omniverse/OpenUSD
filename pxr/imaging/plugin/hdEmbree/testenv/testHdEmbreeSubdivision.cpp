//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/imaging/plugin/hdEmbree/delegate/adaptiveSubdivision.h"
#include "pxr/imaging/plugin/hdEmbree/delegate/displacement.h"
#include "pxr/imaging/plugin/hdEmbree/delegate/material.h"
#include "pxr/imaging/plugin/hdEmbree/delegate/mesh.h"
#include "pxr/imaging/plugin/hdEmbree/delegate/rendererPlugin.h"
#include "pxr/imaging/plugin/hdEmbree/delegate/renderDelegate.h"
#include "pxr/imaging/plugin/hdEmbree/delegate/renderParam.h"
#include "pxr/imaging/plugin/hdEmbree/renderer/geometry/context.h"
#include "pxr/imaging/plugin/hdEmbree/renderer/geometry/meshSamplers.h"
#include "pxr/imaging/plugin/hdEmbree/renderer/geometry/primvarSampling.h"
#include "pxr/imaging/plugin/hdEmbree/renderer/materials/MaterialXCpp/graph.h"
#include "pxr/imaging/plugin/hdEmbree/renderer/materials/material.h"

#include "pxr/imaging/hd/camera.h"
#include "pxr/imaging/hd/renderIndex.h"
#include "pxr/imaging/hd/renderPass.h"
#include "pxr/imaging/hd/renderPassState.h"
#include "pxr/imaging/hd/rprim.h"
#include "pxr/imaging/hd/repr.h"
#include "pxr/imaging/hd/unitTestDelegate.h"
#include "pxr/imaging/pxOsd/tokens.h"
#include "pxr/usd/sdf/layer.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usdImaging/usdImaging/delegate.h"

#include <embree4/rtcore.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

using namespace mxcpp;

struct _EmbreeTestContext
{
    _EmbreeTestContext()
        : renderDelegate(plugin.CreateRenderDelegate())
        , renderIndex(renderDelegate
              ? HdRenderIndex::New(renderDelegate, HdDriverVector())
              : nullptr)
    {
    }

    ~_EmbreeTestContext()
    {
        renderIndex.reset();
        if (renderDelegate) {
            plugin.DeleteRenderDelegate(renderDelegate);
        }
    }

    HdEmbreeRendererPlugin plugin;
    HdRenderDelegate* renderDelegate;
    std::unique_ptr<HdRenderIndex> renderIndex;
};

bool
_Close(float a, float b, float epsilon = 1.0e-3f)
{
    return std::abs(a - b) <= epsilon;
}

/// Embree has four relevant boundary modes for USD's six authored rules. Test
/// the production mapper directly so a visually stable but incorrect golden
/// image cannot redefine the intended approximation.
bool
TestFaceVaryingRulesMapToEmbreeModes()
{
    return
        HdEmbreeGetFaceVaryingSubdivisionMode(TfToken()) ==
            RTC_SUBDIVISION_MODE_SMOOTH_BOUNDARY &&
        HdEmbreeGetFaceVaryingSubdivisionMode(
            PxOsdOpenSubdivTokens->none) ==
            RTC_SUBDIVISION_MODE_SMOOTH_BOUNDARY &&
        HdEmbreeGetFaceVaryingSubdivisionMode(
            PxOsdOpenSubdivTokens->cornersOnly) ==
            RTC_SUBDIVISION_MODE_PIN_CORNERS &&
        HdEmbreeGetFaceVaryingSubdivisionMode(
            PxOsdOpenSubdivTokens->cornersPlus1) ==
            RTC_SUBDIVISION_MODE_PIN_CORNERS &&
        HdEmbreeGetFaceVaryingSubdivisionMode(
            PxOsdOpenSubdivTokens->cornersPlus2) ==
            RTC_SUBDIVISION_MODE_PIN_CORNERS &&
        HdEmbreeGetFaceVaryingSubdivisionMode(
            PxOsdOpenSubdivTokens->boundaries) ==
            RTC_SUBDIVISION_MODE_PIN_BOUNDARY &&
        HdEmbreeGetFaceVaryingSubdivisionMode(
            PxOsdOpenSubdivTokens->all) ==
            RTC_SUBDIVISION_MODE_PIN_ALL;
}

std::vector<float>
_Compute(
    VtVec3fArray const& points,
    VtIntArray const& counts,
    VtIntArray const& indices,
    std::vector<GfMatrix4f> const& transforms,
    int refineLevel,
    GfMatrix4d const& projection = GfMatrix4d(1.0))
{
    return HdEmbreeComputeAdaptiveSubdivisionLevels(
        points, counts, indices, transforms,
        GfMatrix4d(1.0), projection,
        GfRect2i(GfVec2i(0), 100, 100), refineLevel);
}

bool
TestComplexityTargetsExactPixelLengths()
{
    const VtVec3fArray points{
        GfVec3f(-1.0f, 0.0f, 0.0f),
        GfVec3f(1.0f, 0.0f, 0.0f),
        GfVec3f(0.0f, 0.5f, 0.0f)};
    const VtIntArray counts{3};
    const VtIntArray indices{0, 1, 2};
    const std::vector<GfMatrix4f> transforms{GfMatrix4f(1.0f)};
    const float expected[] = {0.0f, 25.0f, 100.0f, 200.0f};
    // Refine level zero uses the triangulated cage in production; adaptive
    // edge targets apply only to medium, high, and very-high complexity.
    for (int refineLevel = 1; refineLevel != 4; ++refineLevel) {
        const std::vector<float> levels =
            _Compute(points, counts, indices, transforms, refineLevel);
        if (levels.size() != 3 || levels[0] != expected[refineLevel]) {
            std::printf("    refine %d level %g, expected %g\n",
                        refineLevel,
                        levels.empty() ? -1.0f : levels[0],
                        expected[refineLevel]);
            return false;
        }
    }
    return true;
}

bool
TestSharedEdgesUseSameMaximumLevel()
{
    // Supply deliberately different candidates for the same reversed edge.
    // This directly fails if shared-edge max consolidation is removed.
    const std::vector<float> levels =
        HdEmbreeConsolidateSharedEdgeLevels(
            VtIntArray{3, 3}, VtIntArray{0, 1, 2, 1, 0, 3},
            std::vector<float>{12.0f, 2.0f, 3.0f, 57.0f, 4.0f, 5.0f});
    return levels.size() == 6 && levels[0] == 57.0f &&
        levels[3] == 57.0f;
}

bool
TestLargestInstanceProjectionWins()
{
    GfMatrix4f twice(1.0f);
    twice.SetScale(GfVec3f(2.0f, 1.0f, 1.0f));
    const std::vector<float> levels = _Compute(
        VtVec3fArray{
            GfVec3f(-0.5f, 0.0f, 0.0f),
            GfVec3f(0.5f, 0.0f, 0.0f),
            GfVec3f(0.0f, 0.5f, 0.0f)},
        VtIntArray{3}, VtIntArray{0, 1, 2},
        {GfMatrix4f(1.0f), twice}, 2);
    return levels.size() == 3 && levels[0] == 100.0f;
}

bool
TestViewportChangeUpdatesLevels()
{
    const VtVec3fArray points{
        GfVec3f(-1.0f, 0.0f, 0.0f),
        GfVec3f(1.0f, 0.0f, 0.0f),
        GfVec3f(0.0f, 0.5f, 0.0f)};
    const auto compute = [&](int width) {
        return HdEmbreeComputeAdaptiveSubdivisionLevels(
            points, VtIntArray{3}, VtIntArray{0, 1, 2},
            {GfMatrix4f(1.0f)}, GfMatrix4d(1.0), GfMatrix4d(1.0),
            GfRect2i(GfVec2i(0), width, 100), 2);
    };
    const std::vector<float> small = compute(100);
    const std::vector<float> large = compute(200);
    return small.size() == 3 && large.size() == 3 &&
        small[0] == 100.0f && large[0] == 200.0f;
}

bool
TestLevelsClampToEmbreeRange()
{
    const std::vector<float> minimum = _Compute(
        VtVec3fArray{
            GfVec3f(0.0f), GfVec3f(0.0f), GfVec3f(0.0f)},
        VtIntArray{3}, VtIntArray{0, 1, 2},
        {GfMatrix4f(1.0f)}, 3);

    const std::vector<float> maximum =
        HdEmbreeComputeAdaptiveSubdivisionLevels(
            VtVec3fArray{
                GfVec3f(-1.0f, 0.0f, 0.0f),
                GfVec3f(1.0f, 0.0f, 0.0f),
                GfVec3f(0.0f, 0.5f, 0.0f)},
            VtIntArray{3}, VtIntArray{0, 1, 2},
            {GfMatrix4f(1.0f)}, GfMatrix4d(1.0), GfMatrix4d(1.0),
            GfRect2i(GfVec2i(0), 100000, 100000), 3);
    return minimum.size() == 3 && maximum.size() == 3 &&
        minimum[0] == 1.0f && maximum[0] == 4096.0f;
}

bool
TestEdgeCrossingNearPlaneIsClippedBeforeProjection()
{
    // w = z and clip-z = z - 1, so the near plane is z = 0.5.
    GfMatrix4d projection(0.0);
    projection[0][0] = 1.0;
    projection[1][1] = 1.0;
    projection[2][2] = 1.0;
    projection[2][3] = 1.0;
    projection[3][2] = -1.0;

    const std::vector<float> levels = _Compute(
        VtVec3fArray{
            GfVec3f(-1.0f, 0.0f, -1.0f),
            GfVec3f(0.0f, 0.0f, 1.0f),
            GfVec3f(0.0f, 0.5f, 1.0f)},
        VtIntArray{3}, VtIntArray{0, 1, 2},
        {GfMatrix4f(1.0f)}, 2, projection);
    return levels.size() == 3 && levels[0] == 25.0f;
}

bool
TestEdgesAreClippedToViewportSides()
{
    const std::vector<float> fullyOutside = _Compute(
        VtVec3fArray{
            GfVec3f(-4.0f, 0.0f, 0.0f),
            GfVec3f(-2.0f, 0.0f, 0.0f),
            GfVec3f(-3.0f, 0.5f, 0.0f)},
        VtIntArray{3}, VtIntArray{0, 1, 2},
        {GfMatrix4f(1.0f)}, 2);
    const std::vector<float> partlyOutside = _Compute(
        VtVec3fArray{
            GfVec3f(-3.0f, 0.0f, 0.0f),
            GfVec3f(0.0f, 0.0f, 0.0f),
            GfVec3f(0.0f, 0.5f, 0.0f)},
        VtIntArray{3}, VtIntArray{0, 1, 2},
        {GfMatrix4f(1.0f)}, 2);
    return fullyOutside.size() == 3 && fullyOutside[0] == 1.0f &&
        partlyOutside.size() == 3 && partlyOutside[0] == 50.0f;
}

std::unique_ptr<EvalGraph>
_CompileConstantDisplacement(float value)
{
    MaterialGraph network;
    GraphNode terminal;
    terminal.nodeTypeId = "ND_displacement_float";
    terminal.parameters["displacement"] = Value(value);
    terminal.parameters["scale"] = Value(1.0f);
    const std::string path = "/Material/Displacement";
    network.nodes[path] = terminal;
    network.terminals["displacement"] = GraphConnection{path, "out"};
    return EvalGraph::Compile(network, "displacement");
}

std::unique_ptr<EvalGraph>
_CompileGeomPropDisplacement(std::string const& name)
{
    MaterialGraph network;
    GraphNode geomprop;
    geomprop.nodeTypeId = "ND_geompropvalue_float";
    geomprop.parameters["geomprop"] = Value(name);
    geomprop.parameters["default"] = Value(0.0f);
    network.nodes["/Material/GeomProp"] = geomprop;

    GraphNode terminal;
    terminal.nodeTypeId = "ND_displacement_float";
    terminal.parameters["scale"] = Value(1.0f);
    terminal.inputConnections["displacement"] =
        {{"/Material/GeomProp", "out"}};
    network.nodes["/Material/Displacement"] = terminal;
    network.terminals["displacement"] =
        {"/Material/Displacement", "out"};
    return EvalGraph::Compile(network, "displacement");
}

std::unique_ptr<EvalGraph>
_CompileTexcoordDisplacement()
{
    MaterialGraph network;
    GraphNode texcoord;
    texcoord.nodeTypeId = "ND_texcoord_vector2";
    network.nodes["/Material/Texcoord"] = texcoord;

    GraphNode extract;
    extract.nodeTypeId = "ND_extract_vector2";
    extract.parameters["index"] = Value(0);
    extract.inputConnections["in"] = {{"/Material/Texcoord", "out"}};
    network.nodes["/Material/ExtractU"] = extract;

    GraphNode terminal;
    terminal.nodeTypeId = "ND_displacement_float";
    terminal.parameters["scale"] = Value(1.0f);
    terminal.inputConnections["displacement"] =
        {{"/Material/ExtractU", "out"}};
    network.nodes["/Material/Displacement"] = terminal;
    network.terminals["displacement"] =
        {"/Material/Displacement", "out"};
    return EvalGraph::Compile(network, "displacement");
}

float
_TraceCenter(RTCScene scene)
{
    RTCRayHit rayHit{};
    rayHit.ray.org_x = 0.5f;
    rayHit.ray.org_y = 0.5f;
    rayHit.ray.org_z = 2.0f;
    rayHit.ray.dir_z = -1.0f;
    rayHit.ray.tnear = 0.0f;
    rayHit.ray.tfar = std::numeric_limits<float>::infinity();
    rayHit.ray.mask = 0xffffffffu;
    rayHit.hit.geomID = RTC_INVALID_GEOMETRY_ID;
    rtcIntersect1(scene, &rayHit);
    return rayHit.ray.tfar;
}

bool
TestRealCallbackAddsRemovesAndReplacesDisplacement()
{
    RTCDevice device = rtcNewDevice(nullptr);
    RTCScene scene = rtcNewScene(device);
    RTCGeometry geometry =
        rtcNewGeometry(device, RTC_GEOMETRY_TYPE_SUBDIVISION);

    VtVec3fArray points{
        GfVec3f(0.0f, 0.0f, 0.0f),
        GfVec3f(1.0f, 0.0f, 0.0f),
        GfVec3f(1.0f, 1.0f, 0.0f),
        GfVec3f(0.0f, 1.0f, 0.0f)};
    VtIntArray counts{4};
    VtIntArray indices{0, 1, 2, 3};
    std::vector<float> levels(4, 8.0f);
    rtcSetSharedGeometryBuffer(
        geometry, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3,
        points.cdata(), 0, sizeof(GfVec3f), points.size());
    rtcSetSharedGeometryBuffer(
        geometry, RTC_BUFFER_TYPE_FACE, 0, RTC_FORMAT_UINT,
        counts.cdata(), 0, sizeof(int), counts.size());
    rtcSetSharedGeometryBuffer(
        geometry, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT,
        indices.cdata(), 0, sizeof(int), indices.size());
    rtcSetSharedGeometryBuffer(
        geometry, RTC_BUFFER_TYPE_LEVEL, 0, RTC_FORMAT_FLOAT,
        levels.data(), 0, sizeof(float), levels.size());

    // Exercise the actual subdivision samplers inside Embree's callback,
    // including a float3 st primvar. The detached sample below separately
    // proves sampler lifetime does not depend on a scene/id lookup.
    rtcSetGeometryTopologyCount(geometry, 3);
    rtcSetGeometrySubdivisionMode(
        geometry, 1, RTC_SUBDIVISION_MODE_PIN_ALL);
    rtcSetGeometrySubdivisionMode(
        geometry, 2, RTC_SUBDIVISION_MODE_PIN_ALL);
    rtcSetSharedGeometryBuffer(
        geometry, RTC_BUFFER_TYPE_INDEX, 1, RTC_FORMAT_UINT,
        indices.cdata(), 0, sizeof(int), indices.size());
    rtcSetSharedGeometryBuffer(
        geometry, RTC_BUFFER_TYPE_INDEX, 2, RTC_FORMAT_UINT,
        indices.cdata(), 0, sizeof(int), indices.size());

    HdEmbreeRTCBufferAllocator allocator;
    HdEmbreeSubdivVaryingSampler heightSampler(
        TfToken("height"),
        VtValue(VtFloatArray{0.0f, 1.0f, 1.0f, 0.0f}),
        geometry, &allocator);
    HdEmbreeSubdivFaceVaryingSampler stSampler(
        TfToken("st"),
        VtValue(VtVec3fArray{
            GfVec3f(0.0f, 0.0f, 9.0f),
            GfVec3f(1.0f, 0.0f, 9.0f),
            GfVec3f(1.0f, 1.0f, 9.0f),
            GfVec3f(0.0f, 1.0f, 9.0f)}),
        geometry, 2, &allocator);

    HdEmbreePrototypeContext context;
    HdEmbreeMaterialData material;
    context.material = &material;
    context.primvarMap[TfToken("st")] = &stSampler;
    context.primvarMapByString["height"] = &heightSampler;
    rtcSetGeometryUserData(geometry, &context);
    rtcSetGeometryDisplacementFunction(
        geometry, HdEmbreeDisplacementFunction);
    rtcAttachGeometry(scene, geometry);

    // The positive graph proves the callback exposes mesh primvars rather
    // than merely evaluating graph constants.
    auto positive = _CompileGeomPropDisplacement("height");
    auto texcoord = _CompileTexcoordDisplacement();
    auto negative = _CompileConstantDisplacement(-0.25f);
    if (!positive || !positive->IsValid() ||
        !texcoord || !texcoord->IsValid() ||
        !negative || !negative->IsValid()) {
        rtcReleaseGeometry(geometry);
        rtcReleaseScene(scene);
        rtcReleaseDevice(device);
        return false;
    }

    const auto rebuild = [&]() {
        rtcUpdateGeometryBuffer(geometry, RTC_BUFFER_TYPE_LEVEL, 0);
        rtcCommitGeometry(geometry);
        rtcCommitScene(scene);
    };

    material.displacementGraph = positive.get();
    rebuild();
    const float positiveHit = _TraceCenter(scene);
    material.displacementGraph = texcoord.get();
    rebuild();
    const float float3TexcoordHit = _TraceCenter(scene);
    material.displacementGraph = nullptr;
    rebuild();
    const float undisplacedHit = _TraceCenter(scene);
    material.displacementGraph = negative.get();
    rebuild();
    const float negativeHit = _TraceCenter(scene);

    // A retained geometry remains valid after its scene is gone. Sampling here
    // would fail—or touch released scene state—if callbacks stored scene/id
    // pairs instead of the geometry handle captured during mesh sync.
    rtcReleaseScene(scene);
    float detachedHeight = -1.0f;
    const bool detachedSampled =
        heightSampler.Sample(0, 0.5f, 0.5f, &detachedHeight);

    rtcReleaseGeometry(geometry);
    rtcReleaseDevice(device);
    return _Close(positiveHit, 1.5f, 0.02f) &&
        _Close(float3TexcoordHit, 1.5f, 0.02f) &&
        _Close(undisplacedHit, 2.0f, 0.02f) &&
        _Close(negativeHit, 2.25f, 0.02f) &&
        detachedSampled && _Close(detachedHeight, 0.5f, 0.02f);
}

HdMaterialNetwork2
_MakeHydraMaterialNetwork(bool withDisplacement, float displacement = 0.0f)
{
    HdMaterialNetwork2 network;
    const SdfPath surfacePath("/Material/Surface");
    HdMaterialNode2 surface;
    surface.nodeTypeId = TfToken("UsdPreviewSurface");
    network.nodes[surfacePath] = surface;
    network.terminals[TfToken("surface")] =
        HdMaterialConnection2{surfacePath, TfToken("out")};
    if (withDisplacement) {
        const SdfPath displacementPath("/Material/Displacement");
        HdMaterialNode2 node;
        node.nodeTypeId = TfToken("ND_displacement_float");
        node.parameters[TfToken("displacement")] = VtValue(displacement);
        node.parameters[TfToken("scale")] = VtValue(1.0f);
        network.nodes[displacementPath] = node;
        network.terminals[TfToken("displacement")] =
            HdMaterialConnection2{displacementPath, TfToken("out")};
    }
    return network;
}

class _MaterialDelegate final : public HdUnitTestDelegate
{
public:
    explicit _MaterialDelegate(HdRenderIndex* index)
        : HdUnitTestDelegate(index, SdfPath::AbsoluteRootPath())
    {
    }

    VtValue GetMaterialResource(SdfPath const&) override
    {
        return resource;
    }

    VtValue resource;
};

bool
TestMaterialSyncKeepsStableHandleAndReplacesDisplacementGraph()
{
    _EmbreeTestContext context;
    HdRenderIndex* renderIndex = context.renderIndex.get();
    if (!renderIndex) {
        return false;
    }

    RTCDevice notificationDevice = rtcNewDevice(nullptr);
    RTCScene notificationScene = rtcNewScene(notificationDevice);
    HdRenderThread renderThread;
    std::atomic<int> sceneVersion{0};
    HdEmbreeRenderParam renderParam(
        notificationDevice, notificationScene, &renderThread, nullptr,
        &sceneVersion);

    bool valid = false;
    {
        _MaterialDelegate delegate(renderIndex);
        HdEmbreeMaterial material(SdfPath("/material"));
        delegate.resource = VtValue(_MakeHydraMaterialNetwork(true, 0.25f));
        HdDirtyBits bits = material.GetInitialDirtyBitsMask();
        material.Sync(&delegate, &renderParam, &bits);
        HdEmbreeMaterialData const* handle = material.GetRenderMaterial();
        bool firstValid = handle->evalGraph && handle->displacementGraph;

        delegate.resource = VtValue(_MakeHydraMaterialNetwork(false));
        bits = HdMaterial::AllDirty;
        material.Sync(&delegate, &renderParam, &bits);
        bool removed = material.GetRenderMaterial() == handle &&
            handle->evalGraph && !handle->displacementGraph;

        delegate.resource = VtValue(_MakeHydraMaterialNetwork(true, -0.75f));
        bits = HdMaterial::AllDirty;
        material.Sync(&delegate, &renderParam, &bits);
        float value = 0.0f;
        ShadingContext context;
        bool replaced = material.GetRenderMaterial() == handle &&
            handle->displacementGraph &&
            handle->displacementGraph->EvaluateDisplacement(context, &value) &&
            _Close(value, -0.75f);
        valid = firstValid && removed && replaced &&
            sceneVersion.load() == 3;
    }

    rtcReleaseScene(notificationScene);
    rtcReleaseDevice(notificationDevice);
    return valid;
}


bool
TestMaterialChangeForcesProductionDisplacementRecommit()
{
    _EmbreeTestContext context;
    HdRenderDelegate* renderDelegate = context.renderDelegate;
    HdRenderIndex* renderIndex = context.renderIndex.get();
    auto* embreeDelegate =
        dynamic_cast<HdEmbreeRenderDelegate*>(renderDelegate);
    if (!renderIndex || !embreeDelegate) {
        return false;
    }

    bool valid = false;
    {
        HdUnitTestDelegate delegate(
            renderIndex, SdfPath::AbsoluteRootPath());
        const SdfPath materialId("/material");
        const SdfPath meshId("/quad");
        delegate.AddMaterialResource(
            materialId,
            VtValue(_MakeHydraMaterialNetwork(true, 0.5f)));
        delegate.AddMesh(
            meshId, GfMatrix4f(1.0f),
            VtVec3fArray{
                GfVec3f(0.0f, 0.0f, 0.0f),
                GfVec3f(1.0f, 0.0f, 0.0f),
                GfVec3f(1.0f, 1.0f, 0.0f),
                GfVec3f(0.0f, 1.0f, 0.0f)},
            VtIntArray{4}, VtIntArray{0, 1, 2, 3});
        delegate.BindMaterial(meshId, materialId);
        delegate.SetRefineLevel(meshId, 2);

        HdSprim* material = renderIndex->GetSprim(
            HdPrimTypeTokens->material, materialId);
        HdDirtyBits materialBits = material->GetInitialDirtyBitsMask();
        material->Sync(
            &delegate, renderDelegate->GetRenderParam(), &materialBits);

        HdRprim* mesh = const_cast<HdRprim*>(renderIndex->GetRprim(meshId));
        HdDirtyBits meshBits = mesh->GetInitialDirtyBitsMask();
        mesh->InitRepr(&delegate, HdReprTokens->refined, &meshBits);
        mesh->Sync(
            &delegate, renderDelegate->GetRenderParam(),
            &meshBits, HdReprTokens->refined);

        RTCScene root = static_cast<HdEmbreeRenderParam*>(
            renderDelegate->GetRenderParam())->AcquireSceneForEdit();
        rtcCommitScene(root);
        embreeDelegate->UpdateAdaptiveSubdivision(
            GfMatrix4d(1.0), GfMatrix4d(1.0),
            GfRect2i(GfVec2i(0), 100, 100), true);
        rtcCommitScene(root);
        const float positiveHit = _TraceCenter(root);

        delegate.UpdateMaterialResource(
            materialId,
            VtValue(_MakeHydraMaterialNetwork(true, -0.25f)));
        materialBits = HdMaterial::DirtyResource;
        material->Sync(
            &delegate, renderDelegate->GetRenderParam(), &materialBits);
        const bool rebuilt = embreeDelegate->UpdateAdaptiveSubdivision(
            GfMatrix4d(1.0), GfMatrix4d(1.0),
            GfRect2i(GfVec2i(0), 100, 100), true);
        rtcCommitScene(root);
        const float negativeHit = _TraceCenter(root);

        valid = rebuilt && _Close(positiveHit, 1.5f, 0.02f) &&
            _Close(negativeHit, 2.25f, 0.02f);
    }

    return valid;
}

bool
TestSubdivisionPrimvarsUseHydraInterpolationModes()
{
    _EmbreeTestContext context;
    HdRenderDelegate* renderDelegate = context.renderDelegate;
    HdRenderIndex* renderIndex = context.renderIndex.get();
    if (!renderIndex) {
        return false;
    }

    bool valid = false;
    {
        HdUnitTestDelegate delegate(
            renderIndex, SdfPath::AbsoluteRootPath());
        const SdfPath id("/twoQuads");
        const VtVec3fArray points{
            GfVec3f(0.0f, 0.0f, 0.0f),
            GfVec3f(1.0f, 0.0f, 0.0f),
            GfVec3f(2.5f, 0.0f, 0.0f),
            GfVec3f(0.0f, 1.0f, 0.0f),
            GfVec3f(1.0f, 1.0f, 0.0f),
            GfVec3f(2.5f, 1.0f, 0.0f)};
        const VtIntArray counts{4, 4};
        const VtIntArray indices{0, 1, 4, 3, 1, 2, 5, 4};
        PxOsdSubdivTags tags;
        tags.SetVertexInterpolationRule(PxOsdOpenSubdivTokens->edgeAndCorner);
        tags.SetFaceVaryingInterpolationRule(
            PxOsdOpenSubdivTokens->boundaries);
        delegate.AddMesh(
            id, GfMatrix4f(1.0f), points, counts, indices,
            VtIntArray(), tags,
            VtValue(VtVec3fArray{GfVec3f(0.5f)}),
            HdInterpolationConstant,
            VtValue(VtFloatArray{1.0f}), HdInterpolationConstant);
        delegate.SetRefineLevel(id, 2);

        delegate.AddPrimvar(
            id, TfToken("constantValue"), VtValue(VtFloatArray{1.0f}),
            HdInterpolationConstant, TfToken());
        delegate.AddPrimvar(
            id, TfToken("uniformValue"), VtValue(VtFloatArray{2.0f, 4.0f}),
            HdInterpolationUniform, TfToken());
        const VtFloatArray xValues{0.0f, 1.0f, 2.5f, 0.0f, 1.0f, 2.5f};
        delegate.AddPrimvar(
            id, TfToken("vertexValue"), VtValue(xValues),
            HdInterpolationVertex, TfToken());
        delegate.AddPrimvar(
            id, TfToken("varyingValue"), VtValue(xValues),
            HdInterpolationVarying, TfToken());
        delegate.AddPrimvar(
            id, TfToken("unindexedSeam"),
            VtValue(VtFloatArray{0, 1, 1, 0, 10, 11, 11, 10}),
            HdInterpolationFaceVarying, TfToken());
        delegate.AddPrimvar(
            id, TfToken("indexedShared"), VtValue(xValues),
            HdInterpolationFaceVarying, TfToken(), indices);
        delegate.AddPrimvar(
            id, TfToken("indexedRemapped"),
            VtValue(VtFloatArray{2.0f, 7.0f, 19.0f}),
            HdInterpolationFaceVarying, TfToken(),
            VtIntArray{2, 0, 1, 2, 0, 2, 1, 0});
        delegate.AddPrimvar(
            id, TfToken("vector3Value"), VtValue(points),
            HdInterpolationVertex, TfToken());

        HdRprim* rprim = const_cast<HdRprim*>(renderIndex->GetRprim(id));
        HdDirtyBits bits = rprim->GetInitialDirtyBitsMask();
        rprim->InitRepr(&delegate, HdReprTokens->refined, &bits);
        rprim->Sync(
            &delegate, renderDelegate->GetRenderParam(),
            &bits, HdReprTokens->refined);

        RTCScene root = static_cast<HdEmbreeRenderParam*>(
            renderDelegate->GetRenderParam())->AcquireSceneForEdit();
        rtcCommitScene(root);
        RTCGeometry instance = rtcGetGeometry(root, 0);
        auto* instanceContext = instance
            ? static_cast<HdEmbreeInstanceContext*>(
                rtcGetGeometryUserData(instance))
            : nullptr;
        RTCGeometry prototype = instanceContext
            ? rtcGetGeometry(instanceContext->rootScene, 0)
            : nullptr;
        auto* prototypeContext = prototype
            ? static_cast<HdEmbreePrototypeContext*>(
                rtcGetGeometryUserData(prototype))
            : nullptr;
        if (!prototypeContext) {
            return false;
        }

        const auto sample = [&](char const* name, unsigned int face,
                                float u, float v) {
            HdEmbreePrimvarLookup lookup{
                &prototypeContext->primvarMapByString, face, u, v};
            const Value value = HdEmbreeSamplePrimvar(&lookup, name);
            return ValueHolds<float>(value)
                ? ValueGet<float>(value)
                : std::numeric_limits<float>::quiet_NaN();
        };

        constexpr float u = 0.37f;
        constexpr float v = 0.41f;
        alignas(16) float limitPosition[4] = {};
        rtcInterpolate1(
            prototype, 0, u, v, RTC_BUFFER_TYPE_VERTEX, 0,
            limitPosition, nullptr, nullptr, 3);

        struct Vec3WithCanary {
            GfVec3f value;
            uint32_t canary;
        };
        Vec3WithCanary sampledVector{GfVec3f(0.0f), 0x5a17c0deu};
        Vec3WithCanary sampledDu{GfVec3f(0.0f), 0x18dd3a91u};
        Vec3WithCanary sampledDv{GfVec3f(0.0f), 0x734be20fu};
        auto vectorIt = prototypeContext->primvarMap.find(
            TfToken("vector3Value"));
        auto* vectorSampler = vectorIt != prototypeContext->primvarMap.end()
            ? dynamic_cast<HdEmbreeSubdivVertexSampler*>(vectorIt->second)
            : nullptr;
        const bool vectorSampled = vectorSampler &&
            vectorSampler->SampleWithDerivatives(
                0, u, v, &sampledVector.value,
                &sampledDu.value, &sampledDv.value);

        const float sharedLeft = sample("indexedShared", 0, 1.0f, 0.35f);
        const float sharedRight = sample("indexedShared", 1, 0.0f, 0.35f);
        const float constantValue = sample("constantValue", 0, u, v);
        const float uniform0 = sample("uniformValue", 0, u, v);
        const float uniform1 = sample("uniformValue", 1, u, v);
        const float vertexValue = sample("vertexValue", 0, u, v);
        const float varyingValue = sample("varyingValue", 0, u, v);
        const float seamLeft = sample("unindexedSeam", 0, 1.0f, 0.35f);
        const float seamRight = sample("unindexedSeam", 1, 0.0f, 0.35f);
        const float remapped0 = sample("indexedRemapped", 0, 0.5f, 0.5f);
        const float remapped1 = sample("indexedRemapped", 1, 0.5f, 0.5f);
        valid =
            _Close(constantValue, 1.0f) &&
            _Close(uniform0, 2.0f) && _Close(uniform1, 4.0f) &&
            _Close(vertexValue, limitPosition[0]) &&
            _Close(varyingValue, u) && _Close(sharedLeft, sharedRight) &&
            _Close(seamLeft, 1.0f) && _Close(seamRight, 10.0f) &&
            remapped0 > 10.0f && remapped1 < 9.0f &&
            remapped0 - remapped1 > 3.0f &&
            vectorSampled && sampledVector.canary == 0x5a17c0deu &&
            sampledDu.canary == 0x18dd3a91u &&
            sampledDv.canary == 0x734be20fu &&
            _Close(sampledVector.value[0], limitPosition[0]);
        if (!valid) {
            std::printf(
                "    values c=%g u=(%g,%g) vertex=%g limit=%g varying=%g "
                "shared=(%g,%g) seam=(%g,%g) remapped=(%g,%g) "
                "vec=%d canary=(%x,%x,%x) vecx=%g\n",
                constantValue, uniform0, uniform1, vertexValue,
                limitPosition[0], varyingValue, sharedLeft, sharedRight,
                seamLeft, seamRight, remapped0, remapped1,
                vectorSampled, sampledVector.canary,
                sampledDu.canary, sampledDv.canary, sampledVector.value[0]);
        }
    }
    return valid;
}

bool
TestRenderPassRequiresCameraAndGatesDynamicTessellation()
{
    _EmbreeTestContext context;
    HdRenderDelegate* renderDelegate = context.renderDelegate;
    HdRenderIndex* renderIndex = context.renderIndex.get();
    if (!renderDelegate || !renderIndex) {
        return false;
    }

    HdUnitTestDelegate delegate(
        renderIndex, SdfPath::AbsoluteRootPath());
    const SdfPath meshId("/adaptiveQuad");
    const SdfPath cameraId("/camera");
    delegate.AddMesh(
        meshId, GfMatrix4f(1.0f),
        VtVec3fArray{
            GfVec3f(-1.0f, -0.5f, -5.0f),
            GfVec3f(1.0f, -0.5f, -5.0f),
            GfVec3f(1.0f, 0.5f, -5.0f),
            GfVec3f(-1.0f, 0.5f, -5.0f)},
        VtIntArray{4}, VtIntArray{0, 1, 2, 3});
    delegate.SetRefineLevel(meshId, 2);

    HdRprim* mesh = const_cast<HdRprim*>(renderIndex->GetRprim(meshId));
    HdDirtyBits meshBits = mesh->GetInitialDirtyBitsMask();
    mesh->InitRepr(&delegate, HdReprTokens->refined, &meshBits);
    mesh->Sync(
        &delegate, renderDelegate->GetRenderParam(),
        &meshBits, HdReprTokens->refined);

    RTCScene root = static_cast<HdEmbreeRenderParam*>(
        renderDelegate->GetRenderParam())->AcquireSceneForEdit();
    rtcCommitScene(root);
    const auto getFirstLevel = [&]() {
        RTCGeometry instance = rtcGetGeometry(root, 0);
        auto* instanceContext = instance
            ? static_cast<HdEmbreeInstanceContext*>(
                rtcGetGeometryUserData(instance))
            : nullptr;
        RTCGeometry prototype = instanceContext
            ? rtcGetGeometry(instanceContext->rootScene, 0)
            : nullptr;
        float const* levels = prototype
            ? static_cast<float const*>(rtcGetGeometryBufferData(
                prototype, RTC_BUFFER_TYPE_LEVEL, 0))
            : nullptr;
        return levels ? levels[0] : -1.0f;
    };

    HdRenderPassSharedPtr renderPass = renderDelegate->CreateRenderPass(
        renderIndex, HdRprimCollection());
    HdRenderPassStateSharedPtr state =
        renderDelegate->CreateRenderPassState();
    state->SetViewport(GfVec4d(0.0, 0.0, 50.0, 50.0));
    renderPass->Execute(state, TfTokenVector());
    const float withoutCamera = getFirstLevel();

    delegate.AddCamera(cameraId);
    delegate.UpdateCamera(
        cameraId, HdCameraTokens->horizontalAperture, VtValue(20.0f));
    delegate.UpdateCamera(
        cameraId, HdCameraTokens->verticalAperture, VtValue(20.0f));
    delegate.UpdateCamera(
        cameraId, HdCameraTokens->focalLength, VtValue(50.0f));
    delegate.UpdateCamera(
        cameraId, HdCameraTokens->clippingRange,
        VtValue(GfRange1f(0.1f, 100.0f)));
    HdSprim* camera = renderIndex->GetSprim(
        HdPrimTypeTokens->camera, cameraId);
    HdDirtyBits cameraBits = camera->GetInitialDirtyBitsMask();
    camera->Sync(
        &delegate, renderDelegate->GetRenderParam(), &cameraBits);
    state->SetCamera(static_cast<HdCamera const*>(camera));

    state->SetViewport(GfVec4d(0.0, 0.0, 100.0, 100.0));
    renderPass->Execute(state, TfTokenVector());
    const float initialCamera = getFirstLevel();

    state->SetViewport(GfVec4d(0.0, 0.0, 200.0, 200.0));
    renderPass->Execute(state, TfTokenVector());
    const float frozen = getFirstLevel();

    renderDelegate->SetRenderSetting(
        HdEmbreeRenderSettingsTokens->dynamicSubdvTesselation,
        VtValue(true));
    renderPass->Execute(state, TfTokenVector());
    const float dynamicEnabled = getFirstLevel();

    state->SetViewport(GfVec4d(0.0, 0.0, 300.0, 300.0));
    renderPass->Execute(state, TfTokenVector());
    const float dynamicUpdated = getFirstLevel();

    renderDelegate->SetRenderSetting(
        HdEmbreeRenderSettingsTokens->dynamicSubdvTesselation,
        VtValue(false));
    state->SetViewport(GfVec4d(0.0, 0.0, 400.0, 400.0));
    renderPass->Execute(state, TfTokenVector());
    const float refrozen = getFirstLevel();

    const bool valid =
        _Close(withoutCamera, 1.0f) &&
        initialCamera > withoutCamera &&
        _Close(frozen, initialCamera) &&
        dynamicEnabled > frozen &&
        dynamicUpdated > dynamicEnabled &&
        _Close(refrozen, dynamicUpdated);
    if (!valid) {
        std::printf(
            "    levels noCamera=%g initial=%g frozen=%g "
            "enabled=%g updated=%g refrozen=%g\n",
            withoutCamera, initialCamera, frozen,
            dynamicEnabled, dynamicUpdated, refrozen);
    }
    return valid;
}

bool
TestProductionAdaptiveLevelsForRefinedComplexities()
{
    _EmbreeTestContext context;
    auto* renderDelegate =
        dynamic_cast<HdEmbreeRenderDelegate*>(context.renderDelegate);
    HdRenderIndex* renderIndex = context.renderIndex.get();
    if (!renderDelegate || !renderIndex) {
        return false;
    }

    HdUnitTestDelegate delegate(
        renderIndex, SdfPath::AbsoluteRootPath());
    const SdfPath id("/adaptiveQuad");
    delegate.AddMesh(
        id, GfMatrix4f(1.0f),
        VtVec3fArray{
            GfVec3f(-1.0f, -0.5f, 0.0f),
            GfVec3f(1.0f, -0.5f, 0.0f),
            GfVec3f(1.0f, 0.5f, 0.0f),
            GfVec3f(-1.0f, 0.5f, 0.0f)},
        VtIntArray{4}, VtIntArray{0, 1, 2, 3});

    HdRprim* rprim = const_cast<HdRprim*>(renderIndex->GetRprim(id));
    RTCScene root = static_cast<HdEmbreeRenderParam*>(
        renderDelegate->GetRenderParam())->AcquireSceneForEdit();
    const float expected[] = {0.0f, 25.0f, 100.0f, 200.0f};
    for (int refineLevel = 1; refineLevel <= 3; ++refineLevel) {
        delegate.SetRefineLevel(id, refineLevel);
        HdDirtyBits bits = refineLevel == 1
            ? rprim->GetInitialDirtyBitsMask()
            : HdChangeTracker::DirtyDisplayStyle;
        if (refineLevel == 1) {
            rprim->InitRepr(&delegate, HdReprTokens->refined, &bits);
        }
        rprim->Sync(
            &delegate, renderDelegate->GetRenderParam(),
            &bits, HdReprTokens->refined);
        rtcCommitScene(root);
        if (!renderDelegate->UpdateAdaptiveSubdivision(
                GfMatrix4d(1.0), GfMatrix4d(1.0),
                GfRect2i(GfVec2i(0), 100, 100), false)) {
            return false;
        }
        rtcCommitScene(root);

        RTCGeometry instance = rtcGetGeometry(root, 0);
        auto* instanceContext = instance
            ? static_cast<HdEmbreeInstanceContext*>(
                rtcGetGeometryUserData(instance))
            : nullptr;
        RTCGeometry prototype = instanceContext
            ? rtcGetGeometry(instanceContext->rootScene, 0)
            : nullptr;
        auto* prototypeContext = prototype
            ? static_cast<HdEmbreePrototypeContext*>(
                rtcGetGeometryUserData(prototype))
            : nullptr;
        float const* levels = prototype
            ? static_cast<float const*>(rtcGetGeometryBufferData(
                prototype, RTC_BUFFER_TYPE_LEVEL, 0))
            : nullptr;
        if (!prototypeContext || !prototypeContext->refined || !levels ||
            !_Close(levels[0], expected[refineLevel])) {
            return false;
        }
    }
    return true;
}


bool
TestLowComplexityUsesTriangulatedControlCage()
{
    _EmbreeTestContext context;
    HdRenderDelegate* renderDelegate = context.renderDelegate;
    HdRenderIndex* renderIndex = context.renderIndex.get();
    if (!renderIndex) {
        return false;
    }

    bool valid = false;
    {
        HdUnitTestDelegate delegate(
            renderIndex, SdfPath::AbsoluteRootPath());
        const SdfPath id("/quad");
        delegate.AddMesh(
            id, GfMatrix4f(1.0f),
            VtVec3fArray{
                GfVec3f(0.0f, 0.0f, 0.0f),
                GfVec3f(1.0f, 0.0f, 0.0f),
                GfVec3f(1.0f, 1.0f, 0.0f),
                GfVec3f(0.0f, 1.0f, 0.0f)},
            VtIntArray{4}, VtIntArray{0, 1, 2, 3});
        delegate.SetRefineLevel(id, 0);
        delegate.AddPrimvar(
            id, TfToken("indexedCageValue"),
            VtValue(VtFloatArray{0.0f, 1.0f}),
            HdInterpolationFaceVarying, TfToken(),
            VtIntArray{0, 1, 1, 0});
        HdRprim* rprim = const_cast<HdRprim*>(renderIndex->GetRprim(id));
        HdDirtyBits bits = rprim->GetInitialDirtyBitsMask();
        rprim->InitRepr(&delegate, HdReprTokens->refined, &bits);
        rprim->Sync(
            &delegate, renderDelegate->GetRenderParam(),
            &bits, HdReprTokens->refined);

        RTCScene root = static_cast<HdEmbreeRenderParam*>(
            renderDelegate->GetRenderParam())->AcquireSceneForEdit();
        rtcCommitScene(root);
        RTCGeometry instance = rtcGetGeometry(root, 0);
        auto* instanceContext = instance
            ? static_cast<HdEmbreeInstanceContext*>(
                rtcGetGeometryUserData(instance))
            : nullptr;
        RTCGeometry prototype = instanceContext
            ? rtcGetGeometry(instanceContext->rootScene, 0)
            : nullptr;
        auto* prototypeContext = prototype
            ? static_cast<HdEmbreePrototypeContext*>(
                rtcGetGeometryUserData(prototype))
            : nullptr;
        const auto trace = [&](float x, float y) {
            RTCRayHit rayHit{};
            rayHit.ray.org_x = x;
            rayHit.ray.org_y = y;
            rayHit.ray.org_z = 2.0f;
            rayHit.ray.dir_z = -1.0f;
            rayHit.ray.tnear = 0.0f;
            rayHit.ray.tfar = std::numeric_limits<float>::infinity();
            rayHit.ray.mask = 0xffffffffu;
            rayHit.hit.geomID = RTC_INVALID_GEOMETRY_ID;
            rtcIntersect1(root, &rayHit);
            return rayHit;
        };
        // Opposite halves of the quad must hit distinct cage triangles. A
        // subdivision prototype would report the same coarse face for both.
        const RTCRayHit first = trace(0.75f, 0.25f);
        const RTCRayHit second = trace(0.25f, 0.75f);
        float firstValue = -1.0f;
        float secondValue = -1.0f;
        bool indexedSampled = false;
        if (prototypeContext) {
            auto indexedIt = prototypeContext->primvarMap.find(
                TfToken("indexedCageValue"));
            indexedSampled =
                indexedIt != prototypeContext->primvarMap.end() &&
                indexedIt->second->Sample(
                    first.hit.primID, first.hit.u, first.hit.v,
                    &firstValue) &&
                indexedIt->second->Sample(
                    second.hit.primID, second.hit.u, second.hit.v,
                    &secondValue);
        }
        valid = prototypeContext && !prototypeContext->refined &&
            first.hit.primID != RTC_INVALID_GEOMETRY_ID &&
            second.hit.primID != RTC_INVALID_GEOMETRY_ID &&
            first.hit.primID != second.hit.primID && indexedSampled &&
            _Close(firstValue, 0.75f) && _Close(secondValue, 0.25f);
    }

    return valid;
}


bool
TestUsdImagingExtractsSurfaceAndDisplacementTerminals()
{
    SdfLayerRefPtr layer = SdfLayer::CreateAnonymous(".usda");
    if (!layer || !layer->ImportFromString(R"USD(#usda 1.0

def Material "Material"
{
    token outputs:mtlx:surface.connect = </Material/Surface.outputs:out>
    token outputs:surface.connect = </Material/Surface.outputs:out>
    token outputs:mtlx:displacement.connect = </Material/Displacement.outputs:out>
    token outputs:displacement.connect = </Material/Displacement.outputs:out>

    def Shader "Surface"
    {
        uniform token info:id = "ND_open_pbr_surface_surfaceshader"
        token outputs:out
    }
    def Shader "Displacement"
    {
        uniform token info:id = "ND_displacement_float"
        float inputs:displacement = 0.25
        float inputs:scale = 1
        token outputs:out
    }
}

def Mesh "Mesh" (
    prepend apiSchemas = ["MaterialBindingAPI"]
)
{
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0, 1, 2, 3]
    rel material:binding = </Material>
    point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]
    uniform token subdivisionScheme = "catmullClark"
}
)USD")) {
        return false;
    }
    UsdStageRefPtr stage = UsdStage::Open(layer);
    if (!stage) {
        return false;
    }

    _EmbreeTestContext context;
    HdRenderIndex* renderIndex = context.renderIndex.get();
    if (!renderIndex) {
        return false;
    }

    bool valid = false;
    {
        UsdImagingDelegate delegate(
            renderIndex, SdfPath::AbsoluteRootPath());
        delegate.Populate(stage->GetPseudoRoot());
        delegate.SetTime(UsdTimeCode::Default());
        delegate.SyncAll(true);
        VtValue const resource =
            delegate.GetMaterialResource(SdfPath("/Material"));
        if (resource.IsHolding<HdMaterialNetworkMap>()) {
            HdMaterialNetwork2 const network = HdConvertToHdMaterialNetwork2(
                resource.UncheckedGet<HdMaterialNetworkMap>());
            valid = network.terminals.count(
                        HdMaterialTerminalTokens->surface) == 1 &&
                network.terminals.count(
                        HdMaterialTerminalTokens->displacement) == 1 &&
                network.nodes.count(SdfPath("/Material/Surface")) == 1 &&
                network.nodes.count(SdfPath("/Material/Displacement")) == 1;
        }
    }

    return valid;
}

} // namespace

int
main()
{
    struct Test { const char* name; bool (*fn)(); };
    const Test tests[] = {
        {"Subdivision.TestFaceVaryingRulesMapToEmbreeModes",
         &TestFaceVaryingRulesMapToEmbreeModes},
        {"Subdivision.TestComplexityTargetsExactPixelLengths",
         &TestComplexityTargetsExactPixelLengths},
        {"Subdivision.TestSharedEdgesUseSameMaximumLevel",
         &TestSharedEdgesUseSameMaximumLevel},
        {"Subdivision.TestLargestInstanceProjectionWins",
         &TestLargestInstanceProjectionWins},
        {"Subdivision.TestViewportChangeUpdatesLevels",
         &TestViewportChangeUpdatesLevels},
        {"Subdivision.TestLevelsClampToEmbreeRange",
         &TestLevelsClampToEmbreeRange},
        {"Subdivision.TestEdgeCrossingNearPlaneIsClippedBeforeProjection",
         &TestEdgeCrossingNearPlaneIsClippedBeforeProjection},
        {"Subdivision.TestEdgesAreClippedToViewportSides",
         &TestEdgesAreClippedToViewportSides},
        {"Subdivision.TestRealCallbackAddsRemovesAndReplacesDisplacement",
         &TestRealCallbackAddsRemovesAndReplacesDisplacement},
        {"Subdivision.TestMaterialSyncKeepsStableHandleAndReplacesDisplacementGraph",
         &TestMaterialSyncKeepsStableHandleAndReplacesDisplacementGraph},
        {"Subdivision.TestMaterialChangeForcesProductionDisplacementRecommit",
         &TestMaterialChangeForcesProductionDisplacementRecommit},
        {"Subdivision.TestSubdivisionPrimvarsUseHydraInterpolationModes",
         &TestSubdivisionPrimvarsUseHydraInterpolationModes},
        {"Subdivision.TestRenderPassRequiresCameraAndGatesDynamicTessellation",
         &TestRenderPassRequiresCameraAndGatesDynamicTessellation},
        {"Subdivision.TestProductionAdaptiveLevelsForRefinedComplexities",
         &TestProductionAdaptiveLevelsForRefinedComplexities},
        {"Subdivision.TestLowComplexityUsesTriangulatedControlCage",
         &TestLowComplexityUsesTriangulatedControlCage},
        {"Subdivision.TestUsdImagingExtractsSurfaceAndDisplacementTerminals",
         &TestUsdImagingExtractsSurfaceAndDisplacementTerminals},
    };

    int failed = 0;
    for (const Test& test : tests) {
        std::printf("  [RUN ] %s\n", test.name);
        if (test.fn()) {
            std::printf("  [PASS] %s\n", test.name);
        } else {
            std::printf("  [FAIL] %s\n", test.name);
            ++failed;
        }
    }
    std::printf("%zu/%zu tests passed.\n",
                std::size(tests) - failed, std::size(tests));
    return failed == 0 ? 0 : 1;
}
