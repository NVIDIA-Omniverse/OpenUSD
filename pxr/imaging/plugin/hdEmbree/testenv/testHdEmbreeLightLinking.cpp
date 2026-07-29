//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include <delegate/instancer.h>
#include <delegate/rendererPlugin.h>
#include <renderer/lights/lightLinking.h>

#include "pxr/imaging/hd/renderIndex.h"
#include "pxr/imaging/hd/unitTestDelegate.h"

#include <cstdio>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

class _CategoryDelegate final : public HdUnitTestDelegate
{
public:
    _CategoryDelegate(HdRenderIndex* index)
        : HdUnitTestDelegate(index, SdfPath::AbsoluteRootPath())
    {
    }

    VtArray<TfToken> GetCategories(SdfPath const& id) override
    {
        if (id == SdfPath("/parent")) {
            return {TfToken("parentWhole")};
        }
        if (id == SdfPath("/child")) {
            return {TfToken("childWhole")};
        }
        return {};
    }

    std::vector<VtArray<TfToken>> GetInstanceCategories(
        SdfPath const& id) override
    {
        if (id == SdfPath("/parent")) {
            return {{TfToken("parent0")}, {TfToken("parent1")}};
        }
        if (id == SdfPath("/child")) {
            return {
                {TfToken("child0")},
                {TfToken("child1")},
                {TfToken("child2")}};
        }
        return {};
    }
};

bool
TestDefaultLinkMatchesEveryReceiver()
{
    return ty::MatchesLink(TfToken(), {}) &&
        ty::MatchesLink(TfToken(), {TfToken("receiver")});
}

bool
TestAuthoredLinkRequiresExactMembership()
{
    const TfToken link("collection:lightLink");
    return !ty::MatchesLink(link, {}) &&
        !ty::MatchesLink(link, {TfToken("other")}) &&
        ty::MatchesLink(
            link, {TfToken("other"), TfToken("collection:lightLink")});
}

bool
TestLightAndShadowLinksRemainIndependent()
{
    const ty::CategorySet categories{TfToken("lightMembership")};
    return ty::MatchesLink(TfToken("lightMembership"), categories) &&
        !ty::MatchesLink(TfToken("shadowMembership"), categories);
}

bool
TestCategoryMergeIsStableAndDeduplicated()
{
    ty::CategorySet categories{
        TfToken("prototype"), TfToken("shared")};
    ty::MergeCategories(
        {TfToken("shared"), TfToken("instance")}, &categories);

    return categories.size() == 3 &&
        categories[0] == TfToken("prototype") &&
        categories[1] == TfToken("shared") &&
        categories[2] == TfToken("instance");
}

bool
TestSparseNestedInstancesKeepTransformsAndCategoriesPaired()
{
    HdEmbreeRendererPlugin plugin;
    HdRenderDelegate* renderDelegate = plugin.CreateRenderDelegate();
    if (!renderDelegate) {
        return false;
    }
    HdRenderIndex* renderIndex =
        HdRenderIndex::New(renderDelegate, HdDriverVector());
    if (!renderIndex) {
        plugin.DeleteRenderDelegate(renderDelegate);
        return false;
    }

    bool valid = false;
    {
        _CategoryDelegate delegate(renderIndex);
        delegate.AddInstancer(SdfPath("/parent"));
        delegate.AddInstancer(SdfPath("/child"), SdfPath("/parent"));
        delegate.AddCube(
            SdfPath("/prototype"), GfMatrix4f(1.0f), false,
            SdfPath("/child"));
        delegate.AddCube(
            SdfPath("/otherPrototype"), GfMatrix4f(1.0f), false,
            SdfPath("/child"));

        delegate.SetInstancerProperties(
            SdfPath("/parent"),
            VtIntArray{0, 0},
            VtVec3fArray{GfVec3f(1.0f), GfVec3f(1.0f)},
            VtVec4fArray{
                GfVec4f(1.0f, 0.0f, 0.0f, 0.0f),
                GfVec4f(1.0f, 0.0f, 0.0f, 0.0f)},
            VtVec3fArray{
                GfVec3f(10.0f, 0.0f, 0.0f),
                GfVec3f(20.0f, 0.0f, 0.0f)});
        delegate.SetInstancerProperties(
            SdfPath("/child"),
            VtIntArray{0, 1, 0},
            VtVec3fArray{
                GfVec3f(1.0f), GfVec3f(1.0f), GfVec3f(1.0f)},
            VtVec4fArray{
                GfVec4f(1.0f, 0.0f, 0.0f, 0.0f),
                GfVec4f(1.0f, 0.0f, 0.0f, 0.0f),
                GfVec4f(1.0f, 0.0f, 0.0f, 0.0f)},
            VtVec3fArray{
                GfVec3f(1.0f, 0.0f, 0.0f),
                GfVec3f(2.0f, 0.0f, 0.0f),
                GfVec3f(3.0f, 0.0f, 0.0f)});

        for (SdfPath const& id : {SdfPath("/parent"), SdfPath("/child")}) {
            HdInstancer* instancer = renderIndex->GetInstancer(id);
            HdDirtyBits bits = instancer->GetInitialDirtyBitsMask();
            instancer->Sync(
                &delegate, renderDelegate->GetRenderParam(), &bits);
        }

        HdEmbreeInstancer* child = static_cast<HdEmbreeInstancer*>(
            renderIndex->GetInstancer(SdfPath("/child")));
        const std::vector<HdEmbreeInstanceData> instances =
            child->ComputeInstanceData(SdfPath("/prototype"));

        valid = instances.size() == 4 &&
            instances[0].sourceInstanceIndex == 0 &&
            instances[1].sourceInstanceIndex == 2 &&
            instances[2].sourceInstanceIndex == 0 &&
            instances[3].sourceInstanceIndex == 2 &&
            instances[0].transform.ExtractTranslation() ==
                GfVec3d(11.0, 0.0, 0.0) &&
            instances[1].transform.ExtractTranslation() ==
                GfVec3d(13.0, 0.0, 0.0) &&
            instances[2].transform.ExtractTranslation() ==
                GfVec3d(21.0, 0.0, 0.0) &&
            instances[3].transform.ExtractTranslation() ==
                GfVec3d(23.0, 0.0, 0.0) &&
            instances[0].categories == ty::CategorySet{
                TfToken("parentWhole"), TfToken("parent0"),
                TfToken("childWhole"), TfToken("child0")} &&
            instances[1].categories == ty::CategorySet{
                TfToken("parentWhole"), TfToken("parent0"),
                TfToken("childWhole"), TfToken("child2")} &&
            instances[2].categories == ty::CategorySet{
                TfToken("parentWhole"), TfToken("parent1"),
                TfToken("childWhole"), TfToken("child0")} &&
            instances[3].categories == ty::CategorySet{
                TfToken("parentWhole"), TfToken("parent1"),
                TfToken("childWhole"), TfToken("child2")};
    }

    delete renderIndex;
    plugin.DeleteRenderDelegate(renderDelegate);
    return valid;
}

} // namespace

int
main()
{
    struct Test {
        const char* name;
        bool (*fn)();
    };

    const Test tests[] = {
        {"LightLinking.TestDefaultLinkMatchesEveryReceiver",
         &TestDefaultLinkMatchesEveryReceiver},
        {"LightLinking.TestAuthoredLinkRequiresExactMembership",
         &TestAuthoredLinkRequiresExactMembership},
        {"LightLinking.TestLightAndShadowLinksRemainIndependent",
         &TestLightAndShadowLinksRemainIndependent},
        {"LightLinking.TestCategoryMergeIsStableAndDeduplicated",
         &TestCategoryMergeIsStableAndDeduplicated},
        {"LightLinking.TestSparseNestedInstancesKeepTransformsAndCategories"
         "Paired",
         &TestSparseNestedInstancesKeepTransformsAndCategoriesPaired},
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

    if (failed != 0) {
        std::printf("%d/%zu tests failed.\n", failed,
                    sizeof(tests) / sizeof(tests[0]));
        return 1;
    }

    std::printf("%zu/%zu tests passed.\n",
                sizeof(tests) / sizeof(tests[0]),
                sizeof(tests) / sizeof(tests[0]));
    return 0;
}
