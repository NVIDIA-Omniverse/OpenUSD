// Copyright 2026 Pixar
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
#include "pxr/usd/ar/asset.h"
#include "pxr/usd/ar/defaultResolverContext.h"
#include "pxr/usd/ar/resolver.h"
#include "pxr/usd/ar/resolverContext.h"
#include "pxr/usd/ar/resolverContextBinder.h"
#include "pxr/usd/ar/resolverScopedCache.h"
#ifdef PXR_PYTHON_SUPPORT_ENABLED
#include "pxr/base/tf/pyLock.h"
#endif
#include <cstdio>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

PXR_NAMESPACE_USING_DIRECTIVE
int main(int argc, char** argv)
{
    if (argc < 1) return 1;
    ArDefaultResolverContext searchContext(std::vector<std::string>{"."});
    ArResolverContext context(searchContext);
    ArResolverContext copy(std::vector<ArResolverContext>{context, context});
    if (context.IsEmpty() || copy != context ||
        hash_value(copy) != hash_value(context) ||
        !copy.Get<ArDefaultResolverContext>() ||
        copy.GetDebugString().empty()) return 2;
    {
        ArResolverContextBinder binder(context);
        ArResolverScopedCache cache;
        auto& resolver = ArGetResolver();
        const auto path = resolver.Resolve(argv[0]);
        if (path.empty()) return 3;
        const auto asset = resolver.OpenAsset(path);
        char byte = 0;
        if (!asset || !asset->GetSize() || asset->Read(&byte, 1, 0) != 1) return 4;
    }
#ifdef PXR_PYTHON_SUPPORT_ENABLED
    // Exercise the stable Python dependency through the core target alone.
    // No binding module is imported, and context operations above need no GIL.
    Py_Initialize();
    TfPyLock lock;
    PyObject* number = PyLong_FromLong(42);
    if (!number || PyLong_AsLong(number) != 42) return 5;
    Py_DECREF(number);
    if (PyDict_GetItemString(PyImport_GetModuleDict(), "pxr.Ar")) return 6;
    std::puts(Py_GetVersion());
#elif defined(_WIN32)
    if (GetModuleHandleW(L"python3.dll") || GetModuleHandleW(L"python312.dll") ||
        GetModuleHandleW(L"python313.dll")) return 7;
#endif
    return 0;
}
