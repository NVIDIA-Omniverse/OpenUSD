//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/base/vt/array.h"
#include "pxr/base/vt/dictionary.h"
#include "pxr/base/vt/value.h"
#include "pxr/base/vt/valueRef.h"
#include "pxr/base/vt/valueFromPython.h"
#include "pxr/base/vt/valueToPython.h"
#include "pxr/base/tf/pyLock.h"
#include <cstdio>

PXR_NAMESPACE_USING_DIRECTIVE
static VtValue FromPython(PyObject *object) {
    return PyLong_Check(object) ? VtValue(int(PyLong_AsLong(object))) : VtValue();
}
static PyObject *ToPython(void const *value) {
    return PyLong_FromLong(*static_cast<int const *>(value));
}
int main() {
    VtDoubleArray array{1, 2, 3};
    VtValue value(array);
    VtValue copy(value);
    array[0] = 7;
    if (copy.Get<VtDoubleArray>()[0] != 1) return 1;
    VtDictionary dictionary;
    dictionary["number"] = VtValue(42);
    if (dictionary["number"].Get<int>() != 42) return 2;
    if (VtValue::Cast<double>(VtValue(3)).Get<double>() != 3) return 3;

    Py_Initialize();
    TfPyLock lock;
    Vt_ValueFromPythonRegistry::Register(FromPython);
    Vt_ValueToPythonRegistry::Register(typeid(int), ToPython);
    TfPyObjWrapper input(PyLong_FromLong(42), TfPyNewReference);
    VtValue result = Vt_ValueFromPythonRegistry::Invoke(input.ptr());
    if (!result.IsHolding<int>() || result.Get<int>() != 42) return 4;
    TfPyObjWrapper output = Vt_GetPythonObjectFromHeldValue(VtValueRef(result));
    if (PyErr_Occurred() || PyLong_AsLong(output.ptr()) != 42) return 5;
    if (PyDict_GetItemString(PyImport_GetModuleDict(), "pxr.Vt")) return 6;
    std::puts(Py_GetVersion());
    return 0;
}
