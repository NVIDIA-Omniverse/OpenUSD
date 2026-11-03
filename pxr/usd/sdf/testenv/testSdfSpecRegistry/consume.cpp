// Copyright 2026 Pixar
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
#include "native.h"
#include "pxr/usd/sdf/pySpecRegistry.h"
PXR_NAMESPACE_USING_DIRECTIVE
static PyObject* Get(PyObject*, PyObject*)
{
    return Sdf_PySpecDetail::_CreateHolder(typeid(SdfSpec), NativePrim());
}
static PyMethodDef methods[] = {{"get", Get, METH_NOARGS, nullptr}, {nullptr}};
static PyModuleDef module = {PyModuleDef_HEAD_INIT, "sdfSpecConsume", nullptr, -1, methods};
PyMODINIT_FUNC PyInit_sdfSpecConsume() { return PyModule_Create(&module); }
