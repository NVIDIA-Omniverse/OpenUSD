// Copyright 2026 Pixar
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
#include "native.h"
#include "pxr/usd/sdf/pySpecRegistry.h"
#include "pxr/usd/sdf/primSpec.h"
PXR_NAMESPACE_USING_DIRECTIVE
static PyObject* Create(const SdfSpec&) { return PyUnicode_FromString("registered in module A"); }
static PyObject* Register(PyObject*, PyObject*)
{
    Sdf_PySpecDetail::_RegisterHolderCreator(typeid(SdfPrimSpec), Create);
    Py_RETURN_NONE;
}
static PyMethodDef methods[] = {{"register", Register, METH_NOARGS, nullptr}, {nullptr}};
static PyModuleDef module = {PyModuleDef_HEAD_INIT, "sdfSpecRegister", nullptr, -1, methods};
PyMODINIT_FUNC PyInit_sdfSpecRegister() { return PyModule_Create(&module); }
