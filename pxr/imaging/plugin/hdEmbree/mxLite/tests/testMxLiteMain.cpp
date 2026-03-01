//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/pxr.h"
#include "pxr/base/gf/vec3f.h"

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

// ---------------------------------------------------------------------------
// Minimal test framework
// ---------------------------------------------------------------------------

static int _totalTests  = 0;
static int _failedTests = 0;

struct _TestEntry {
    std::string name;
    std::function<bool()> fn;
};

static std::vector<_TestEntry>& _Tests() {
    static std::vector<_TestEntry> tests;
    return tests;
}

void
MxLiteTest_Register(const char* name, std::function<bool()> fn)
{
    _Tests().push_back({name, std::move(fn)});
}

bool
MxLiteTest_IsClose(float a, float b, float eps)
{
    return std::fabs(a - b) <= eps;
}

bool
MxLiteTest_IsClose(const GfVec3f& a, const GfVec3f& b, float eps)
{
    return std::fabs(a[0]-b[0]) <= eps &&
           std::fabs(a[1]-b[1]) <= eps &&
           std::fabs(a[2]-b[2]) <= eps;
}

// Declared in individual test files.
void MxLiteTest_RegisterBsdfTests();
void MxLiteTest_RegisterNodeTests();
void MxLiteTest_RegisterMaterialTests();
void MxLiteTest_RegisterGraphTests();

int
main(int /*argc*/, char** /*argv*/)
{
    MxLiteTest_RegisterBsdfTests();
    MxLiteTest_RegisterNodeTests();
    MxLiteTest_RegisterMaterialTests();
    MxLiteTest_RegisterGraphTests();

    for (const auto& entry : _Tests()) {
        _totalTests++;
        printf("  [RUN ] %s\n", entry.name.c_str());
        bool passed = false;
        try {
            passed = entry.fn();
        } catch (const std::exception& e) {
            printf("  [EXCEPTION] %s: %s\n", entry.name.c_str(), e.what());
        }
        if (passed) {
            printf("  [PASS] %s\n", entry.name.c_str());
        } else {
            printf("  [FAIL] %s\n", entry.name.c_str());
            _failedTests++;
        }
    }

    printf("\n%d/%d tests passed.\n", _totalTests - _failedTests, _totalTests);
    if (_failedTests > 0) {
        printf("%d test(s) FAILED.\n", _failedTests);
        return 1;
    }
    printf("All tests passed.\n");
    return 0;
}
