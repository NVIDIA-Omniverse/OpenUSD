// Copyright 2026 Pixar
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
#include "pxr/usd/pcp/cache.h"
#include "pxr/usd/pcp/iterator.h"
#include "pxr/usd/pcp/mapFunction.h"
#include "pxr/usd/pcp/node.h"
#include "pxr/usd/pcp/primIndex.h"
#include "pxr/usd/sdf/layer.h"
#include "pxr/usd/sdf/primSpec.h"
#include "pxr/usd/sdf/reference.h"
#ifdef PXR_PYTHON_SUPPORT_ENABLED
#include "pxr/base/tf/pyLock.h"
#endif
#include <cstdio>
#ifdef _WIN32
#include <windows.h>
#endif

PXR_NAMESPACE_USING_DIRECTIVE
int main()
{
    // Exercise native composition before Python starts, so automatic module
    // loading cannot silently supply the default bindings to this consumer.
    {
        auto source = SdfLayer::CreateAnonymous("source.usda");
        auto root = SdfLayer::CreateAnonymous("root.usda");
        if (!source || !root) return 1;
        auto model = SdfPrimSpec::New(source, "Model", SdfSpecifierDef);
        auto child = SdfPrimSpec::New(model, "Child", SdfSpecifierDef);
        auto prim = SdfPrimSpec::New(root, "Root", SdfSpecifierDef);
        if (!model || !child || !prim) return 2;
        prim->GetReferenceList().Add(SdfReference(source->GetIdentifier(), SdfPath("/Model")));
        PcpCache cache{PcpLayerStackIdentifier(root)};
        cache.SetVariantFallbacks({{"choice", {"one", "two"}}});
        if (cache.GetVariantFallbacks().at("choice").size() != 2) return 3;
        PcpErrorVector errors;
        const auto& index = cache.ComputePrimIndex(SdfPath("/Root/Child"), &errors);
        if (!errors.empty() || !index.HasSpecs() || index.GetPath() != SdfPath("/Root/Child"))
            return 4;
        bool foundReference = false;
        for (const auto& node : index.GetNodeRange()) {
            if (node.GetArcType() == PcpArcTypeReference && node.GetPath() == SdfPath("/Model/Child"))
                foundReference = true;
        }
        if (!foundReference || !cache.FindPrimIndex(SdfPath("/Root/Child"))) return 5;
    }
    if (PcpMapFunction::Identity().MapSourceToTarget(SdfPath("/Root")) != SdfPath("/Root"))
        return 6;
#ifdef PXR_PYTHON_SUPPORT_ENABLED
    Py_Initialize();
    TfPyLock lock;
    PyObject* number = PyLong_FromLong(42);
    if (!number || PyLong_AsLong(number) != 42) return 7;
    Py_DECREF(number);
    if (PyDict_GetItemString(PyImport_GetModuleDict(), "pxr.Pcp") ||
        PyDict_GetItemString(PyImport_GetModuleDict(), "pxr.Sdf")) return 8;
    std::puts(Py_GetVersion());
#elif defined(_WIN32)
    if (GetModuleHandleW(L"python3.dll") || GetModuleHandleW(L"python312.dll") ||
        GetModuleHandleW(L"python313.dll")) return 9;
#endif
    std::puts("PASS: native Pcp cache, reference composition and path mapping");
    return 0;
}
