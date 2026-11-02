// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
#ifndef TEST_AR_CONTEXT_NATIVE_H
#define TEST_AR_CONTEXT_NATIVE_H
#include "pxr/usd/ar/resolverContext.h"
#include "pxr/usd/ar/defineResolverContext.h"
#include "pxr/base/arch/export.h"

#ifdef AR_CONTEXT_NATIVE_EXPORTS
#define CONTEXT_API ARCH_EXPORT
#else
#define CONTEXT_API ARCH_IMPORT
#endif

struct LateContext {
    explicit LateContext(int value = 0) : value(value) {}
    bool operator==(const LateContext& rhs) const { return value == rhs.value; }
    bool operator<(const LateContext& rhs) const { return value < rhs.value; }
    friend size_t hash_value(const LateContext& ctx) { return ctx.value; }
    int value;
};
PXR_NAMESPACE_OPEN_SCOPE
AR_DECLARE_RESOLVER_CONTEXT(LateContext);
PXR_NAMESPACE_CLOSE_SCOPE

CONTEXT_API const PXR_NS::ArResolverContext& GetEarlyContext();
#endif
