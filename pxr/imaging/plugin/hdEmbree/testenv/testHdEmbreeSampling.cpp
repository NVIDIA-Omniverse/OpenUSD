//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/imaging/plugin/hdEmbree/sampling.h"

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
TestDomainKeyValuesAreStable()
{
    if (HdEmbreeSampleDomainKeyValue(
            HdEmbreeSampleDomainKey::CameraJitter) != 0x0010u) {
        std::printf("    CameraJitter key changed\n");
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
    if (HdEmbreeGetDefaultSamplerSequence(false) !=
        HdEmbreeSamplerSequence::Random) {
        std::printf("    disabled Sobol default did not choose random\n");
        return false;
    }

#if defined(PXR_HDEMBREE_ENABLE_OPENQMC)
    const HdEmbreeSamplerSequence expected =
        HdEmbreeSamplerSequence::OpenQMCSobolBN;
#else
    const HdEmbreeSamplerSequence expected = HdEmbreeSamplerSequence::Sobol;
#endif

    if (HdEmbreeGetDefaultSamplerSequence(true) != expected) {
        std::printf("    enabled Sobol default chose the wrong sequence\n");
        return false;
    }

    return true;
}

bool
TestSobolDomainsAreDeterministicAndSeparated()
{
    HdEmbreeSampler sampler(
        1234u, 8u, 16u, 7u, HdEmbreeSamplerSequence::Sobol);
    const HdEmbreeSampleDomain root = sampler.RootDomain();

    const GfVec2f cameraA =
        root.Fork(HdEmbreeSampleDomainKey::CameraJitter).Draw2D();
    const GfVec2f cameraB =
        root.Fork(HdEmbreeSampleDomainKey::CameraJitter).Draw2D();
    if (!_Same(cameraA, cameraB)) {
        std::printf("    same Sobol domain did not reproduce values\n");
        return false;
    }

    const GfVec2f direct =
        root.Fork(HdEmbreeSampleDomainKey::DirectLightSample).Draw2D();
    if (!_Different(cameraA, direct)) {
        std::printf("    Sobol domains shared the same 2D sample\n");
        return false;
    }

    return true;
}

bool
TestSobolSplitAndChainAreStable()
{
    HdEmbreeSampler sampler(
        4321u, 4u, 5u, 3u, HdEmbreeSamplerSequence::Sobol);
    const HdEmbreeSampleDomain root = sampler.RootDomain();

    const GfVec2f splitA =
        root.Split(HdEmbreeSampleDomainKey::DirectLightSample, 4, 1).Draw2D();
    const GfVec2f splitB =
        root.Split(HdEmbreeSampleDomainKey::DirectLightSample, 4, 1).Draw2D();
    const GfVec2f splitC =
        root.Split(HdEmbreeSampleDomainKey::DirectLightSample, 4, 2).Draw2D();
    if (!_Same(splitA, splitB) || !_Different(splitA, splitC)) {
        std::printf("    Sobol split domain was not stable by index\n");
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
        std::printf("    Sobol chained bounce domains matched\n");
        return false;
    }

    return true;
}

bool
TestRandomDomainsAreDeterministic()
{
    HdEmbreeSampler sampler(
        5678u, 11u, 13u, 2u, HdEmbreeSamplerSequence::Random);
    const HdEmbreeSampleDomain root = sampler.RootDomain();

    const float rrA =
        root.Chain(HdEmbreeSampleDomainKey::PathBounce, 2)
            .Fork(HdEmbreeSampleDomainKey::RussianRoulette)
            .Draw1D();
    const float rrB =
        root.Chain(HdEmbreeSampleDomainKey::PathBounce, 2)
            .Fork(HdEmbreeSampleDomainKey::RussianRoulette)
            .Draw1D();
    if (rrA != rrB) {
        std::printf("    random domain did not reproduce values\n");
        return false;
    }

    return true;
}

bool
TestOpenQmcDomainsAreDeterministicAndSeparated()
{
#if defined(PXR_HDEMBREE_ENABLE_OPENQMC)
    HdEmbreeSampler sampler(
        1234u, 8u, 16u, 7u, HdEmbreeSamplerSequence::OpenQMCSobolBN);
    const HdEmbreeSampleDomain root = sampler.RootDomain();

    const GfVec2f cameraA =
        root.Fork(HdEmbreeSampleDomainKey::CameraJitter).Draw2D();
    const GfVec2f cameraB =
        root.Fork(HdEmbreeSampleDomainKey::CameraJitter).Draw2D();
    const GfVec2f direct =
        root.Fork(HdEmbreeSampleDomainKey::DirectLightSample).Draw2D();

    if (!_Same(cameraA, cameraB) || !_Different(cameraA, direct)) {
        std::printf("    OpenQMC fork domains were not stable/separated\n");
        return false;
    }

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
#endif

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
        {"Sampling.TestSobolDomainsAreDeterministicAndSeparated",
         &TestSobolDomainsAreDeterministicAndSeparated},
        {"Sampling.TestSobolSplitAndChainAreStable",
         &TestSobolSplitAndChainAreStable},
        {"Sampling.TestRandomDomainsAreDeterministic",
         &TestRandomDomainsAreDeterministic},
        {"Sampling.TestOpenQmcDomainsAreDeterministicAndSeparated",
         &TestOpenQmcDomainsAreDeterministicAndSeparated},
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
