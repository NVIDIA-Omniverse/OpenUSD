//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//

#include <delegate/renderDelegate.h>
#include <delegate/renderParam.h>
#include <renderer/geometry/context.h>
#include <renderer/geometry/wireframe.h>

#include "pxr/base/gf/matrix4f.h"
#include "pxr/imaging/hd/changeTracker.h"
#include "pxr/imaging/hd/renderIndex.h"
#include "pxr/imaging/hd/renderPass.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/hd/unitTestDelegate.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

bool
_IsClose(float actual, float expected, float tolerance = 1.0e-5f)
{
    if (std::abs(actual - expected) <= tolerance) {
        return true;
    }
    std::printf(
        "Expected %.8f, received %.8f\n",
        static_cast<double>(expected),
        static_cast<double>(actual));
    return false;
}

ty::SubdivWireframeTopology
_MakeTopology(
    std::vector<int> const& faceVertexCounts,
    std::vector<size_t> const& faceVertexOffsets,
    std::vector<float> const& edgeLevels)
{
    return {
        faceVertexCounts.data(),
        faceVertexCounts.size(),
        faceVertexOffsets.data(),
        faceVertexOffsets.size(),
        edgeLevels.data(),
        edgeLevels.size()};
}

bool
_TestTriangleEdgesAndWidth()
{
    const ty::WireframeSample edge{
        0.0f, 0.3f, 0.01f, 0.0f, 0.0f, 0.01f};
    if (!_IsClose(ty::ComputeTriangleWireframeOpacity(edge, 1.0f),
                  1.0f)) {
        return false;
    }

    const ty::WireframeSample interior{
        0.3f, 0.3f, 0.01f, 0.0f, 0.0f, 0.01f};
    if (ty::ComputeTriangleWireframeOpacity(interior, 1.0f) >
            1.0e-6f) {
        std::printf("Triangle interior was classified as an edge\n");
        return false;
    }

    const ty::WireframeSample onePixelFromEdge{
        0.01f, 0.3f, 0.01f, 0.0f, 0.0f, 0.01f};
    return
        _IsClose(
            ty::ComputeTriangleWireframeOpacity(
                onePixelFromEdge, 1.0f),
            0.0625f) &&
        _IsClose(
            ty::ComputeTriangleWireframeOpacity(
                onePixelFromEdge, 2.0f),
            0.5f);
}

bool
_TestWireframeDerivativeScaleRestoresPixelFootprint()
{
    return
        _IsClose(ty::ComputeWireframeDerivativeScale(0), 1.0f) &&
        _IsClose(ty::ComputeWireframeDerivativeScale(1), 1.0f) &&
        _IsClose(ty::ComputeWireframeDerivativeScale(16), 4.0f) &&
        _IsClose(ty::ComputeWireframeDerivativeScale(256), 16.0f);
}

bool
_TestTrianglePixelWidthIsProjectionInvariant()
{
    const ty::WireframeSample largeProjectedTriangle{
        0.1f, 0.3f, 0.1f, 0.0f, 0.0f, 0.1f};
    const ty::WireframeSample smallProjectedTriangle{
        0.01f, 0.3f, 0.01f, 0.0f, 0.0f, 0.01f};
    const float largeOpacity = ty::ComputeTriangleWireframeOpacity(
        largeProjectedTriangle, 1.0f);
    const float smallOpacity = ty::ComputeTriangleWireframeOpacity(
        smallProjectedTriangle, 1.0f);
    return
        _IsClose(largeOpacity, 0.0625f) &&
        _IsClose(smallOpacity, 0.0625f) &&
        _IsClose(largeOpacity, smallOpacity);
}

bool
_TestQuadDicingGrid()
{
    const std::vector<int> counts{4};
    const std::vector<size_t> offsets{0, 4};
    const std::vector<float> levels{4.0f, 4.0f, 4.0f, 4.0f};
    const ty::SubdivWireframeTopology topology =
        _MakeTopology(counts, offsets, levels);

    const ty::WireframeSample verticalGridLine{
        0.25f, 0.1f, 0.0025f, 0.0f, 0.0f, 0.0025f};
    if (!_IsClose(ty::ComputeSubdivisionWireframeOpacity(
            verticalGridLine, 0, topology, 1.0f), 1.0f)) {
        return false;
    }

    const ty::WireframeSample triangleDiagonal{
        0.125f, 0.125f, 0.0025f, 0.0f, 0.0f, 0.0025f};
    if (!_IsClose(ty::ComputeSubdivisionWireframeOpacity(
            triangleDiagonal, 0, topology, 1.0f), 1.0f)) {
        return false;
    }

    const ty::WireframeSample interior{
        0.075f, 0.1f, 0.0025f, 0.0f, 0.0f, 0.0025f};
    if (ty::ComputeSubdivisionWireframeOpacity(
            interior, 0, topology, 1.0f) > 1.0e-6f) {
        std::printf("Diced quad interior was classified as an edge\n");
        return false;
    }
    return true;
}

bool
_TestDenseQuadGridRetainsEveryEdge()
{
    const std::vector<int> counts{4};
    const std::vector<size_t> offsets{0, 4};
    const std::vector<float> levels{16.0f, 16.0f, 16.0f, 16.0f};
    const ty::SubdivWireframeTopology topology =
        _MakeTopology(counts, offsets, levels);

    const ty::WireframeSample firstEdge{
        4.0f / 16.0f, 2.4f / 16.0f,
        0.03f / 16.0f, 0.0f, 0.0f, 0.03f / 16.0f};
    const ty::WireframeSample adjacentEdge{
        2.0f / 16.0f, 2.4f / 16.0f,
        0.03f / 16.0f, 0.0f, 0.0f, 0.03f / 16.0f};

    return
        _IsClose(ty::ComputeSubdivisionWireframeOpacity(
            firstEdge, 0, topology, 1.0f), 1.0f) &&
        _IsClose(ty::ComputeSubdivisionWireframeOpacity(
            adjacentEdge, 0, topology, 1.0f), 1.0f);
}

bool
_TestSubpixelGridDoesNotDisappear()
{
    const std::vector<int> counts{4};
    const std::vector<size_t> offsets{0, 4};
    const std::vector<float> levels{16.0f, 16.0f, 16.0f, 16.0f};
    const ty::SubdivWireframeTopology topology =
        _MakeTopology(counts, offsets, levels);

    // Two diced cells per pixel cannot be individually resolved, but their
    // exact edge coverage must remain visible rather than being removed.
    const ty::WireframeSample betweenSubpixelEdges{
        0.5f / 16.0f, 0.25f / 16.0f,
        2.0f / 16.0f, 0.0f, 0.0f, 2.0f / 16.0f};
    return ty::ComputeSubdivisionWireframeOpacity(
        betweenSubpixelEdges, 0, topology, 1.0f) > 0.5f;
}

bool
_TestNonQuadSubpatchDecoding()
{
    const std::vector<int> counts{5};
    const std::vector<size_t> offsets{0, 5};
    const std::vector<float> levels{
        4.0f, 4.0f, 4.0f, 4.0f, 8.0f};
    const ty::SubdivWireframeTopology topology =
        _MakeTopology(counts, offsets, levels);

    // Face sub-patch 4 is encoded in UV tile (0,1). Its max adjacent edge
    // level is halved to four grid segments, making local u=0.25 a line.
    const ty::WireframeSample encodedGridLine{
        0.75f, 2.8f, 0.0025f, 0.0f, 0.0f, 0.0025f};
    return _IsClose(ty::ComputeSubdivisionWireframeOpacity(
        encodedGridLine, 0, topology, 1.0f), 1.0f);
}

bool
_TestInvalidTopologyIsRejected()
{
    const ty::WireframeSample sample{
        0.0f, 0.0f, 0.01f, 0.0f, 0.0f, 0.01f};
    return _IsClose(ty::ComputeSubdivisionWireframeOpacity(
        sample, 0, ty::SubdivWireframeTopology{}, 1.0f), 0.0f);
}

bool
_TestEdgeOnlyWireframeIsSolidBlack()
{
    const GfVec4f clearColor(0.2f, 0.4f, 0.8f, 0.0f);
    const GfVec4f fullCoverage =
        ty::CompositeEdgeOnlyWireframe(clearColor, 1.0f);
    if (fullCoverage != GfVec4f(0.0f, 0.0f, 0.0f, 1.0f)) {
        std::printf("Full edge-only coverage was not opaque black\n");
        return false;
    }

    const GfVec4f noCoverage =
        ty::CompositeEdgeOnlyWireframe(clearColor, 0.0f);
    if (noCoverage != clearColor) {
        std::printf("Zero edge-only coverage changed the clear color\n");
        return false;
    }

    const GfVec4f halfCoverage =
        ty::CompositeEdgeOnlyWireframe(clearColor, 0.5f);
    return
        _IsClose(halfCoverage[0], 0.1f) &&
        _IsClose(halfCoverage[1], 0.2f) &&
        _IsClose(halfCoverage[2], 0.4f) &&
        _IsClose(halfCoverage[3], 0.5f);
}

bool
_TestCollectionReprTransitionsDirtyMesh()
{
    HdEmbreeRenderDelegate renderDelegate;
    std::unique_ptr<HdRenderIndex> renderIndex(
        HdRenderIndex::New(&renderDelegate, HdDriverVector()));
    if (!renderIndex) {
        std::printf("Failed to create the render index\n");
        return false;
    }

    bool passed = true;
    {
        HdUnitTestDelegate sceneDelegate(
            renderIndex.get(), SdfPath::AbsoluteRootPath());
        const SdfPath meshId("/mesh");
        sceneDelegate.AddCube(meshId, GfMatrix4f(1.0f));
        sceneDelegate.SetRefineLevel(meshId, 2);

        HdChangeTracker& tracker = renderIndex->GetChangeTracker();
        const auto makeCollection = [](TfToken const& reprToken,
                                       bool forcedRepr = false) {
            return HdRprimCollection(
                HdTokens->geometry, HdReprSelector(reprToken), forcedRepr);
        };
        HdRenderPassSharedPtr renderPass = renderDelegate.CreateRenderPass(
            renderIndex.get(), makeCollection(HdReprTokens->refined));
        auto* const renderParam = static_cast<HdEmbreeRenderParam*>(
            renderDelegate.GetRenderParam());
        RTCScene const rootScene = renderParam->AcquireSceneForEdit();

        const auto getWireframeMode =
            [rootScene](ty::WireframeMode* wireframeMode) {
                RTCGeometry const instance = rtcGetGeometry(rootScene, 0);
                auto* const instanceContext = instance
                    ? static_cast<ty::InstanceContext*>(
                        rtcGetGeometryUserData(instance))
                    : nullptr;
                RTCGeometry const prototype = instanceContext
                    ? rtcGetGeometry(instanceContext->rootScene, 0)
                    : nullptr;
                auto* const prototypeContext = prototype
                    ? static_cast<ty::PrototypeContext*>(
                        rtcGetGeometryUserData(prototype))
                    : nullptr;
                if (!prototypeContext) {
                    return false;
                }
                *wireframeMode = prototypeContext->wireframeMode;
                return true;
            };

        const auto syncAndVerify =
            [&](TfToken const& reprToken,
                ty::WireframeMode expectedMode,
                bool forcedRepr = false) {
                renderPass->SetRprimCollection(
                    makeCollection(reprToken, forcedRepr));
                renderPass->Sync();

                HdTaskSharedPtrVector tasks;
                HdTaskContext taskContext;
                renderIndex->SyncAll(&tasks, &taskContext);

                ty::WireframeMode actualMode =
                    ty::WireframeMode::disabled;
                if (!getWireframeMode(&actualMode)) {
                    std::printf(
                        "Collection transition to %s did not create an "
                        "Embree prototype context\n",
                        reprToken.GetText());
                    return false;
                }
                if (actualMode != expectedMode) {
                    std::printf(
                        "Collection transition to %s retained wireframe "
                        "mode %d instead of %d\n",
                        reprToken.GetText(),
                        static_cast<int>(actualMode),
                        static_cast<int>(expectedMode));
                    return false;
                }
                return true;
            };

        // Exercise both first-use and return transitions. The latter did not
        // rebuild Hydra's dirty list before the render-pass fix.
        passed =
            syncAndVerify(
                HdReprTokens->refined,
                ty::WireframeMode::disabled) &&
            syncAndVerify(
                HdReprTokens->refinedWireOnSurf,
                ty::WireframeMode::edgeOnSurface) &&
            syncAndVerify(
                HdReprTokens->refined,
                ty::WireframeMode::disabled) &&
            syncAndVerify(
                HdReprTokens->refinedWire,
                ty::WireframeMode::edgeOnly) &&
            syncAndVerify(
                HdReprTokens->refinedWireOnSurf,
                ty::WireframeMode::edgeOnSurface) &&
            syncAndVerify(
                HdReprTokens->refined,
                ty::WireframeMode::disabled);

        tracker.MarkRprimClean(meshId);
        renderPass->SetRprimCollection(
            makeCollection(HdReprTokens->refined));
        if (!HdChangeTracker::IsClean(
                tracker.GetRprimDirtyBits(meshId))) {
            std::printf(
                "An unchanged collection redundantly dirtied the mesh\n");
            passed = false;
        }

        tracker.MarkRprimClean(meshId);
        renderPass->SetRprimCollection(
            makeCollection(HdReprTokens->refined, true));
        if ((tracker.GetRprimDirtyBits(meshId) &
             HdChangeTracker::DirtyRepr) == 0) {
            std::printf("Changing forced repr state did not dirty the mesh\n");
            passed = false;
        }
    }

    renderIndex.reset();
    return passed;
}

} // anonymous namespace

int
main()
{
    const bool passed =
        _TestTriangleEdgesAndWidth() &&
        _TestWireframeDerivativeScaleRestoresPixelFootprint() &&
        _TestTrianglePixelWidthIsProjectionInvariant() &&
        _TestQuadDicingGrid() &&
        _TestDenseQuadGridRetainsEveryEdge() &&
        _TestSubpixelGridDoesNotDisappear() &&
        _TestNonQuadSubpatchDecoding() &&
        _TestInvalidTopologyIsRejected() &&
        _TestEdgeOnlyWireframeIsSolidBlack() &&
        _TestCollectionReprTransitionsDirtyMesh();
    std::printf("testHdEmbreeWireframe: %s\n", passed ? "PASS" : "FAIL");
    return passed ? 0 : 1;
}
