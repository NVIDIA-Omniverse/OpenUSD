// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
#include "native.h"
#ifdef Py_PYTHON_H
#error Native context headers must not include Python.h
#endif

// Constructed as this native library loads, before any Python module imports.
static const PXR_NS::ArResolverContext early(LateContext(17));
const PXR_NS::ArResolverContext& GetEarlyContext() { return early; }

extern "C" CONTEXT_API int EarlyContextValue()
{
    return early.Get<LateContext>()->value;
}
