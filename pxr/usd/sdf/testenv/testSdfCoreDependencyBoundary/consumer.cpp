// Copyright 2026 Pixar
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
#include "pxr/usd/sdf/attributeSpec.h"
#include "pxr/usd/sdf/layer.h"
#include "pxr/usd/sdf/primSpec.h"
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
    // Native scene construction also exercises file-format plugin loading.
    // Do this before interpreter initialization so the test cannot silently
    // depend on the default binding module's automatic registration.
    auto layer = SdfLayer::CreateAnonymous("stable-core.usda");
    if (!layer) return 1;
    auto prim = SdfPrimSpec::New(layer, "Root", SdfSpecifierDef, "Scope");
    if (!prim) return 2;
    auto attr = SdfAttributeSpec::New(prim, "answer", SdfValueTypeNames->Int);
    if (!attr || !attr->SetDefaultValue(VtValue(42))) return 3;
    std::string text;
    if (!layer->ExportToString(&text) || text.find("answer = 42") == std::string::npos)
        return 4;
    auto copy = SdfLayer::CreateAnonymous("copy.usda");
    if (!copy || !copy->ImportFromString(text)) return 5;
    auto copiedAttr = copy->GetAttributeAtPath(SdfPath("/Root.answer"));
    if (!copiedAttr || copiedAttr->GetDefaultValue() != VtValue(42)) return 5;
    VtDictionary dictionary;
    dictionary["answer"] = VtValue(42);
    std::string error;
    if (!SdfConvertToValidMetadataDictionary(&dictionary, &error) || !error.empty())
        return 6;
    // Destroy scene objects before initializing Python: native teardown should
    // not trigger automatic binding-module loading either.
    attr.Reset();
    copiedAttr.Reset();
    prim.Reset();
    copy.Reset();
    layer.Reset();
#ifdef PXR_PYTHON_SUPPORT_ENABLED
    Py_Initialize();
    TfPyLock lock;
    PyObject* number = PyLong_FromLong(42);
    if (!number || PyLong_AsLong(number) != 42) return 7;
    Py_DECREF(number);
    if (!SdfConvertToValidMetadataDictionary(&dictionary, &error)) return 8;
    if (PyDict_GetItemString(PyImport_GetModuleDict(), "pxr.Sdf")) return 9;
    std::puts(Py_GetVersion());
#elif defined(_WIN32)
    if (GetModuleHandleW(L"python3.dll") || GetModuleHandleW(L"python312.dll") ||
        GetModuleHandleW(L"python313.dll")) return 10;
#endif
    return 0;
}
