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
TestSobolResetForBounceResetsDimension()
{
    HdEmbreeSampler sampler(
        1234u, 8u, 16u, 7u, HdEmbreeSamplerSequence::Sobol);

    sampler.Next();
    sampler.Next();
    sampler.Next();
    if (sampler.dimension != 3) {
        std::printf("    expected dimension 3 before reset, got %d\n",
                    sampler.dimension);
        return false;
    }

    sampler.ResetForBounce(0x12345678u);
    if (sampler.dimension != 0) {
        std::printf("    Sobol reset left dimension at %d\n",
                    sampler.dimension);
        return false;
    }

    sampler.Next();
    if (sampler.dimension != 1) {
        std::printf("    Sobol next after reset left dimension at %d\n",
                    sampler.dimension);
        return false;
    }

    return true;
}

bool
TestOpenQmcResetForBounceResetsDimension()
{
#if defined(PXR_HDEMBREE_ENABLE_OPENQMC)
    HdEmbreeSampler sampler(
        1234u, 8u, 16u, 7u, HdEmbreeSamplerSequence::OpenQMCSobolBN);

    sampler.Next();
    sampler.Next();
    sampler.Next();
    if (sampler.dimension != 3) {
        std::printf("    expected dimension 3 before reset, got %d\n",
                    sampler.dimension);
        return false;
    }

    sampler.ResetForBounce(0x12345678u);
    if (sampler.dimension != 0) {
        std::printf("    OpenQMC reset left dimension at %d\n",
                    sampler.dimension);
        return false;
    }

    sampler.Next();
    if (sampler.dimension != 1) {
        std::printf("    OpenQMC next after reset left dimension at %d\n",
                    sampler.dimension);
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
        {"Sampling.TestSobolResetForBounceResetsDimension",
         &TestSobolResetForBounceResetsDimension},
        {"Sampling.TestOpenQmcResetForBounceResetsDimension",
         &TestOpenQmcResetForBounceResetsDimension},
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
