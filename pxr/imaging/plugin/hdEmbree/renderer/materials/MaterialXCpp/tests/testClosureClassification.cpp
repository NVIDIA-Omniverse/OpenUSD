//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "../../../rendererImpl.h"

#include <cstdio>
#include <functional>
#include <utility>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

using namespace mxcpp;

// From testMaterialXCppMain.cpp.
void Test_Register(const char* name, std::function<bool()> fn);

#define _REG(name) Test_Register("ClosureClassification." #name, &name)

namespace {

struct _LeafCase
{
    const char* name;
    Bsdf::NodeData data;
    bool expected;
};

static bool
_Check(bool condition, const char* description)
{
    if (!condition) {
        std::printf("    failed: %s\n", description);
    }
    return condition;
}

static bool
_ClassifyLeaf(const Bsdf::NodeData& data)
{
    Bsdf::ClosureTree tree;
    tree.root = tree.Add(data);
    return _IsReflectionOnlyNode(tree, tree.root);
}

static Bsdf::NodeId
_AddReflectionOnly(Bsdf::ClosureTree* tree)
{
    return tree->Add(Bsdf::OrenNayarDiffuseData{});
}

static Bsdf::NodeId
_AddTransmissive(Bsdf::ClosureTree* tree)
{
    return tree->Add(Bsdf::TranslucentData{});
}

static bool
LeafKinds()
{
    Bsdf::TranslucentData translucentZero;
    translucentZero.weight = 0.0f;
    Bsdf::TranslucentData translucentAtZeroThreshold;
    translucentAtZeroThreshold.weight = _reflectionOnlyEps;
    Bsdf::TranslucentData translucentOutsideZeroThreshold;
    translucentOutsideZeroThreshold.weight = 2.0f * _reflectionOnlyEps;
    Bsdf::SubsurfaceData subsurfaceZero;
    subsurfaceZero.weight = 0.0f;

    Bsdf::DielectricData dielectricTransmission;
    dielectricTransmission.scatterMode = Bsdf::ScatterMode::Transmission;
    Bsdf::DielectricData dielectricReflectionTransmission;
    dielectricReflectionTransmission.scatterMode =
        Bsdf::ScatterMode::ReflectionTransmission;
    Bsdf::DielectricData dielectricZero;
    dielectricZero.weight = 0.0f;
    dielectricZero.scatterMode = Bsdf::ScatterMode::Transmission;

    Bsdf::DielectricInterfaceData dielectricInterfaceZero;
    dielectricInterfaceZero.transmissionWeight = 0.0f;

    Bsdf::GeneralizedSchlickData schlickTransmission;
    schlickTransmission.scatterMode = Bsdf::ScatterMode::Transmission;
    Bsdf::GeneralizedSchlickData schlickReflectionTransmission;
    schlickReflectionTransmission.scatterMode =
        Bsdf::ScatterMode::ReflectionTransmission;
    Bsdf::GeneralizedSchlickData schlickZero;
    schlickZero.weight = 0.0f;
    schlickZero.scatterMode = Bsdf::ScatterMode::Transmission;

    Bsdf::AdobeOpenPbrData openPbrTransparent;
    openPbrTransparent.geometryOpacity = 0.5f;
    Bsdf::AdobeOpenPbrData openPbrAtOpacityThreshold;
    openPbrAtOpacityThreshold.geometryOpacity =
        1.0f - _reflectionOnlyEps;
    Bsdf::AdobeOpenPbrData openPbrOutsideOpacityThreshold;
    openPbrOutsideOpacityThreshold.geometryOpacity =
        1.0f - 2.0f * _reflectionOnlyEps;
    Bsdf::AdobeOpenPbrData openPbrTransmissive;
    openPbrTransmissive.transmissionWeight = 0.5f;
    Bsdf::AdobeOpenPbrData openPbrSubsurface;
    openPbrSubsurface.subsurfaceWeight = 0.5f;

    const std::vector<_LeafCase> cases = {
        {"Oren-Nayar diffuse", Bsdf::OrenNayarDiffuseData{}, true},
        {"Burley diffuse", Bsdf::BurleyDiffuseData{}, true},
        {"translucent", Bsdf::TranslucentData{}, false},
        {"zero-weight translucent", translucentZero, true},
        {"translucent at zero threshold",
         translucentAtZeroThreshold, true},
        {"translucent outside zero threshold",
         translucentOutsideZeroThreshold, false},
        {"subsurface", Bsdf::SubsurfaceData{}, false},
        {"zero-weight subsurface", subsurfaceZero, true},
        {"dielectric reflection", Bsdf::DielectricData{}, true},
        {"dielectric transmission", dielectricTransmission, false},
        {"dielectric reflection/transmission",
         dielectricReflectionTransmission, false},
        {"zero-weight dielectric", dielectricZero, true},
        {"dielectric interface", Bsdf::DielectricInterfaceData{}, false},
        {"zero-transmission dielectric interface",
         dielectricInterfaceZero, true},
        {"conductor", Bsdf::ConductorData{}, true},
        {"Schlick reflection", Bsdf::GeneralizedSchlickData{}, true},
        {"Schlick transmission", schlickTransmission, false},
        {"Schlick reflection/transmission",
         schlickReflectionTransmission, false},
        {"zero-weight Schlick", schlickZero, true},
        {"sheen", Bsdf::SheenData{}, true},
        {"opaque OpenPBR", Bsdf::AdobeOpenPbrData{}, true},
        {"OpenPBR at opacity threshold",
         openPbrAtOpacityThreshold, true},
        {"OpenPBR outside opacity threshold",
         openPbrOutsideOpacityThreshold, false},
        {"transparent OpenPBR", openPbrTransparent, false},
        {"transmissive OpenPBR", openPbrTransmissive, false},
        {"subsurface OpenPBR", openPbrSubsurface, false},
        {"unsupported", Bsdf::UnsupportedData{}, false},
    };

    bool passed = true;
    for (const _LeafCase& testCase : cases) {
        passed &= _Check(
            _ClassifyLeaf(testCase.data) == testCase.expected,
            testCase.name);
    }
    return passed;
}

static bool
MixEndpointsAndInterior()
{
    bool passed = true;
    for (const bool foregroundReflectionOnly : {false, true}) {
        for (const bool backgroundReflectionOnly : {false, true}) {
            for (const float mix : {0.0f, 1.0f, 0.5f}) {
                Bsdf::ClosureTree tree;
                const Bsdf::NodeId foreground =
                    foregroundReflectionOnly
                    ? _AddReflectionOnly(&tree)
                    : _AddTransmissive(&tree);
                const Bsdf::NodeId background =
                    backgroundReflectionOnly
                    ? _AddReflectionOnly(&tree)
                    : _AddTransmissive(&tree);
                Bsdf::MixData data;
                data.fg = foreground;
                data.bg = background;
                data.mix = mix;
                tree.root = tree.Add(data);

                const bool expected =
                    mix == 0.0f
                    ? backgroundReflectionOnly
                    : mix == 1.0f
                    ? foregroundReflectionOnly
                    : foregroundReflectionOnly && backgroundReflectionOnly;
                passed &= _Check(
                    _IsReflectionOnlyNode(tree, tree.root) == expected,
                    "mix endpoint/interior child selection");
            }
        }
    }

    struct _MixThresholdCase
    {
        bool foregroundReflectionOnly;
        bool backgroundReflectionOnly;
        float mix;
        bool expected;
    };
    const std::vector<_MixThresholdCase> thresholdCases = {
        {false, true, _reflectionOnlyEps, true},
        {false, true, 2.0f * _reflectionOnlyEps, false},
        {true, false, 1.0f - _reflectionOnlyEps, true},
        {true, false, 1.0f - 2.0f * _reflectionOnlyEps, false},
    };
    for (const _MixThresholdCase& testCase : thresholdCases) {
        Bsdf::ClosureTree tree;
        const Bsdf::NodeId foreground =
            testCase.foregroundReflectionOnly
            ? _AddReflectionOnly(&tree)
            : _AddTransmissive(&tree);
        const Bsdf::NodeId background =
            testCase.backgroundReflectionOnly
            ? _AddReflectionOnly(&tree)
            : _AddTransmissive(&tree);
        Bsdf::MixData data;
        data.fg = foreground;
        data.bg = background;
        data.mix = testCase.mix;
        tree.root = tree.Add(data);
        passed &= _Check(
            _IsReflectionOnlyNode(tree, tree.root) == testCase.expected,
            "mix epsilon endpoint selection");
    }
    return passed;
}

static bool
CompositeKinds()
{
    bool passed = true;

    for (const bool firstReflectionOnly : {false, true}) {
        for (const bool secondReflectionOnly : {false, true}) {
            Bsdf::ClosureTree layerTree;
            Bsdf::LayerData layer;
            layer.top = firstReflectionOnly
                ? _AddReflectionOnly(&layerTree)
                : _AddTransmissive(&layerTree);
            layer.base = secondReflectionOnly
                ? _AddReflectionOnly(&layerTree)
                : _AddTransmissive(&layerTree);
            layerTree.root = layerTree.Add(layer);
            passed &= _Check(
                _IsReflectionOnlyNode(layerTree, layerTree.root) ==
                    (firstReflectionOnly && secondReflectionOnly),
                "layer requires both children to be reflection-only");

            Bsdf::ClosureTree addTree;
            Bsdf::AddData add;
            add.in1 = firstReflectionOnly
                ? _AddReflectionOnly(&addTree)
                : _AddTransmissive(&addTree);
            add.in2 = secondReflectionOnly
                ? _AddReflectionOnly(&addTree)
                : _AddTransmissive(&addTree);
            addTree.root = addTree.Add(add);
            passed &= _Check(
                _IsReflectionOnlyNode(addTree, addTree.root) ==
                    (firstReflectionOnly && secondReflectionOnly),
                "add requires both children to be reflection-only");
        }
    }

    for (const bool inputReflectionOnly : {false, true}) {
        Bsdf::ClosureTree tree;
        Bsdf::MultiplyData multiply;
        multiply.input = inputReflectionOnly
            ? _AddReflectionOnly(&tree)
            : _AddTransmissive(&tree);
        tree.root = tree.Add(multiply);
        passed &= _Check(
            _IsReflectionOnlyNode(tree, tree.root) == inputReflectionOnly,
            "multiply preserves its input classification");
    }

    return passed;
}

static bool
NestedComposite()
{
    Bsdf::ClosureTree tree;

    Bsdf::AddData add;
    add.in1 = _AddReflectionOnly(&tree);
    add.in2 = _AddReflectionOnly(&tree);
    const Bsdf::NodeId addId = tree.Add(add);

    Bsdf::LayerData layer;
    layer.top = addId;
    layer.base = _AddReflectionOnly(&tree);
    const Bsdf::NodeId layerId = tree.Add(layer);

    Bsdf::MixData mix;
    mix.fg = layerId;
    mix.bg = _AddTransmissive(&tree);
    mix.mix = 1.0f;
    tree.root = tree.Add(mix);

    const bool selectedReflection =
        _IsReflectionOnlyNode(tree, tree.root);
    std::get<Bsdf::MixData>(tree.nodes[tree.root].data).mix = 0.5f;
    const bool combinedTransmission =
        _IsReflectionOnlyNode(tree, tree.root);

    return _Check(
               selectedReflection,
               "nested selected reflection-only branch") &&
           _Check(
               !combinedTransmission,
               "nested interior mix includes transmissive branch");
}

static bool
InvalidNodes()
{
    Bsdf::ClosureTree emptyTree;
    bool passed = _Check(
        !_IsReflectionOnlyNode(emptyTree, Bsdf::InvalidNodeId),
        "empty tree invalid root");

    emptyTree.nodes.push_back(Bsdf::Node{Bsdf::OrenNayarDiffuseData{}});
    passed &= _Check(
        !_IsReflectionOnlyNode(emptyTree, 42),
        "out-of-range node id");
    return passed;
}

}  // namespace

void
Test_RegisterClosureClassificationTests()
{
    _REG(LeafKinds);
    _REG(MixEndpointsAndInterior);
    _REG(CompositeKinds);
    _REG(NestedComposite);
    _REG(InvalidNodes);
}
