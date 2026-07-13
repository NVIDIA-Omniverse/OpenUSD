//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/imaging/plugin/hdEmbree/renderer/sampling.h"

#include <cstdio>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

bool
_Same(GfVec2f const& a, GfVec2f const& b)
{
    return a[0] == b[0] && a[1] == b[1];
}

bool
_Same(GfVec3f const& a, GfVec3f const& b)
{
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
}

bool
_Different(GfVec2f const& a, GfVec2f const& b)
{
    return !_Same(a, b);
}

bool
_Different(GfVec3f const& a, GfVec3f const& b)
{
    return !_Same(a, b);
}

bool
_InUnitInterval(GfVec4f const& sample)
{
    for (int i = 0; i < 4; ++i) {
        if (sample[i] < 0.0f || sample[i] >= 1.0f) {
            return false;
        }
    }
    return true;
}

bool
TestDomainKeyValuesAreStable()
{
    if (HdEmbreeSampleDomainKeyValue(
            HdEmbreeSampleDomainKey::CameraJitter) != 0x0010u) {
        std::printf("    CameraJitter key changed\n");
        return false;
    }
    if (HdEmbreeSampleDomainKeyValue(
            HdEmbreeSampleDomainKey::CameraLens) != 0x0011u) {
        std::printf("    CameraLens key changed\n");
        return false;
    }
    if (HdEmbreeSampleDomainKeyValue(
            HdEmbreeSampleDomainKey::PathBounce) != 0x0100u) {
        std::printf("    PathBounce key changed\n");
        return false;
    }
    if (HdEmbreeSampleDomainKeyValue(
            HdEmbreeSampleDomainKey::SssFreeFlight) != 0x0324u) {
        std::printf("    SssFreeFlight key changed\n");
        return false;
    }

    return true;
}

bool
TestDefaultSamplerSequence()
{
    if (HdEmbreeGetDefaultSamplerSequence() !=
        HdEmbreeSamplerSequence::OpenQMCSobolBN) {
        std::printf("    default sampler sequence was unexpected\n");
        return false;
    }

    return true;
}

bool
TestSamplerSequenceTokens()
{
    const HdEmbreeSamplerSequence sequences[] = {
        HdEmbreeSamplerSequence::OpenQMCSobol,
        HdEmbreeSamplerSequence::OpenQMCSobolBN,
        HdEmbreeSamplerSequence::OpenQMCPMJ,
        HdEmbreeSamplerSequence::OpenQMCPMJBN,
        HdEmbreeSamplerSequence::OpenQMCLattice,
        HdEmbreeSamplerSequence::OpenQMCLatticeBN,
    };

    for (HdEmbreeSamplerSequence sequence : sequences) {
        const TfToken token = HdEmbreeGetSamplerSequenceToken(sequence);
        if (HdEmbreeGetSamplerSequenceFromToken(token) != sequence) {
            std::printf("    sampler sequence token did not round-trip: %s\n",
                        token.GetText());
            return false;
        }
    }

    if (HdEmbreeGetSamplerSequenceFromToken(TfToken("unknown")) !=
        HdEmbreeGetDefaultSamplerSequence()) {
        std::printf("    unknown token did not map to default\n");
        return false;
    }

    return true;
}

bool
TestFrameSeedUsesSceneFrameUnlessOverridden()
{
    if (HdEmbreeResolveFrameSeed(-1, 24.0f) != 0x41c00000u ||
        HdEmbreeResolveFrameSeed(-1, 24.5f) != 0x41c40000u) {
        std::printf("    default seed did not preserve scene frame bits\n");
        return false;
    }
    if (HdEmbreeResolveFrameSeed(91, 24.0f) != 91u) {
        std::printf("    configured seed did not override the scene frame\n");
        return false;
    }
    return true;
}

bool
TestOpenQmcDomainsAreDeterministicAndSeparated()
{
    HdEmbreeSampler sampler(
        1234u, 8u, 16u, 7u, HdEmbreeSamplerSequence::OpenQMCSobolBN);
    const HdEmbreeSampleDomain root = sampler.RootDomain();

    const GfVec2f cameraA =
        root.Fork(HdEmbreeSampleDomainKey::CameraJitter).Draw2D();
    const GfVec2f cameraB =
        root.Fork(HdEmbreeSampleDomainKey::CameraJitter).Draw2D();
    const GfVec2f direct =
        root.Fork(HdEmbreeSampleDomainKey::DirectLightSample).Draw2D();
    const GfVec2f lens =
        root.Fork(HdEmbreeSampleDomainKey::CameraLens).Draw2D();

    if (!_Same(cameraA, cameraB) ||
        !_Different(cameraA, direct) ||
        !_Different(cameraA, lens)) {
        std::printf("    OpenQMC fork domains were not stable/separated\n");
        return false;
    }

    return true;
}

bool
TestOpenQmcSplitAndChainAreStable()
{
    HdEmbreeSampler sampler(
        4321u, 4u, 5u, 3u, HdEmbreeSamplerSequence::OpenQMCSobolBN);
    const HdEmbreeSampleDomain root = sampler.RootDomain();

    const GfVec2f splitA =
        root.Split(HdEmbreeSampleDomainKey::DirectLightSample, 4, 1).Draw2D();
    const GfVec2f splitB =
        root.Split(HdEmbreeSampleDomainKey::DirectLightSample, 4, 1).Draw2D();
    const GfVec2f splitC =
        root.Split(HdEmbreeSampleDomainKey::DirectLightSample, 4, 2).Draw2D();
    if (!_Same(splitA, splitB) || !_Different(splitA, splitC)) {
        std::printf("    OpenQMC split domain was not stable by index\n");
        return false;
    }

    const GfVec3f bounce0 =
        root.Chain(HdEmbreeSampleDomainKey::PathBounce, 0)
            .Fork(HdEmbreeSampleDomainKey::BsdfSample)
            .Draw3D();
    const GfVec3f bounce1 =
        root.Chain(HdEmbreeSampleDomainKey::PathBounce, 1)
            .Fork(HdEmbreeSampleDomainKey::BsdfSample)
            .Draw3D();
    if (!_Different(bounce0, bounce1)) {
        std::printf("    OpenQMC chained bounce domains matched\n");
        return false;
    }

    return true;
}

bool
TestAllOpenQmcSequencesDrawSamples()
{
    const HdEmbreeSamplerSequence sequences[] = {
        HdEmbreeSamplerSequence::OpenQMCSobol,
        HdEmbreeSamplerSequence::OpenQMCSobolBN,
        HdEmbreeSamplerSequence::OpenQMCPMJ,
        HdEmbreeSamplerSequence::OpenQMCPMJBN,
        HdEmbreeSamplerSequence::OpenQMCLattice,
        HdEmbreeSamplerSequence::OpenQMCLatticeBN,
    };

    for (HdEmbreeSamplerSequence sequence : sequences) {
        HdEmbreeSampler sampler(2468u, 2u, 3u, 4u, sequence);
        const GfVec4f sample =
            sampler.RootDomain()
                .Fork(HdEmbreeSampleDomainKey::CameraJitter)
                .Draw4D();
        if (!_InUnitInterval(sample)) {
            std::printf("    OpenQMC sequence produced out-of-range sample: %s\n",
                        HdEmbreeGetSamplerSequenceToken(sequence).GetText());
            return false;
        }
    }

    return true;
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
        {"Sampling.TestDomainKeyValuesAreStable",
         &TestDomainKeyValuesAreStable},
        {"Sampling.TestDefaultSamplerSequence",
         &TestDefaultSamplerSequence},
        {"Sampling.TestSamplerSequenceTokens",
         &TestSamplerSequenceTokens},
        {"Sampling.TestFrameSeedUsesSceneFrameUnlessOverridden",
         &TestFrameSeedUsesSceneFrameUnlessOverridden},
        {"Sampling.TestOpenQmcDomainsAreDeterministicAndSeparated",
         &TestOpenQmcDomainsAreDeterministicAndSeparated},
        {"Sampling.TestOpenQmcSplitAndChainAreStable",
         &TestOpenQmcSplitAndChainAreStable},
        {"Sampling.TestAllOpenQmcSequencesDrawSamples",
         &TestAllOpenQmcSequencesDrawSamples},
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
