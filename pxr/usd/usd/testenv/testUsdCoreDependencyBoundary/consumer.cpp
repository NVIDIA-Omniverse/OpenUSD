// Copyright 2026 Pixar
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usd/prim.h"
#include "pxr/usd/usd/attribute.h"
#include "pxr/usd/usd/references.h"
#include "pxr/usd/usd/variantSets.h"
#include "pxr/usd/usd/editContext.h"
#include "pxr/usd/usd/timeCode.h"
#include "pxr/usd/sdf/types.h"
#include "pxr/base/vt/dictionary.h"
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
    // Native scene lifetime precedes Python initialization, so automatic
    // binding-module loading cannot supply an implicit dependency to this test.
    {
        auto source = UsdStage::CreateInMemory("source.usda");
        auto stage = UsdStage::CreateInMemory("root.usda");
        if (!source || !stage) return 1;
        auto child = source->DefinePrim(SdfPath("/Model/Child"));
        if (!child.CreateAttribute(TfToken("answer"), SdfValueTypeNames->Int).Set(42)) return 2;
        auto root = stage->DefinePrim(SdfPath("/Root"));
        if (!root.GetReferences().AddReference(source->GetRootLayer()->GetIdentifier(), SdfPath("/Model")))
            return 3;
        int value = 0;
        if (!stage->GetPrimAtPath(SdfPath("/Root/Child")).GetAttribute(TfToken("answer")).Get(&value) || value != 42)
            return 4;
        VtDictionary data;
        data["answer"] = VtValue(42);
        if (!root.SetMetadata(TfToken("customData"), data)) return 5;
        VtDictionary retrieved;
        if (!root.GetMetadata(TfToken("customData"), &retrieved) || retrieved != data) return 6;
        auto variant = root.GetVariantSets().AddVariantSet("choice");
        if (!variant.AddVariant("one") || !variant.SetVariantSelection("one")) return 7;
        {
            UsdEditContext context(variant.GetVariantEditContext());
            if (!stage->DefinePrim(SdfPath("/Root/VariantChild"))) return 8;
        }
        const auto path = SdfPath("/Root").AppendVariantSelection("choice", "one").AppendChild(TfToken("VariantChild"));
        if (!stage->GetRootLayer()->GetPrimAtPath(path)) return 9;
        std::string text;
        if (!stage->GetRootLayer()->ExportToString(&text)) return 10;
        auto layer = SdfLayer::CreateAnonymous("roundtrip.usda");
        if (!layer->ImportFromString(text)) return 11;
        auto copy = UsdStage::Open(layer);
        if (!copy || !copy->GetPrimAtPath(SdfPath("/Root/Child")).GetAttribute(TfToken("answer")).Get(&value) || value != 42)
            return 12;
    }
    if (UsdTimeCode(42).GetValue() != 42) return 13;
#ifdef PXR_PYTHON_SUPPORT_ENABLED
    Py_Initialize();
    TfPyLock lock;
    PyObject* number = PyLong_FromLong(42);
    if (!number || PyLong_AsLong(number) != 42) return 14;
    Py_DECREF(number);
    for (const char* module : {"pxr.Usd", "pxr.Sdf", "pxr.Pcp"})
        if (PyDict_GetItemString(PyImport_GetModuleDict(), module)) return 15;
    std::puts(Py_GetVersion());
#elif defined(_WIN32)
    if (GetModuleHandleW(L"python3.dll") || GetModuleHandleW(L"python312.dll") || GetModuleHandleW(L"python313.dll"))
        return 16;
#endif
    std::puts("PASS: native Usd stage, references, values, metadata, variants and serialization");
    return 0;
}
