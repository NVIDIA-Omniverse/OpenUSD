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
#include "pxr/imaging/plugin/hdEmbree/renderer/geometry/displacementEvaluation.h"
#include "pxr/imaging/plugin/hdEmbree/renderer/geometry/meshSamplers.h"
#include "pxr/imaging/plugin/hdEmbree/renderer/geometry/primvarSampling.h"
#include "pxr/imaging/plugin/hdEmbree/renderer/materials/MaterialXCpp/graph.h"
#include "pxr/imaging/plugin/hdEmbree/renderer/materials/MaterialXCpp/textureSystem.h"
#include "pxr/imaging/plugin/hdEmbree/renderer/materials/materialEvalContext.h"
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

class _DisplayStyleDelegate : public HdUnitTestDelegate
{
public:
    using HdUnitTestDelegate::HdUnitTestDelegate;

    HdDisplayStyle GetDisplayStyle(SdfPath const& id) override
    {
        HdDisplayStyle result = HdUnitTestDelegate::GetDisplayStyle(id);
        result.displacementEnabled = displacementEnabled;
        return result;
    }

    void SetDisplacementEnabled(SdfPath const& id, bool enabled)
    {
        displacementEnabled = enabled;
        MarkRprimDirty(id, HdChangeTracker::DirtyDisplayStyle);
    }

    bool displacementEnabled = true;
};

class _PointsOverrideDelegate : public HdUnitTestDelegate
{
public:
    using HdUnitTestDelegate::HdUnitTestDelegate;

    VtValue Get(SdfPath const& id, TfToken const& key) override
    {
        if (id == meshId && key == HdTokens->points) {
            return VtValue(points);
        }
        return HdUnitTestDelegate::Get(id, key);
    }

    void SetPoints(SdfPath const& id, VtVec3fArray const& value)
    {
        meshId = id;
        points = value;
        MarkRprimDirty(id, HdChangeTracker::DirtyPoints);
    }

    SdfPath meshId;
    VtVec3fArray points;
};

bool
_Close(float a, float b, float epsilon = 1.0e-3f)
{
    return std::abs(a - b) <= epsilon;
}

bool
_Close(
    GfVec3f const& a, GfVec3f const& b, float epsilon = 1.0e-3f)
{
    return (a - b).GetLength() <= epsilon;
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

HdEmbreeDisplacedPositionProbe
_MakeBentQuadProbe(float uBend, float vBend)
{
    return [uBend, vBend](
               unsigned int primID,
               float u,
               float v,
               GfVec3f* position) {
        if (primID != 0 || !position) {
            return false;
        }
        *position = GfVec3f(
            -0.125f + 0.25f * u,
            -0.125f + 0.25f * v,
            0.0f);
        if (u == 0.5f) {
            (*position)[1] += uBend;
        }
        if (v == 0.5f) {
            (*position)[0] += vBend;
        }
        return true;
    };
}

std::vector<float>
_ComputeDisplacementAwareQuad(
    HdEmbreeDisplacedPositionProbe const& probe,
    int refineLevel = 1,
    std::vector<GfMatrix4f> const& transforms = {GfMatrix4f(1.0f)})
{
    return HdEmbreeComputeAdaptiveSubdivisionLevels(
        VtVec3fArray{
            GfVec3f(-0.125f, -0.125f, 0.0f),
            GfVec3f(0.125f, -0.125f, 0.0f),
            GfVec3f(0.125f, 0.125f, 0.0f),
            GfVec3f(-0.125f, 0.125f, 0.0f)},
        VtIntArray{4}, VtIntArray{0, 1, 2, 3}, transforms,
        GfMatrix4d(1.0), GfMatrix4d(1.0),
        GfRect2i(GfVec2i(0), 100, 100), refineLevel, probe);
}

bool
TestDisplacementProbeRaisesOnlyCurvedDirection()
{
    const std::vector<float> flat =
        _ComputeDisplacementAwareQuad(_MakeBentQuadProbe(0.0f, 0.0f));
    const std::vector<float> uBent =
        _ComputeDisplacementAwareQuad(_MakeBentQuadProbe(0.02f, 0.0f));
    const std::vector<float> vBent =
        _ComputeDisplacementAwareQuad(_MakeBentQuadProbe(0.0f, 0.02f));
    const std::vector<float> repeated =
        _ComputeDisplacementAwareQuad(_MakeBentQuadProbe(0.02f, 0.0f));
    return flat == std::vector<float>({4.0f, 4.0f, 4.0f, 4.0f}) &&
        uBent == std::vector<float>({8.0f, 4.0f, 8.0f, 4.0f}) &&
        vBent == std::vector<float>({4.0f, 8.0f, 4.0f, 8.0f}) &&
        repeated == uBent;
}

bool
TestDisplacementProbeUsesComplexityAndLargestInstance()
{
    constexpr float bend = 0.007f;
    const HdEmbreeDisplacedPositionProbe probe =
        _MakeBentQuadProbe(bend, 0.0f);
    const std::vector<float> medium =
        _ComputeDisplacementAwareQuad(probe, 1);
    const std::vector<float> high =
        _ComputeDisplacementAwareQuad(probe, 2);

    GfMatrix4f twice(1.0f);
    twice.SetScale(GfVec3f(2.0f));
    const std::vector<float> largestInstance =
        _ComputeDisplacementAwareQuad(
            probe, 1, {GfMatrix4f(1.0f), twice});
    const bool valid = medium ==
            std::vector<float>({4.0f, 4.0f, 4.0f, 4.0f}) &&
        high == std::vector<float>({26.0f, 13.0f, 26.0f, 13.0f}) &&
        largestInstance ==
            std::vector<float>({14.0f, 7.0f, 14.0f, 7.0f});
    if (!valid) {
        const auto printLevels = [](char const* label,
                                    std::vector<float> const& levels) {
            std::printf("    %s:", label);
            for (const float level : levels) {
                std::printf(" %g", level);
            }
            std::printf("\n");
        };
        printLevels("medium", medium);
        printLevels("high", high);
        printLevels("largest", largestInstance);
    }
    return valid;
}

bool
TestDisplacementProbeFailureAndNonQuadKeepBaseline()
{
    const HdEmbreeDisplacedPositionProbe failingProbe =
        [](unsigned int, float u, float v, GfVec3f* position) {
            if (u == 0.5f && v == 0.5f) {
                return false;
            }
            if (position) {
                *position = GfVec3f(u, v, 0.0f);
            }
            return position != nullptr;
        };
    const std::vector<float> failed =
        _ComputeDisplacementAwareQuad(failingProbe);

    int nonQuadProbeCalls = 0;
    const std::vector<float> triangle =
        HdEmbreeComputeAdaptiveSubdivisionLevels(
            VtVec3fArray{
                GfVec3f(-0.1f, -0.1f, 0.0f),
                GfVec3f(0.1f, -0.1f, 0.0f),
                GfVec3f(0.0f, 0.1f, 0.0f)},
            VtIntArray{3}, VtIntArray{0, 1, 2},
            {GfMatrix4f(1.0f)}, GfMatrix4d(1.0), GfMatrix4d(1.0),
            GfRect2i(GfVec2i(0), 100, 100), 1,
            [&nonQuadProbeCalls](
                unsigned int, float, float, GfVec3f*) {
                ++nonQuadProbeCalls;
                return false;
            });
    return failed == std::vector<float>({4.0f, 4.0f, 4.0f, 4.0f}) &&
        triangle.size() == 3 && nonQuadProbeCalls == 0;
}

bool
TestDisplacementProbePreservesSharedEdgeBalance()
{
    const std::vector<float> levels =
        HdEmbreeComputeAdaptiveSubdivisionLevels(
            VtVec3fArray{
                GfVec3f(-0.25f, -0.125f, 0.0f),
                GfVec3f(0.0f, -0.125f, 0.0f),
                GfVec3f(0.0f, 0.125f, 0.0f),
                GfVec3f(-0.25f, 0.125f, 0.0f),
                GfVec3f(0.25f, -0.125f, 0.0f),
                GfVec3f(0.25f, 0.125f, 0.0f)},
            VtIntArray{4, 4},
            VtIntArray{0, 1, 2, 3, 2, 1, 4, 5},
            {GfMatrix4f(1.0f)}, GfMatrix4d(1.0), GfMatrix4d(1.0),
            GfRect2i(GfVec2i(0), 100, 100), 1,
            [](unsigned int primID,
               float u,
               float v,
               GfVec3f* position) {
                if (!position || primID > 1) {
                    return false;
                }
                if (primID == 0) {
                    *position = GfVec3f(
                        -0.25f + 0.25f * u,
                        -0.125f + 0.25f * v,
                        0.0f);
                    if (v == 0.5f) {
                        (*position)[0] += 0.02f;
                    }
                } else {
                    *position = GfVec3f(
                        0.25f * v,
                        0.125f - 0.25f * u,
                        0.0f);
                }
                return true;
            });
    return levels == std::vector<float>({
        4.0f, 8.0f, 4.0f, 8.0f,
        8.0f, 4.0f, 8.0f, 4.0f});
}

bool
TestDisplacementBoostPropagatesAcrossQuadStrip()
{
    const std::vector<float> levels =
        HdEmbreeComputeAdaptiveSubdivisionLevels(
            VtVec3fArray{
                GfVec3f(-0.375f, -0.125f, 0.0f),
                GfVec3f(-0.125f, -0.125f, 0.0f),
                GfVec3f(0.125f, -0.125f, 0.0f),
                GfVec3f(0.375f, -0.125f, 0.0f),
                GfVec3f(-0.375f, 0.125f, 0.0f),
                GfVec3f(-0.125f, 0.125f, 0.0f),
                GfVec3f(0.125f, 0.125f, 0.0f),
                GfVec3f(0.375f, 0.125f, 0.0f)},
            VtIntArray{4, 4, 4},
            VtIntArray{
                0, 1, 5, 4,
                1, 2, 6, 5,
                2, 3, 7, 6},
            {GfMatrix4f(1.0f)}, GfMatrix4d(1.0), GfMatrix4d(1.0),
            GfRect2i(GfVec2i(0), 100, 100), 1,
            [](unsigned int primID,
               float u,
               float v,
               GfVec3f* position) {
                if (!position || primID > 2) {
                    return false;
                }
                *position = GfVec3f(
                    -0.375f +
                        0.25f * (static_cast<float>(primID) + u),
                    -0.125f + 0.25f * v,
                    0.0f);
                if (primID == 0 && v == 0.5f) {
                    (*position)[0] += 0.02f;
                }
                return true;
            });
    return levels == std::vector<float>({
        4.0f, 8.0f, 4.0f, 8.0f,
        4.0f, 8.0f, 4.0f, 8.0f,
        4.0f, 8.0f, 4.0f, 8.0f});
}

bool
TestDisplacementBoostPreservesBaselineOppositeRatio()
{
    const VtVec3fArray points{
        GfVec3f(-0.125f, -0.125f, 0.0f),
        GfVec3f(0.125f, -0.125f, 0.0f),
        GfVec3f(0.25f, 0.125f, 0.0f),
        GfVec3f(-0.25f, 0.125f, 0.0f)};
    const auto compute = [&points](
        HdEmbreeDisplacedPositionProbe const& probe) {
        return HdEmbreeComputeAdaptiveSubdivisionLevels(
            points, VtIntArray{4}, VtIntArray{0, 1, 2, 3},
            {GfMatrix4f(1.0f)}, GfMatrix4d(1.0), GfMatrix4d(1.0),
            GfRect2i(GfVec2i(0), 100, 100), 1, probe);
    };

    const std::vector<float> baseline = compute({});
    const std::vector<float> boosted = compute(
        [](unsigned int primID,
           float u,
           float v,
           GfVec3f* position) {
            if (!position || primID != 0) {
                return false;
            }
            const float left = -0.125f - 0.125f * v;
            const float right = 0.125f + 0.125f * v;
            *position = GfVec3f(
                left + (right - left) * u,
                -0.125f + 0.25f * v,
                0.0f);
            if (u == 0.5f) {
                (*position)[1] += 0.02f;
            }
            return true;
        });
    return baseline == std::vector<float>({4.0f, 4.0f, 7.0f, 4.0f}) &&
        boosted == std::vector<float>({8.0f, 4.0f, 14.0f, 4.0f});
}

bool
TestDisplacementBoostTouchesOnlySharedNonQuadEdge()
{
    const std::vector<float> levels =
        HdEmbreeComputeAdaptiveSubdivisionLevels(
            VtVec3fArray{
                GfVec3f(-0.125f, -0.125f, 0.0f),
                GfVec3f(0.125f, -0.125f, 0.0f),
                GfVec3f(0.125f, 0.125f, 0.0f),
                GfVec3f(-0.125f, 0.125f, 0.0f),
                GfVec3f(0.375f, 0.0f, 0.0f)},
            VtIntArray{4, 3},
            VtIntArray{0, 1, 2, 3, 2, 1, 4},
            {GfMatrix4f(1.0f)}, GfMatrix4d(1.0), GfMatrix4d(1.0),
            GfRect2i(GfVec2i(0), 100, 100), 1,
            _MakeBentQuadProbe(0.0f, 0.02f));
    return levels == std::vector<float>({
        4.0f, 8.0f, 4.0f, 8.0f,
        8.0f, 4.0f, 4.0f});
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
TestBalancedSubdivisionLevelsUseSharedMaximum()
{
    // Face 0 edge 1 and face 1 edge 0 are the same reversed coarse edge.
    const std::vector<float> levels =
        HdEmbreeBalanceSubdivisionLevels(
            VtIntArray{4, 4},
            VtIntArray{0, 1, 2, 3, 2, 1, 4, 5},
            std::vector<float>{
                4.0f, 8.0f, 4.0f, 4.0f,
                20.0f, 4.0f, 4.0f, 4.0f});
    return levels.size() == 8 &&
        levels[1] == 20.0f && levels[4] == 20.0f;
}

bool
TestBalancedSubdivisionLevelsReachOppositeTwoToOneFixedPoint()
{
    // Each face's edge 2 is the next face's edge 0. The level 64 must decay
    // through all three quads, which requires alternating opposite balancing
    // and shared-edge consolidation more than once.
    const VtIntArray counts{4, 4, 4};
    const VtIntArray indices{
        0, 1, 2, 3,
        3, 2, 4, 5,
        5, 4, 6, 7};
    const std::vector<float> levels =
        HdEmbreeBalanceSubdivisionLevels(
            counts, indices,
            std::vector<float>{
                64.0f, 4.0f, 4.0f, 4.0f,
                4.0f, 4.0f, 4.0f, 4.0f,
                4.0f, 4.0f, 4.0f, 4.0f});
    return levels.size() == 12 &&
        levels[0] == 64.0f && levels[2] == 32.0f &&
        levels[4] == 32.0f && levels[6] == 16.0f &&
        levels[8] == 16.0f && levels[10] == 8.0f;
}

bool
TestBalancedSubdivisionLevelsAreRaiseOnlyAndIdempotent()
{
    const VtIntArray counts{4, 4};
    const VtIntArray indices{0, 1, 2, 3, 3, 2, 4, 5};
    const std::vector<float> candidates{
        31.0f, 7.0f, 4.0f, 5.0f,
        9.0f, 6.0f, 4.0f, 8.0f};
    const std::vector<float> balanced =
        HdEmbreeBalanceSubdivisionLevels(
            counts, indices, candidates);
    if (balanced.size() != candidates.size()) {
        return false;
    }
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (balanced[i] < candidates[i]) {
            return false;
        }
    }
    return HdEmbreeBalanceSubdivisionLevels(
        counts, indices, balanced) == balanced;
}

bool
TestBalancedSubdivisionLevelsStayInRangeAndRejectInvalidInputs()
{
    const VtIntArray counts{4};
    const VtIntArray indices{0, 1, 2, 3};
    const std::vector<float> maximum =
        HdEmbreeBalanceSubdivisionLevels(
            counts, indices,
            std::vector<float>{4096.0f, 1.0f, 1.0f, 1.0f});
    const float nan = std::numeric_limits<float>::quiet_NaN();
    return maximum ==
            std::vector<float>({4096.0f, 1.0f, 2048.0f, 1.0f}) &&
        HdEmbreeBalanceSubdivisionLevels(
            counts, indices,
            std::vector<float>{0.0f, 1.0f, 1.0f, 1.0f}).empty() &&
        HdEmbreeBalanceSubdivisionLevels(
            counts, indices,
            std::vector<float>{4097.0f, 1.0f, 1.0f, 1.0f}).empty() &&
        HdEmbreeBalanceSubdivisionLevels(
            counts, indices,
            std::vector<float>{nan, 1.0f, 1.0f, 1.0f}).empty() &&
        HdEmbreeBalanceSubdivisionLevels(
            counts, indices,
            std::vector<float>{1.0f, 1.0f, 1.0f}).empty() &&
        HdEmbreeBalanceSubdivisionLevels(
            counts, VtIntArray{0, -1, 2, 3},
            std::vector<float>{1.0f, 1.0f, 1.0f, 1.0f}).empty();
}

bool
TestBalancedSubdivisionLevelsSkipNonQuadFaces()
{
    // The pentagon shares its first edge with quad edge 1. Shared-edge max
    // still applies, but the other pentagon edges must not be raised merely
    // because a notion of "opposite" is ambiguous for non-quads.
    const std::vector<float> levels =
        HdEmbreeBalanceSubdivisionLevels(
            VtIntArray{4, 5},
            VtIntArray{0, 1, 2, 3, 2, 1, 4, 5, 6},
            std::vector<float>{
                32.0f, 8.0f, 4.0f, 4.0f,
                20.0f, 4.0f, 4.0f, 4.0f, 4.0f});
    return levels == std::vector<float>({
        32.0f, 20.0f, 16.0f, 10.0f,
        20.0f, 4.0f, 4.0f, 4.0f, 4.0f});
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
        minimum[0] == 4.0f && maximum[0] == 4096.0f;
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
            // Avoid an exact-in-real-arithmetic integer before ceil: 1.1 is
            // not exactly representable and would make the expected result
            // depend on floating-point rounding at the integer boundary.
            GfVec3f(-0.01f, 0.0f, 0.0f),
            GfVec3f(0.0f, 0.5f, 0.0f)},
        VtIntArray{3}, VtIntArray{0, 1, 2},
        {GfMatrix4f(1.0f)}, 2);
    return fullyOutside.size() == 3 && fullyOutside[0] == 4.0f &&
        partlyOutside.size() == 3 && partlyOutside[0] == 55.0f;
}

bool
TestAdaptiveSubdivisionUsesGuardBandAndRequiredMinimum()
{
    const std::vector<GfMatrix4f> transforms{GfMatrix4f(1.0f)};
    // This edge is wholly outside the visible x=1 plane but inside the 10%
    // guard. Its 4.5-pixel projected length rounds up to 5, proving that it
    // was measured rather than replaced by the minimum.
    const std::vector<float> guardedX = _Compute(
        VtVec3fArray{
            GfVec3f(1.005f, 0.0f, 0.0f),
            GfVec3f(1.095f, 0.0f, 0.0f),
            GfVec3f(1.05f, 0.05f, 0.0f)},
        VtIntArray{3}, VtIntArray{0, 1, 2}, transforms, 2);
    const std::vector<float> guardedY = _Compute(
        VtVec3fArray{
            GfVec3f(0.0f, 1.005f, 0.0f),
            GfVec3f(0.0f, 1.095f, 0.0f),
            GfVec3f(0.05f, 1.05f, 0.0f)},
        VtIntArray{3}, VtIntArray{0, 1, 2}, transforms, 2);
    if (guardedX.size() != 3 || guardedX[0] != 5.0f ||
        guardedY.size() != 3 || guardedY[0] != 5.0f) {
        return false;
    }

    const VtVec3fArray beyondGuardPoints{
        GfVec3f(1.2f, 0.0f, 0.0f),
        GfVec3f(1.4f, 0.0f, 0.0f),
        GfVec3f(1.3f, 0.1f, 0.0f)};
    for (int refineLevel = 1; refineLevel <= 3; ++refineLevel) {
        const std::vector<float> required = _Compute(
            beyondGuardPoints,
            VtIntArray{3}, VtIntArray{0, 1, 2}, transforms, refineLevel);
        if (required != std::vector<float>(3, 4.0f)) {
            return false;
        }
    }
    return true;
}

bool
TestAdaptiveSubdivisionDoesNotGuardDepthPlanes()
{
    const auto computeOutsideDepth = [](const float z) {
        return _Compute(
            VtVec3fArray{
                GfVec3f(-1.0f, 0.0f, z),
                GfVec3f(1.0f, 0.0f, z),
                GfVec3f(0.0f, 0.5f, z)},
            VtIntArray{3}, VtIntArray{0, 1, 2},
            {GfMatrix4f(1.0f)}, 2);
    };
    // A Z guard analogous to the X/Y guard would measure these long edges.
    // Keeping both at the floor proves the ordinary near/far planes remain.
    const std::vector<float> beyondFar = computeOutsideDepth(1.05f);
    const std::vector<float> beyondNear = computeOutsideDepth(-1.05f);
    return beyondFar == std::vector<float>(3, 4.0f) &&
        beyondNear == std::vector<float>(3, 4.0f);
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

std::unique_ptr<EvalGraph>
_CompileTextureFrameTimeDisplacement()
{
    MaterialGraph network;

    GraphNode image;
    image.nodeTypeId = "ND_image_float";
    image.parameters["file"] = Value(std::string("memory:test.exr"));
    // A deliberately different fallback proves callback evaluation reaches
    // the texture backend instead of using this authored default.
    image.parameters["default"] = Value(-0.5f);
    network.nodes["/Material/Image"] = image;

    GraphNode frame;
    frame.nodeTypeId = "ND_frame_float";
    network.nodes["/Material/Frame"] = frame;

    GraphNode time;
    time.nodeTypeId = "ND_time_float";
    network.nodes["/Material/Time"] = time;

    GraphNode imagePlusFrame;
    imagePlusFrame.nodeTypeId = "ND_add_float";
    imagePlusFrame.inputConnections["in1"] =
        {{"/Material/Image", "out"}};
    imagePlusFrame.inputConnections["in2"] =
        {{"/Material/Frame", "out"}};
    network.nodes["/Material/ImagePlusFrame"] = imagePlusFrame;

    GraphNode sum;
    sum.nodeTypeId = "ND_add_float";
    sum.inputConnections["in1"] =
        {{"/Material/ImagePlusFrame", "out"}};
    sum.inputConnections["in2"] =
        {{"/Material/Time", "out"}};
    network.nodes["/Material/Sum"] = sum;

    GraphNode terminal;
    terminal.nodeTypeId = "ND_displacement_float";
    terminal.parameters["scale"] = Value(1.0f);
    terminal.inputConnections["displacement"] =
        {{"/Material/Sum", "out"}};
    network.nodes["/Material/Displacement"] = terminal;
    network.terminals["displacement"] =
        {"/Material/Displacement", "out"};
    return EvalGraph::Compile(network, "displacement");
}

std::unique_ptr<EvalGraph>
_CompileQuadraticTextureDisplacement()
{
    MaterialGraph network;

    // The test texture backend returns st.x. Combine its square with st.y^2 to
    // produce analytic, non-zero derivatives in both parameter directions
    // while keeping one texture-system call per graph evaluation.
    GraphNode image;
    image.nodeTypeId = "ND_image_float";
    image.parameters["file"] = Value(std::string("memory:test.exr"));
    network.nodes["/Material/Image"] = image;

    GraphNode squareU;
    squareU.nodeTypeId = "ND_multiply_float";
    squareU.inputConnections["in1"] = {{"/Material/Image", "out"}};
    squareU.inputConnections["in2"] = {{"/Material/Image", "out"}};
    network.nodes["/Material/SquareU"] = squareU;

    GraphNode texcoord;
    texcoord.nodeTypeId = "ND_texcoord_vector2";
    network.nodes["/Material/Texcoord"] = texcoord;

    GraphNode extractV;
    extractV.nodeTypeId = "ND_extract_vector2";
    extractV.parameters["index"] = Value(1);
    extractV.inputConnections["in"] = {{"/Material/Texcoord", "out"}};
    network.nodes["/Material/ExtractV"] = extractV;

    GraphNode squareV;
    squareV.nodeTypeId = "ND_multiply_float";
    squareV.inputConnections["in1"] = {{"/Material/ExtractV", "out"}};
    squareV.inputConnections["in2"] = {{"/Material/ExtractV", "out"}};
    network.nodes["/Material/SquareV"] = squareV;

    GraphNode sum;
    sum.nodeTypeId = "ND_add_float";
    sum.inputConnections["in1"] = {{"/Material/SquareU", "out"}};
    sum.inputConnections["in2"] = {{"/Material/SquareV", "out"}};
    network.nodes["/Material/Sum"] = sum;

    GraphNode terminal;
    terminal.nodeTypeId = "ND_displacement_float";
    terminal.parameters["scale"] = Value(1.0f);
    terminal.inputConnections["displacement"] =
        {{"/Material/Sum", "out"}};
    network.nodes["/Material/Displacement"] = terminal;
    network.terminals["displacement"] =
        {"/Material/Displacement", "out"};
    return EvalGraph::Compile(network, "displacement");
}

class _ThreadSafeTexcoordTextureSystem final : public TextureSystem
{
public:
    void Reset(float expectedFrame) noexcept
    {
        _expectedFrame.store(expectedFrame, std::memory_order_relaxed);
        _lastFrame.store(0.0f, std::memory_order_relaxed);
        _callCount.store(0, std::memory_order_relaxed);
        _allFramesMatched.store(true, std::memory_order_relaxed);
    }

    Texture2DResult Sample2D(
        Texture2DRequest const& request) const override
    {
        _callCount.fetch_add(1, std::memory_order_relaxed);
        _lastFrame.store(request.frame, std::memory_order_relaxed);
        if (!_Close(
                request.frame,
                _expectedFrame.load(std::memory_order_relaxed))) {
            _allFramesMatched.store(false, std::memory_order_relaxed);
        }

        Texture2DResult result;
        result.value = Vec4f(request.st[0], 0.0f, 0.0f, 0.0f);
        result.status = TextureSampleStatus::Ok;
        return result;
    }

    int GetCallCount() const noexcept
    {
        return _callCount.load(std::memory_order_relaxed);
    }

    float GetLastFrame() const noexcept
    {
        return _lastFrame.load(std::memory_order_relaxed);
    }

    bool AllFramesMatched() const noexcept
    {
        return _allFramesMatched.load(std::memory_order_relaxed);
    }

private:
    std::atomic<float> _expectedFrame{0.0f};
    mutable std::atomic<float> _lastFrame{0.0f};
    mutable std::atomic<int> _callCount{0};
    mutable std::atomic<bool> _allFramesMatched{true};
};

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

RTCRayHit
_TraceWithLimit(
    RTCScene scene, float x, float y, float originZ, float maxDistance)
{
    RTCRayHit rayHit{};
    rayHit.ray.org_x = x;
    rayHit.ray.org_y = y;
    rayHit.ray.org_z = originZ;
    rayHit.ray.dir_z = -1.0f;
    rayHit.ray.tnear = 0.0f;
    rayHit.ray.tfar = maxDistance;
    rayHit.ray.mask = 0xffffffffu;
    rayHit.hit.geomID = RTC_INVALID_GEOMETRY_ID;
    rtcIntersect1(scene, &rayHit);
    return rayHit;
}

RTCGeometry
_GetPrototypeGeometry(RTCScene root, unsigned int instanceId = 0)
{
    RTCGeometry instance = rtcGetGeometry(root, instanceId);
    auto* instanceContext = instance
        ? static_cast<HdEmbreeInstanceContext*>(
            rtcGetGeometryUserData(instance))
        : nullptr;
    return instanceContext
        ? rtcGetGeometry(instanceContext->rootScene, 0)
        : nullptr;
}

bool
_SetPaddedVertexBuffer(
    RTCGeometry geometry, VtVec3fArray const& points)
{
    constexpr size_t vertexStride = 4 * sizeof(float);
    float* const vertices = static_cast<float*>(rtcSetNewGeometryBuffer(
        geometry, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3,
        vertexStride, points.size()));
    if (!vertices && !points.empty()) {
        return false;
    }
    for (size_t i = 0; i < points.size(); ++i) {
        vertices[4 * i + 0] = points[i][0];
        vertices[4 * i + 1] = points[i][1];
        vertices[4 * i + 2] = points[i][2];
        vertices[4 * i + 3] = 0.0f;
    }
    return true;
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
    if (!_SetPaddedVertexBuffer(geometry, points)) {
        rtcReleaseGeometry(geometry);
        rtcReleaseScene(scene);
        rtcReleaseDevice(device);
        return false;
    }
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
        geometry, 0, RTC_SUBDIVISION_MODE_PIN_ALL);
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
    context.displaced = true;
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
    auto textureFrameTime = _CompileTextureFrameTimeDisplacement();
    auto quadraticTexture = _CompileQuadraticTextureDisplacement();
    auto negative = _CompileConstantDisplacement(-0.25f);
    if (!positive || !positive->IsValid() ||
        !texcoord || !texcoord->IsValid() ||
        !textureFrameTime || !textureFrameTime->IsValid() ||
        !quadraticTexture || !quadraticTexture->IsValid() ||
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

    GfVec3f displacedNormal(0.0f);
    GfVec3f displacedDPdu(0.0f);
    GfVec3f displacedDPdv(0.0f);
    GfVec3f displacedPosition(0.0f);
    const bool displacedPositionComputed =
        HdEmbreeComputeDisplacedSubdivPosition(
            geometry, &context, 0, 0.5f, 0.5f,
            &displacedPosition);
    const bool displacedFrameComputed =
        HdEmbreeComputeDisplacedSubdivFrame(
            geometry, &context, 0, 0.5f, 0.5f,
            &displacedNormal, &displacedDPdu, &displacedDPdv);

    _ThreadSafeTexcoordTextureSystem textureSystem;
    HdEmbreeMaterialEvalServices services;
    services.textureSystem = &textureSystem;
    context.materialEvalServices = &services;

    material.displacementGraph = quadraticTexture.get();
    textureSystem.Reset(0.0f);
    HdEmbreeDisplacedSubdivFrame quadraticFrame;
    const bool quadraticFrameComputed =
        HdEmbreeComputeDisplacedSubdivFrame(
            geometry, &context, 0, 0.5f, 0.5f, &quadraticFrame);
    const int quadraticFrameCalls = textureSystem.GetCallCount();
    GfVec3f quadraticDndu(0.0f);
    GfVec3f quadraticDndv(0.0f);
    const bool quadraticDerivativesComputed =
        HdEmbreeComputeDisplacedSubdivNormalDerivatives(
            geometry, &context, quadraticFrame,
            &quadraticDndu, &quadraticDndv);
    const int quadraticDerivativeCalls = textureSystem.GetCallCount();

    // At this coordinate one positive step fits but the two-step outer ring
    // does not. The initial frame must therefore choose one negative step and
    // retain that direction; reselecting at U would erase the curvature.
    constexpr float boundaryU = 0.997f;
    textureSystem.Reset(0.0f);
    HdEmbreeDisplacedSubdivFrame boundaryFrame;
    const bool boundaryFrameComputed =
        HdEmbreeComputeDisplacedSubdivFrame(
            geometry, &context, 0, boundaryU, 0.5f, &boundaryFrame);
    GfVec3f boundaryDndu(0.0f);
    GfVec3f boundaryDndv(0.0f);
    const bool boundaryDerivativesComputed =
        HdEmbreeComputeDisplacedSubdivNormalDerivatives(
            geometry, &context, boundaryFrame,
            &boundaryDndu, &boundaryDndv);
    const int boundaryDerivativeCalls = textureSystem.GetCallCount();

    textureSystem.Reset(0.0f);
    const HdEmbreeDisplacedSubdivFrame invalidFrame;
    GfVec3f invalidDndu(7.0f);
    GfVec3f invalidDndv(11.0f);
    const bool invalidDerivativesComputed =
        HdEmbreeComputeDisplacedSubdivNormalDerivatives(
            geometry, &context, invalidFrame,
            &invalidDndu, &invalidDndv);
    const int invalidDerivativeCalls = textureSystem.GetCallCount();

    material.displacementGraph = textureFrameTime.get();

    constexpr float firstFrame = 0.1f;
    constexpr float firstTime = 0.2f;
    constexpr float centerStX = 0.5f;
    constexpr float rayOriginZ = 2.0f;
    constexpr float firstExpectedHit =
        rayOriginZ - (centerStX + firstFrame + firstTime);
    services.frame = firstFrame;
    services.time = firstTime;
    textureSystem.Reset(firstFrame);
    rebuild();
    const float firstServiceHit = _TraceCenter(scene);
    const int firstTextureCalls = textureSystem.GetCallCount();
    const bool firstFramesMatched = textureSystem.AllFramesMatched() &&
        _Close(textureSystem.GetLastFrame(), firstFrame);

    constexpr float secondFrame = 0.3f;
    constexpr float secondTime = 0.4f;
    constexpr float secondExpectedHit =
        rayOriginZ - (centerStX + secondFrame + secondTime);
    services.frame = secondFrame;
    services.time = secondTime;
    textureSystem.Reset(secondFrame);
    rebuild();
    const float secondServiceHit = _TraceCenter(scene);
    const int secondTextureCalls = textureSystem.GetCallCount();
    const bool secondFramesMatched = textureSystem.AllFramesMatched() &&
        _Close(textureSystem.GetLastFrame(), secondFrame);

    material.displacementGraph = nullptr;
    rebuild();
    const float undisplacedHit = _TraceCenter(scene);
    material.displacementGraph = negative.get();
    rebuild();
    const float negativeHit = _TraceCenter(scene);

    // A small parameterization is not degenerate merely because its absolute
    // area is tiny. A matching small prototype scale makes the world-space
    // tangents smaller still while preserving their linear independence;
    // displacement evaluation must not silently reject either frame using an
    // absolute cross-product threshold.
    constexpr float smallTangentLength = 1.0e-6f;
    GfMatrix4f smallSurfaceToWorld(1.0f);
    smallSurfaceToWorld.SetScale(
        GfVec3f(smallTangentLength));
    context.displacementObjectToWorldMatrix = smallSurfaceToWorld;
    context.displacementWorldToObjectMatrix =
        smallSurfaceToWorld.GetInverse();
    float smallFrameDisplacement =
        std::numeric_limits<float>::quiet_NaN();
    const bool smallFrameEvaluated = HdEmbreeEvaluateDisplacement(
        &context, 0, 0.5f, 0.5f,
        GfVec3f(0.5f, 0.5f, 0.0f),
        GfVec3f(0.0f, 0.0f, 1.0f),
        GfVec3f(smallTangentLength, 0.0f, 0.0f),
        GfVec3f(0.0f, smallTangentLength, 0.0f),
        &smallFrameDisplacement);

    // A retained geometry remains valid after its scene is gone. Sampling here
    // would fail—or touch released scene state—if callbacks stored scene/id
    // pairs instead of the geometry handle captured during mesh sync.
    rtcReleaseScene(scene);
    float detachedHeight = -1.0f;
    const bool detachedSampled =
        heightSampler.Sample(0, 0.5f, 0.5f, &detachedHeight);

    rtcReleaseGeometry(geometry);
    rtcReleaseDevice(device);
    constexpr float inverseSqrtTwo = 0.70710678f;
    const auto expectedNormalDerivative = [](
            float u, float v, bool uDirection) {
        const GfVec3f unnormalizedNormal(-2.0f * u, -2.0f * v, 1.0f);
        const float normalLength = unnormalizedNormal.GetLength();
        const GfVec3f normal = unnormalizedNormal / normalLength;
        const GfVec3f unnormalizedDerivative = uDirection
            ? GfVec3f(-2.0f, 0.0f, 0.0f)
            : GfVec3f(0.0f, -2.0f, 0.0f);
        return (unnormalizedDerivative - normal *
            GfDot(normal, unnormalizedDerivative)) / normalLength;
    };
    const GfVec3f expectedCenterDndu =
        expectedNormalDerivative(0.5f, 0.5f, true);
    const GfVec3f expectedCenterDndv =
        expectedNormalDerivative(0.5f, 0.5f, false);
    const GfVec3f expectedBoundaryDndu =
        expectedNormalDerivative(boundaryU, 0.5f, true);
    const GfVec3f expectedBoundaryDndv =
        expectedNormalDerivative(boundaryU, 0.5f, false);
    GfVec3f expectedCenterNormal(-1.0f, -1.0f, 1.0f);
    expectedCenterNormal.Normalize();
    const bool curvatureValid =
        quadraticFrameComputed && quadraticFrame.valid &&
        quadraticFrameCalls == 3 &&
        _Close(quadraticFrame.normal, expectedCenterNormal, 0.005f) &&
        _Close(quadraticFrame.dPdu, GfVec3f(1.0f, 0.0f, 1.0f), 0.005f) &&
        _Close(quadraticFrame.dPdv, GfVec3f(0.0f, 1.0f, 1.0f), 0.005f) &&
        quadraticDerivativesComputed && quadraticDerivativeCalls == 6 &&
        _Close(quadraticDndu, expectedCenterDndu, 0.01f) &&
        _Close(quadraticDndv, expectedCenterDndv, 0.01f) &&
        std::abs(GfDot(quadraticFrame.normal, quadraticDndu)) < 1.0e-4f &&
        std::abs(GfDot(quadraticFrame.normal, quadraticDndv)) < 1.0e-4f &&
        boundaryFrameComputed && boundaryFrame.valid &&
        boundaryFrame.du < 0.0f && boundaryFrame.dv > 0.0f &&
        boundaryDerivativesComputed && boundaryDerivativeCalls == 6 &&
        _Close(boundaryDndu, expectedBoundaryDndu, 0.01f) &&
        _Close(boundaryDndv, expectedBoundaryDndv, 0.01f) &&
        !invalidDerivativesComputed && invalidDerivativeCalls == 0 &&
        _Close(invalidDndu, GfVec3f(7.0f), 0.0f) &&
        _Close(invalidDndv, GfVec3f(11.0f), 0.0f);
    const bool valid = _Close(positiveHit, 1.5f, 0.02f) &&
        _Close(float3TexcoordHit, 1.5f, 0.02f) &&
        displacedPositionComputed &&
        _Close(displacedPosition, GfVec3f(0.5f, 0.5f, 0.5f), 0.02f) &&
        displacedFrameComputed &&
        _Close(displacedDPdu, GfVec3f(1.0f, 0.0f, 1.0f), 0.02f) &&
        _Close(displacedDPdv, GfVec3f(0.0f, 1.0f, 0.0f), 0.02f) &&
        _Close(
            displacedNormal,
            GfVec3f(-inverseSqrtTwo, 0.0f, inverseSqrtTwo), 0.02f) &&
        firstTextureCalls > 0 && firstFramesMatched &&
        _Close(firstServiceHit, firstExpectedHit, 0.02f) &&
        secondTextureCalls > 0 && secondFramesMatched &&
        _Close(secondServiceHit, secondExpectedHit, 0.02f) &&
        firstServiceHit > secondServiceHit + 0.3f &&
        _Close(undisplacedHit, 2.0f, 0.02f) &&
        _Close(negativeHit, 2.25f, 0.02f) &&
        smallFrameEvaluated &&
        _Close(smallFrameDisplacement, -0.25f) &&
        detachedSampled && _Close(detachedHeight, 0.5f, 0.02f) &&
        curvatureValid;
    if (!valid) {
        std::printf(
            "    callback hits=(%g,%g) services=(%g,%g) calls=(%d,%d) "
            "frames=(%d,%d) frameOk=%d dPdu=(%g,%g,%g) "
            "dPdv=(%g,%g,%g) N=(%g,%g,%g) P=(%g,%g,%g) "
            "smallFrame=(%d,%g)\n",
            positiveHit, float3TexcoordHit,
            firstServiceHit, secondServiceHit,
            firstTextureCalls, secondTextureCalls,
            firstFramesMatched, secondFramesMatched,
            displacedFrameComputed,
            displacedDPdu[0], displacedDPdu[1], displacedDPdu[2],
            displacedDPdv[0], displacedDPdv[1], displacedDPdv[2],
            displacedNormal[0], displacedNormal[1], displacedNormal[2],
            displacedPosition[0], displacedPosition[1],
            displacedPosition[2],
            smallFrameEvaluated, smallFrameDisplacement);
        std::printf(
            "    curvature frame=(%d,%d,%g) deriv=(%d,%d,%g,%g,%g;"
            "%g,%g,%g) boundary=(%d,%d,%g,%g,%g,%g) invalid=(%d,%d)\n",
            quadraticFrameComputed, quadraticFrameCalls, quadraticFrame.du,
            quadraticDerivativesComputed, quadraticDerivativeCalls,
            quadraticDndu[0], quadraticDndu[1], quadraticDndu[2],
            quadraticDndv[0], quadraticDndv[1], quadraticDndv[2],
            boundaryFrameComputed, boundaryDerivativeCalls, boundaryFrame.du,
            boundaryDndu[0], boundaryDndu[1], boundaryDndu[2],
            invalidDerivativesComputed, invalidDerivativeCalls);
    }
    return valid;
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
    std::atomic<int> displacementVersion{0};
    HdEmbreeRenderParam renderParam(
        notificationDevice, notificationScene, &renderThread, nullptr,
        nullptr, &sceneVersion, &displacementVersion);

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
            sceneVersion.load() == 3 && displacementVersion.load() == 3;
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
TestMaterialTerminalTransitionsCommitWithInstanceOnlyDirtyBits()
{
    _EmbreeTestContext context;
    HdRenderDelegate* const renderDelegate = context.renderDelegate;
    HdRenderIndex* const renderIndex = context.renderIndex.get();
    auto* const embreeDelegate =
        dynamic_cast<HdEmbreeRenderDelegate*>(renderDelegate);
    if (!renderIndex || !embreeDelegate) {
        return false;
    }

    HdUnitTestDelegate delegate(
        renderIndex, SdfPath::AbsoluteRootPath());
    const SdfPath materialId("/transitionMaterial");
    const SdfPath meshId("/transitionQuad");
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

    HdSprim* const material = renderIndex->GetSprim(
        HdPrimTypeTokens->material, materialId);
    HdDirtyBits materialBits = material->GetInitialDirtyBitsMask();
    material->Sync(
        &delegate, renderDelegate->GetRenderParam(), &materialBits);

    HdRprim* const mesh =
        const_cast<HdRprim*>(renderIndex->GetRprim(meshId));
    HdDirtyBits meshBits = mesh->GetInitialDirtyBitsMask();
    mesh->InitRepr(&delegate, HdReprTokens->refined, &meshBits);
    mesh->Sync(
        &delegate, renderDelegate->GetRenderParam(),
        &meshBits, HdReprTokens->refined);

    RTCScene const root = static_cast<HdEmbreeRenderParam*>(
        renderDelegate->GetRenderParam())->AcquireSceneForEdit();
    rtcCommitScene(root);
    embreeDelegate->UpdateAdaptiveSubdivision(
        GfMatrix4d(1.0), GfMatrix4d(1.0),
        GfRect2i(GfVec2i(0), 100, 100), true);
    rtcCommitScene(root);
    const float initiallyDisplacedHit = _TraceCenter(root);

    // The material's stable handle changes in place. A simultaneous
    // instance-only mesh edit must still remove the callback and recommit the
    // prototype, rather than leaving stale displaced tessellation behind.
    delegate.UpdateMaterialResource(
        materialId, VtValue(_MakeHydraMaterialNetwork(false)));
    materialBits = HdMaterial::DirtyResource;
    material->Sync(
        &delegate, renderDelegate->GetRenderParam(), &materialBits);
    meshBits = HdChangeTracker::DirtyTransform;
    mesh->Sync(
        &delegate, renderDelegate->GetRenderParam(),
        &meshBits, HdReprTokens->refined);
    rtcCommitScene(root);
    const float removedHit = _TraceCenter(root);

    // Exercise the inverse transition with the exact DirtyInstanceIndex path
    // that otherwise contains no prototype data changes.
    delegate.UpdateMaterialResource(
        materialId,
        VtValue(_MakeHydraMaterialNetwork(true, -0.25f)));
    materialBits = HdMaterial::DirtyResource;
    material->Sync(
        &delegate, renderDelegate->GetRenderParam(), &materialBits);
    meshBits = HdChangeTracker::DirtyInstanceIndex;
    mesh->Sync(
        &delegate, renderDelegate->GetRenderParam(),
        &meshBits, HdReprTokens->refined);
    rtcCommitScene(root);
    const float restoredHit = _TraceCenter(root);

    RTCGeometry const prototype = _GetPrototypeGeometry(root);
    auto const* prototypeContext = prototype
        ? static_cast<HdEmbreePrototypeContext const*>(
            rtcGetGeometryUserData(prototype))
        : nullptr;
    const bool valid =
        _Close(initiallyDisplacedHit, 1.5f, 0.02f) &&
        _Close(removedHit, 2.0f, 0.02f) &&
        _Close(restoredHit, 2.25f, 0.02f) &&
        prototypeContext && prototypeContext->displaced;
    if (!valid) {
        std::printf(
            "    displacement transition hits=(%g,%g,%g) displaced=%d\n",
            initiallyDisplacedHit, removedHit, restoredHit,
            prototypeContext && prototypeContext->displaced);
    }
    return valid;
}

bool
TestLeftHandedSubdivisionDisplacesAlongAuthoredNormal()
{
    _EmbreeTestContext context;
    HdRenderDelegate* renderDelegate = context.renderDelegate;
    HdRenderIndex* renderIndex = context.renderIndex.get();
    auto* embreeDelegate =
        dynamic_cast<HdEmbreeRenderDelegate*>(renderDelegate);
    if (!renderIndex || !embreeDelegate) {
        return false;
    }

    HdUnitTestDelegate delegate(
        renderIndex, SdfPath::AbsoluteRootPath());
    const SdfPath materialId("/leftMaterial");
    const SdfPath meshId("/leftQuad");
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
        VtIntArray{4}, VtIntArray{0, 1, 2, 3},
        false, SdfPath(), PxOsdOpenSubdivTokens->catmullClark,
        HdTokens->leftHanded);
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

    RTCGeometry prototype = _GetPrototypeGeometry(root);
    auto const* prototypeContext = prototype
        ? static_cast<HdEmbreePrototypeContext const*>(
            rtcGetGeometryUserData(prototype))
        : nullptr;
    const float hit = _TraceCenter(root);
    if (!prototypeContext ||
        !_Close(prototypeContext->orientationSign, -1.0f) ||
        !_Close(hit, 2.5f, 0.02f)) {
        std::printf(
            "    left-handed displacement: sign=%g hit=%g expected=2.5\n",
            prototypeContext ? prototypeContext->orientationSign : 0.0f,
            hit);
        return false;
    }
    return true;
}

bool
TestRprimScalePreservesWorldUnitDisplacement()
{
    _EmbreeTestContext context;
    HdRenderDelegate* const renderDelegate = context.renderDelegate;
    HdRenderIndex* const renderIndex = context.renderIndex.get();
    auto* const embreeDelegate =
        dynamic_cast<HdEmbreeRenderDelegate*>(renderDelegate);
    if (!renderIndex || !embreeDelegate) {
        return false;
    }

    HdUnitTestDelegate delegate(
        renderIndex, SdfPath::AbsoluteRootPath());
    const SdfPath materialId("/scaledMaterial");
    const SdfPath meshId("/scaledQuad");
    delegate.AddMaterialResource(
        materialId,
        VtValue(_MakeHydraMaterialNetwork(true, 0.5f)));
    GfMatrix4f transform(1.0f);
    transform.SetScale(GfVec3f(2.0f, 1.0f, 3.0f));
    delegate.AddMesh(
        meshId, transform,
        VtVec3fArray{
            GfVec3f(0.0f, 0.0f, 0.0f),
            GfVec3f(1.0f, 0.0f, 0.0f),
            GfVec3f(1.0f, 1.0f, 0.0f),
            GfVec3f(0.0f, 1.0f, 0.0f)},
        VtIntArray{4}, VtIntArray{0, 1, 2, 3});
    delegate.BindMaterial(meshId, materialId);
    delegate.SetRefineLevel(meshId, 2);

    HdSprim* const material = renderIndex->GetSprim(
        HdPrimTypeTokens->material, materialId);
    HdDirtyBits materialBits = material->GetInitialDirtyBitsMask();
    material->Sync(
        &delegate, renderDelegate->GetRenderParam(), &materialBits);

    HdRprim* const mesh =
        const_cast<HdRprim*>(renderIndex->GetRprim(meshId));
    HdDirtyBits meshBits = mesh->GetInitialDirtyBitsMask();
    mesh->InitRepr(&delegate, HdReprTokens->refined, &meshBits);
    mesh->Sync(
        &delegate, renderDelegate->GetRenderParam(),
        &meshBits, HdReprTokens->refined);

    RTCScene const root = static_cast<HdEmbreeRenderParam*>(
        renderDelegate->GetRenderParam())->AcquireSceneForEdit();
    rtcCommitScene(root);
    embreeDelegate->UpdateAdaptiveSubdivision(
        GfMatrix4d(1.0), GfMatrix4d(1.0),
        GfRect2i(GfVec2i(0), 100, 100), true);
    rtcCommitScene(root);

    RTCGeometry const prototype = _GetPrototypeGeometry(root);
    auto const* prototypeContext = prototype
        ? static_cast<HdEmbreePrototypeContext const*>(
            rtcGetGeometryUserData(prototype))
        : nullptr;
    GfVec3f objectOffset(0.0f);
    const bool offsetComputed =
        HdEmbreeComputeObjectSpaceDisplacementOffset(
            prototypeContext,
            GfVec3f(0.0f, 0.0f, 1.0f),
            0.5f,
            &objectOffset);
    const GfVec3f worldOffset = offsetComputed
        ? prototypeContext->displacementObjectToWorldMatrix.TransformDir(
            objectOffset)
        : GfVec3f(0.0f);
    const RTCRayHit rayHit = _TraceWithLimit(
        root, 1.0f, 0.5f, 2.0f, 4.0f);
    const bool valid = prototypeContext && prototypeContext->displaced &&
        offsetComputed &&
        _Close(objectOffset, GfVec3f(0.0f, 0.0f, 1.0f / 6.0f)) &&
        _Close(worldOffset, GfVec3f(0.0f, 0.0f, 0.5f)) &&
        rayHit.hit.geomID != RTC_INVALID_GEOMETRY_ID &&
        _Close(rayHit.ray.tfar, 1.5f, 0.02f);
    if (!valid) {
        std::printf(
            "    scaled displacement offset=(%g,%g,%g) world=(%g,%g,%g) "
            "geom=%u t=%g\n",
            objectOffset[0], objectOffset[1], objectOffset[2],
            worldOffset[0], worldOffset[1], worldOffset[2],
            rayHit.hit.geomID, rayHit.ray.tfar);
    }
    return valid;
}

bool
TestDisplayStyleCanDisableAndRestoreDisplacement()
{
    _EmbreeTestContext context;
    HdRenderDelegate* const renderDelegate = context.renderDelegate;
    HdRenderIndex* const renderIndex = context.renderIndex.get();
    auto* const embreeDelegate =
        dynamic_cast<HdEmbreeRenderDelegate*>(renderDelegate);
    if (!renderIndex || !embreeDelegate) {
        return false;
    }

    _DisplayStyleDelegate delegate(
        renderIndex, SdfPath::AbsoluteRootPath());
    const SdfPath materialId("/displayStyleMaterial");
    const SdfPath meshId("/displayStyleQuad");
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

    HdSprim* const material = renderIndex->GetSprim(
        HdPrimTypeTokens->material, materialId);
    HdDirtyBits materialBits = material->GetInitialDirtyBitsMask();
    material->Sync(
        &delegate, renderDelegate->GetRenderParam(), &materialBits);
    HdRprim* const mesh =
        const_cast<HdRprim*>(renderIndex->GetRprim(meshId));
    HdDirtyBits meshBits = mesh->GetInitialDirtyBitsMask();
    mesh->InitRepr(&delegate, HdReprTokens->refined, &meshBits);
    mesh->Sync(
        &delegate, renderDelegate->GetRenderParam(),
        &meshBits, HdReprTokens->refined);

    RTCScene const root = static_cast<HdEmbreeRenderParam*>(
        renderDelegate->GetRenderParam())->AcquireSceneForEdit();
    rtcCommitScene(root);
    embreeDelegate->UpdateAdaptiveSubdivision(
        GfMatrix4d(1.0), GfMatrix4d(1.0),
        GfRect2i(GfVec2i(0), 100, 100), true);
    rtcCommitScene(root);
    const float enabledHit = _TraceCenter(root);

    delegate.SetDisplacementEnabled(meshId, false);
    meshBits = HdChangeTracker::DirtyDisplayStyle;
    mesh->Sync(
        &delegate, renderDelegate->GetRenderParam(),
        &meshBits, HdReprTokens->refined);
    rtcCommitScene(root);
    const float disabledHit = _TraceCenter(root);

    delegate.SetDisplacementEnabled(meshId, true);
    meshBits = HdChangeTracker::DirtyDisplayStyle;
    mesh->Sync(
        &delegate, renderDelegate->GetRenderParam(),
        &meshBits, HdReprTokens->refined);
    rtcCommitScene(root);
    const float restoredHit = _TraceCenter(root);

    const bool valid =
        _Close(enabledHit, 1.5f, 0.02f) &&
        _Close(disabledHit, 2.0f, 0.02f) &&
        _Close(restoredHit, 1.5f, 0.02f);
    if (!valid) {
        std::printf(
            "    display-style displacement hits=(%g,%g,%g)\n",
            enabledHit, disabledHit, restoredHit);
    }
    return valid;
}

bool
TestEmptyPointsDisableAndRestoreGeometry()
{
    _EmbreeTestContext context;
    HdRenderDelegate* const renderDelegate = context.renderDelegate;
    HdRenderIndex* const renderIndex = context.renderIndex.get();
    if (!renderDelegate || !renderIndex) {
        return false;
    }

    _PointsOverrideDelegate delegate(
        renderIndex, SdfPath::AbsoluteRootPath());
    const SdfPath meshId("/mutablePointsTriangle");
    const VtVec3fArray points{
        GfVec3f(0.0f, 0.0f, 0.0f),
        GfVec3f(1.0f, 0.0f, 0.0f),
        GfVec3f(0.0f, 1.0f, 0.0f)};
    delegate.AddMesh(
        meshId, GfMatrix4f(1.0f), points,
        VtIntArray{3}, VtIntArray{0, 1, 2},
        false, SdfPath(), PxOsdOpenSubdivTokens->none);
    delegate.SetRefineLevel(meshId, 0);
    delegate.SetPoints(meshId, points);

    HdRprim* const mesh =
        const_cast<HdRprim*>(renderIndex->GetRprim(meshId));
    HdDirtyBits meshBits = mesh->GetInitialDirtyBitsMask();
    mesh->InitRepr(&delegate, HdReprTokens->refined, &meshBits);
    mesh->Sync(
        &delegate, renderDelegate->GetRenderParam(),
        &meshBits, HdReprTokens->refined);
    RTCScene const root = static_cast<HdEmbreeRenderParam*>(
        renderDelegate->GetRenderParam())->AcquireSceneForEdit();
    rtcCommitScene(root);
    const RTCRayHit initial = _TraceWithLimit(
        root, 0.25f, 0.25f, 2.0f, 4.0f);

    delegate.SetPoints(meshId, VtVec3fArray());
    meshBits = HdChangeTracker::DirtyPoints;
    mesh->Sync(
        &delegate, renderDelegate->GetRenderParam(),
        &meshBits, HdReprTokens->refined);
    rtcCommitScene(root);
    const RTCRayHit empty = _TraceWithLimit(
        root, 0.25f, 0.25f, 2.0f, 4.0f);

    delegate.SetPoints(meshId, points);
    meshBits = HdChangeTracker::DirtyPoints;
    mesh->Sync(
        &delegate, renderDelegate->GetRenderParam(),
        &meshBits, HdReprTokens->refined);
    rtcCommitScene(root);
    const RTCRayHit restored = _TraceWithLimit(
        root, 0.25f, 0.25f, 2.0f, 4.0f);

    return initial.hit.geomID != RTC_INVALID_GEOMETRY_ID &&
        empty.hit.geomID == RTC_INVALID_GEOMETRY_ID &&
        restored.hit.geomID != RTC_INVALID_GEOMETRY_ID &&
        _Close(initial.ray.tfar, 2.0f, 0.02f) &&
        _Close(restored.ray.tfar, 2.0f, 0.02f);
}

bool
TestProductionVertexBufferHasFloat3PaddingAndUpdates()
{
    _EmbreeTestContext context;
    HdRenderDelegate* renderDelegate = context.renderDelegate;
    HdRenderIndex* renderIndex = context.renderIndex.get();
    if (!renderDelegate || !renderIndex) {
        return false;
    }

    HdUnitTestDelegate delegate(
        renderIndex, SdfPath::AbsoluteRootPath());
    const SdfPath meshId("/paddedQuad");
    const VtVec3fArray points{
        GfVec3f(0.25f, 0.5f, 0.75f),
        GfVec3f(1.25f, 0.5f, 0.75f),
        GfVec3f(1.25f, 1.5f, 0.75f),
        GfVec3f(0.25f, 1.5f, 0.75f)};
    delegate.AddMesh(
        meshId, GfMatrix4f(1.0f), points,
        VtIntArray{4}, VtIntArray{0, 1, 2, 3});
    delegate.SetRefineLevel(meshId, 0);

    HdRprim* mesh = const_cast<HdRprim*>(renderIndex->GetRprim(meshId));
    HdDirtyBits bits = mesh->GetInitialDirtyBitsMask();
    mesh->InitRepr(&delegate, HdReprTokens->refined, &bits);
    mesh->Sync(
        &delegate, renderDelegate->GetRenderParam(),
        &bits, HdReprTokens->refined);

    RTCScene root = static_cast<HdEmbreeRenderParam*>(
        renderDelegate->GetRenderParam())->AcquireSceneForEdit();
    rtcCommitScene(root);
    RTCBounds beforeBounds{};
    rtcGetSceneBounds(root, &beforeBounds);

    const auto bufferMatches = [&](VtVec3fArray const& expected) {
        RTCGeometry prototype = _GetPrototypeGeometry(root);
        float const* data = prototype
            ? static_cast<float const*>(rtcGetGeometryBufferData(
                prototype, RTC_BUFFER_TYPE_VERTEX, 0))
            : nullptr;
        // Check the first padding slot before indexing the complete 16-byte
        // layout. With the old packed float3 buffer this slot is point 1.x,
        // so the test fails without reading beyond that packed allocation.
        if (!data || !_Close(data[0], expected[0][0]) ||
            !_Close(data[1], expected[0][1]) ||
            !_Close(data[2], expected[0][2]) || !_Close(data[3], 0.0f)) {
            std::printf(
                "    invalid first padded vertex: ptr=%p value=(%g,%g,%g,%g)\n",
                static_cast<void const*>(data), data ? data[0] : 0.0f,
                data ? data[1] : 0.0f, data ? data[2] : 0.0f,
                data ? data[3] : 0.0f);
            return false;
        }
        for (size_t i = 0; i < expected.size(); ++i) {
            if (!_Close(data[4 * i + 0], expected[i][0]) ||
                !_Close(data[4 * i + 1], expected[i][1]) ||
                !_Close(data[4 * i + 2], expected[i][2]) ||
                !_Close(data[4 * i + 3], 0.0f)) {
                std::printf(
                    "    invalid padded vertex %zu: got=(%g,%g,%g,%g) "
                    "expected=(%g,%g,%g,0)\n",
                    i, data[4 * i + 0], data[4 * i + 1],
                    data[4 * i + 2], data[4 * i + 3], expected[i][0],
                    expected[i][1], expected[i][2]);
                return false;
            }
        }
        return true;
    };

    if (!bufferMatches(points)) {
        return false;
    }

    constexpr float time = 0.25f;
    delegate.UpdatePositions(meshId, time);
    bits = HdChangeTracker::DirtyPoints;
    mesh->Sync(
        &delegate, renderDelegate->GetRenderParam(),
        &bits, HdReprTokens->refined);
    rtcCommitScene(root);

    VtVec3fArray updatedPoints = points;
    for (size_t i = 0; i < updatedPoints.size(); ++i) {
        updatedPoints[i][0] +=
            0.5f * std::sin(0.5f * static_cast<float>(i) + time);
    }
    RTCBounds afterBounds{};
    rtcGetSceneBounds(root, &afterBounds);
    const bool updatedBufferMatches = bufferMatches(updatedPoints);
    const bool boundsUpdated =
        afterBounds.upper_x > beforeBounds.upper_x + 0.3f;
    if (!boundsUpdated) {
        RTCBounds prototypeBounds{};
        RTCGeometry instance = rtcGetGeometry(root, 0);
        auto const* instanceContext = instance
            ? static_cast<HdEmbreeInstanceContext const*>(
                rtcGetGeometryUserData(instance))
            : nullptr;
        if (instanceContext) {
            rtcGetSceneBounds(
                instanceContext->rootScene, &prototypeBounds);
        }
        RTCGeometry const prototype = _GetPrototypeGeometry(root);
        auto const* triangleIndices = prototype
            ? static_cast<unsigned int const*>(rtcGetGeometryBufferData(
                prototype, RTC_BUFFER_TYPE_INDEX, 0))
            : nullptr;
        std::printf(
            "    DirtyPoints root x bounds did not update: "
            "before=%g after=%g prototype=[%g,%g] indices=(%u,%u,%u)\n",
            beforeBounds.upper_x, afterBounds.upper_x,
            prototypeBounds.lower_x, prototypeBounds.upper_x,
            triangleIndices ? triangleIndices[0] : ~0u,
            triangleIndices ? triangleIndices[1] : ~0u,
            triangleIndices ? triangleIndices[2] : ~0u);
    }
    return updatedBufferMatches && boundsUpdated;
}

bool
TestMirroredInstancePreservesBackfaceCulling()
{
    _EmbreeTestContext context;
    HdRenderDelegate* const renderDelegate = context.renderDelegate;
    HdRenderIndex* const renderIndex = context.renderIndex.get();
    if (!renderDelegate || !renderIndex) {
        return false;
    }

    HdUnitTestDelegate delegate(
        renderIndex, SdfPath::AbsoluteRootPath());
    const SdfPath instancerId("/cullInstancer");
    const SdfPath meshId("/culledTriangle");
    delegate.AddInstancer(instancerId);
    delegate.AddMesh(
        meshId, GfMatrix4f(1.0f),
        VtVec3fArray{
            GfVec3f(0.0f, 0.0f, 0.0f),
            GfVec3f(1.0f, 0.0f, 0.0f),
            GfVec3f(0.0f, 1.0f, 0.0f)},
        VtIntArray{3}, VtIntArray{0, 1, 2},
        false, instancerId);
    delegate.SetRefineLevel(meshId, 0);
    delegate.SetMeshCullStyle(meshId, HdCullStyleBack);
    delegate.SetInstancerProperties(
        instancerId,
        VtIntArray{0, 0},
        VtVec3fArray{
            GfVec3f(1.0f, 1.0f, 1.0f),
            GfVec3f(-1.0f, 1.0f, 1.0f)},
        VtVec4fArray{
            GfVec4f(1.0f, 0.0f, 0.0f, 0.0f),
            GfVec4f(1.0f, 0.0f, 0.0f, 0.0f)},
        VtVec3fArray{
            GfVec3f(0.0f, 0.0f, 0.0f),
            GfVec3f(3.0f, 0.0f, 0.0f)});

    HdRprim* const mesh =
        const_cast<HdRprim*>(renderIndex->GetRprim(meshId));
    HdDirtyBits bits = mesh->GetInitialDirtyBitsMask();
    mesh->InitRepr(&delegate, HdReprTokens->refined, &bits);
    mesh->Sync(
        &delegate, renderDelegate->GetRenderParam(),
        &bits, HdReprTokens->refined);

    RTCScene const root = static_cast<HdEmbreeRenderParam*>(
        renderDelegate->GetRenderParam())->AcquireSceneForEdit();
    rtcCommitScene(root);

    const auto hitsFromWorldSide = [&](float x, float originZ, float dirZ) {
        RTCRayHit rayHit{};
        rayHit.ray.org_x = x;
        rayHit.ray.org_y = 1.0f / 3.0f;
        rayHit.ray.org_z = originZ;
        rayHit.ray.dir_z = dirZ;
        rayHit.ray.tnear = 0.0f;
        rayHit.ray.tfar = 4.0f;
        rayHit.ray.mask = 0xffffffffu;
        rayHit.hit.geomID = RTC_INVALID_GEOMETRY_ID;
        rtcIntersect1(root, &rayHit);
        return rayHit.hit.geomID != RTC_INVALID_GEOMETRY_ID;
    };

    constexpr float identityX = 1.0f / 3.0f;
    constexpr float mirroredX = 3.0f - identityX;
    const bool identityFromAbove =
        hitsFromWorldSide(identityX, 2.0f, -1.0f);
    const bool mirroredFromAbove =
        hitsFromWorldSide(mirroredX, 2.0f, -1.0f);
    const bool identityFromBelow =
        hitsFromWorldSide(identityX, -2.0f, 1.0f);
    const bool mirroredFromBelow =
        hitsFromWorldSide(mirroredX, -2.0f, 1.0f);

    const bool valid =
        identityFromAbove && mirroredFromAbove &&
        !identityFromBelow && !mirroredFromBelow;
    if (!valid) {
        std::printf(
            "    mirrored culling hits identity=(above:%d below:%d) "
            "mirrored=(above:%d below:%d)\n",
            identityFromAbove, identityFromBelow,
            mirroredFromAbove, mirroredFromBelow);
    }
    return valid;
}

bool
_AllInstanceCentersHit(
    RTCScene scene, size_t instanceCount, float spacing,
    float originZ, float maxDistance, float expectedDistance)
{
    for (size_t i = 0; i < instanceCount; ++i) {
        const RTCRayHit rayHit = _TraceWithLimit(
            scene, spacing * static_cast<float>(i) + 0.5f,
            0.5f, originZ, maxDistance);
        if (rayHit.hit.geomID == RTC_INVALID_GEOMETRY_ID ||
            !_Close(rayHit.ray.tfar, expectedDistance, 0.02f)) {
            std::printf(
                "    instance %zu expected hit t=%g, got geom=%u t=%g\n",
                i, expectedDistance, rayHit.hit.geomID, rayHit.ray.tfar);
            return false;
        }
    }
    return true;
}

bool
_AllInstanceCentersMiss(
    RTCScene scene, size_t instanceCount, float spacing,
    float originZ, float maxDistance)
{
    for (size_t i = 0; i < instanceCount; ++i) {
        const RTCRayHit rayHit = _TraceWithLimit(
            scene, spacing * static_cast<float>(i) + 0.5f,
            0.5f, originZ, maxDistance);
        if (rayHit.hit.geomID != RTC_INVALID_GEOMETRY_ID) {
            std::printf(
                "    instance %zu expected miss, got geom=%u t=%g\n",
                i, rayHit.hit.geomID, rayHit.ray.tfar);
            return false;
        }
    }
    return true;
}

bool
TestPrototypeSceneChangesRecommitAllInstances()
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
    const SdfPath materialId("/instanceMaterial");
    const SdfPath instancerId("/instancer");
    const SdfPath meshId("/instancedQuad");
    constexpr size_t instanceCount = 16;
    constexpr float spacing = 3.0f;

    delegate.AddMaterialResource(
        materialId,
        VtValue(_MakeHydraMaterialNetwork(true, 0.0f)));
    delegate.AddInstancer(instancerId);
    delegate.AddMesh(
        meshId, GfMatrix4f(1.0f),
        VtVec3fArray{
            GfVec3f(0.0f, 0.0f, 0.0f),
            GfVec3f(1.0f, 0.0f, 0.0f),
            GfVec3f(1.0f, 1.0f, 0.0f),
            GfVec3f(0.0f, 1.0f, 0.0f)},
        VtIntArray{4}, VtIntArray{0, 1, 2, 3},
        false, instancerId);
    delegate.BindMaterial(meshId, materialId);
    delegate.SetRefineLevel(meshId, 2);

    VtIntArray prototypeIndices;
    VtVec3fArray scales;
    VtVec4fArray rotations;
    VtVec3fArray translations;
    for (size_t i = 0; i < instanceCount; ++i) {
        prototypeIndices.push_back(0);
        scales.push_back(GfVec3f(1.0f));
        rotations.push_back(GfVec4f(1.0f, 0.0f, 0.0f, 0.0f));
        translations.push_back(
            GfVec3f(spacing * static_cast<float>(i), 0.0f, 0.0f));
    }
    delegate.SetInstancerProperties(
        instancerId, prototypeIndices, scales, rotations, translations);

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
    if (!_AllInstanceCentersMiss(
            root, instanceCount, spacing, 10.0f, 6.0f)) {
        std::printf("    failed initial undisplaced finite-ray check\n");
        return false;
    }

    // Exercise the normal DirtyMaterialId Sync path, then the renderer's
    // forced adaptive rebuild that retessellates externally changed
    // displacement graphs. Every instance must be recommitted before the
    // root scene.
    delegate.UpdateMaterialResource(
        materialId,
        VtValue(_MakeHydraMaterialNetwork(true, 5.0f)));
    materialBits = HdMaterial::DirtyResource;
    material->Sync(
        &delegate, renderDelegate->GetRenderParam(), &materialBits);
    meshBits = HdChangeTracker::DirtyMaterialId;
    mesh->Sync(
        &delegate, renderDelegate->GetRenderParam(),
        &meshBits, HdReprTokens->refined);
    rtcCommitScene(root);
    const bool materialRebuilt = renderDelegate->UpdateAdaptiveSubdivision(
        GfMatrix4d(1.0), GfMatrix4d(1.0),
        GfRect2i(GfVec2i(0), 100, 100), true);
    rtcCommitScene(root);
    RTCBounds materialBounds{};
    rtcGetSceneBounds(root, &materialBounds);
    if (!materialRebuilt || materialBounds.upper_z < 4.9f ||
        !_AllInstanceCentersHit(
            root, instanceCount, spacing, 10.0f, 6.0f, 5.0f)) {
        std::printf(
            "    failed material/adaptive check, rebuilt=%d, "
            "root upper z=%g\n",
            materialRebuilt, materialBounds.upper_z);
        return false;
    }

    // Adaptive subdivision can retessellate without a mesh Sync. Raising the
    // displacement above the old instance bounds makes a stale BVH miss these
    // finite-distance rays.
    delegate.UpdateMaterialResource(
        materialId,
        VtValue(_MakeHydraMaterialNetwork(true, 8.0f)));
    materialBits = HdMaterial::DirtyResource;
    material->Sync(
        &delegate, renderDelegate->GetRenderParam(), &materialBits);
    const bool adaptiveRebuilt = renderDelegate->UpdateAdaptiveSubdivision(
        GfMatrix4d(1.0), GfMatrix4d(1.0),
        GfRect2i(GfVec2i(0), 100, 100), true);
    rtcCommitScene(root);
    if (!adaptiveRebuilt || !_AllInstanceCentersHit(
            root, instanceCount, spacing, 10.0f, 3.0f, 2.0f)) {
        std::printf(
            "    failed adaptive check, rebuilt=%d\n", adaptiveRebuilt);
        return false;
    }

    delegate.SetVisibility(meshId, false);
    meshBits = HdChangeTracker::DirtyVisibility;
    mesh->Sync(
        &delegate, renderDelegate->GetRenderParam(),
        &meshBits, HdReprTokens->refined);
    rtcCommitScene(root);
    if (!_AllInstanceCentersMiss(
            root, instanceCount, spacing, 10.0f, 3.0f)) {
        std::printf("    failed hidden visibility check\n");
        return false;
    }

    delegate.SetVisibility(meshId, true);
    meshBits = HdChangeTracker::DirtyVisibility;
    mesh->Sync(
        &delegate, renderDelegate->GetRenderParam(),
        &meshBits, HdReprTokens->refined);
    rtcCommitScene(root);
    if (!_AllInstanceCentersHit(
            root, instanceCount, spacing, 10.0f, 3.0f, 2.0f)) {
        std::printf("    failed restored visibility check\n");
        return false;
    }

    RTCBounds beforeTopologyBounds{};
    rtcGetSceneBounds(root, &beforeTopologyBounds);

    // DirtyTopology replaces the geometry within the existing prototype
    // scene. Move its points at the same time so stale instance bounds are
    // observable rather than accidentally identical to the old geometry.
    delegate.UpdatePositions(meshId, 0.0f);
    meshBits = HdChangeTracker::DirtyTopology |
        HdChangeTracker::DirtyPoints;
    mesh->Sync(
        &delegate, renderDelegate->GetRenderParam(),
        &meshBits, HdReprTokens->refined);
    rtcCommitScene(root);
    RTCBounds topologyBounds{};
    rtcGetSceneBounds(root, &topologyBounds);
    const bool valid =
        topologyBounds.upper_x > beforeTopologyBounds.upper_x + 0.2f;
    if (!valid) {
        std::printf(
            "    failed topology check, root upper x before=%g after=%g\n",
            beforeTopologyBounds.upper_x, topologyBounds.upper_x);
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
    delegate.UpdateTransform(cameraId, GfMatrix4f(1.0f));
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
        delegate.SetRefineLevelFallback(2);
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

        // Continue through the production material/mesh Sync path. Terminal
        // extraction alone would not catch a graph that is present in USD but
        // never compiled or registered as an Embree callback.
        HdRenderDelegate* const renderDelegate = context.renderDelegate;
        HdSprim* const material = renderIndex->GetSprim(
            HdPrimTypeTokens->material, SdfPath("/Material"));
        HdRprim* const mesh =
            const_cast<HdRprim*>(renderIndex->GetRprim(SdfPath("/Mesh")));
        if (!material || !mesh) {
            return false;
        }
        HdDirtyBits materialBits = material->GetInitialDirtyBitsMask();
        material->Sync(
            &delegate, renderDelegate->GetRenderParam(), &materialBits);
        auto const* embreeMaterial =
            dynamic_cast<HdEmbreeMaterial const*>(material);
        valid = valid && embreeMaterial &&
            embreeMaterial->GetRenderMaterial()->displacementGraph;

        HdDirtyBits meshBits = mesh->GetInitialDirtyBitsMask();
        mesh->InitRepr(&delegate, HdReprTokens->refined, &meshBits);
        mesh->Sync(
            &delegate, renderDelegate->GetRenderParam(),
            &meshBits, HdReprTokens->refined);
        RTCScene const root = static_cast<HdEmbreeRenderParam*>(
            renderDelegate->GetRenderParam())->AcquireSceneForEdit();
        rtcCommitScene(root);
        auto* const embreeDelegate =
            dynamic_cast<HdEmbreeRenderDelegate*>(renderDelegate);
        valid = valid && embreeDelegate &&
            embreeDelegate->UpdateAdaptiveSubdivision(
                GfMatrix4d(1.0), GfMatrix4d(1.0),
                GfRect2i(GfVec2i(0), 100, 100), true);
        rtcCommitScene(root);
        valid = valid && _Close(_TraceCenter(root), 1.75f, 0.02f);
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
        {"Subdivision.TestDisplacementProbeRaisesOnlyCurvedDirection",
         &TestDisplacementProbeRaisesOnlyCurvedDirection},
        {"Subdivision.TestDisplacementProbeUsesComplexityAndLargestInstance",
         &TestDisplacementProbeUsesComplexityAndLargestInstance},
        {"Subdivision.TestDisplacementProbeFailureAndNonQuadKeepBaseline",
         &TestDisplacementProbeFailureAndNonQuadKeepBaseline},
        {"Subdivision.TestDisplacementProbePreservesSharedEdgeBalance",
         &TestDisplacementProbePreservesSharedEdgeBalance},
        {"Subdivision.TestDisplacementBoostPropagatesAcrossQuadStrip",
         &TestDisplacementBoostPropagatesAcrossQuadStrip},
        {"Subdivision.TestDisplacementBoostPreservesBaselineOppositeRatio",
         &TestDisplacementBoostPreservesBaselineOppositeRatio},
        {"Subdivision.TestDisplacementBoostTouchesOnlySharedNonQuadEdge",
         &TestDisplacementBoostTouchesOnlySharedNonQuadEdge},
        {"Subdivision.TestSharedEdgesUseSameMaximumLevel",
         &TestSharedEdgesUseSameMaximumLevel},
        {"Subdivision.TestBalancedSubdivisionLevelsUseSharedMaximum",
         &TestBalancedSubdivisionLevelsUseSharedMaximum},
        {"Subdivision.TestBalancedSubdivisionLevelsReachOppositeTwoToOneFixedPoint",
         &TestBalancedSubdivisionLevelsReachOppositeTwoToOneFixedPoint},
        {"Subdivision.TestBalancedSubdivisionLevelsAreRaiseOnlyAndIdempotent",
         &TestBalancedSubdivisionLevelsAreRaiseOnlyAndIdempotent},
        {"Subdivision.TestBalancedSubdivisionLevelsStayInRangeAndRejectInvalidInputs",
         &TestBalancedSubdivisionLevelsStayInRangeAndRejectInvalidInputs},
        {"Subdivision.TestBalancedSubdivisionLevelsSkipNonQuadFaces",
         &TestBalancedSubdivisionLevelsSkipNonQuadFaces},
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
        {"Subdivision.TestAdaptiveSubdivisionUsesGuardBandAndRequiredMinimum",
         &TestAdaptiveSubdivisionUsesGuardBandAndRequiredMinimum},
        {"Subdivision.TestAdaptiveSubdivisionDoesNotGuardDepthPlanes",
         &TestAdaptiveSubdivisionDoesNotGuardDepthPlanes},
        {"Subdivision.TestRealCallbackAddsRemovesAndReplacesDisplacement",
         &TestRealCallbackAddsRemovesAndReplacesDisplacement},
        {"Subdivision.TestMaterialSyncKeepsStableHandleAndReplacesDisplacementGraph",
         &TestMaterialSyncKeepsStableHandleAndReplacesDisplacementGraph},
        {"Subdivision.TestMaterialChangeForcesProductionDisplacementRecommit",
         &TestMaterialChangeForcesProductionDisplacementRecommit},
        {"Subdivision.TestMaterialTerminalTransitionsCommitWithInstanceOnlyDirtyBits",
         &TestMaterialTerminalTransitionsCommitWithInstanceOnlyDirtyBits},
        {"Subdivision.TestLeftHandedSubdivisionDisplacesAlongAuthoredNormal",
         &TestLeftHandedSubdivisionDisplacesAlongAuthoredNormal},
        {"Subdivision.TestRprimScalePreservesWorldUnitDisplacement",
         &TestRprimScalePreservesWorldUnitDisplacement},
        {"Subdivision.TestDisplayStyleCanDisableAndRestoreDisplacement",
         &TestDisplayStyleCanDisableAndRestoreDisplacement},
        {"Subdivision.TestEmptyPointsDisableAndRestoreGeometry",
         &TestEmptyPointsDisableAndRestoreGeometry},
        {"Subdivision.TestProductionVertexBufferHasFloat3PaddingAndUpdates",
         &TestProductionVertexBufferHasFloat3PaddingAndUpdates},
        {"Subdivision.TestMirroredInstancePreservesBackfaceCulling",
         &TestMirroredInstancePreservesBackfaceCulling},
        {"Subdivision.TestPrototypeSceneChangesRecommitAllInstances",
         &TestPrototypeSceneChangesRecommitAllInstances},
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
