//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//

#include "pxr/pxr.h"

#include "pxr/base/tf/pyUtilsImpl.h"

#ifdef PXR_PYTHON_SUPPORT_ENABLED

#include "pxr/base/tf/error.h"
#include "pxr/base/tf/pyLock.h"

PXR_NAMESPACE_OPEN_SCOPE

std::string
Tf_PyObjectRepr(PyObject *obj)
{
    if (!Py_IsInitialized()) {
        // CODE_COVERAGE_OFF
        TF_CODING_ERROR("Called TfPyRepr without python being initialized!");
        return "<error: python not initialized>";
        // CODE_COVERAGE_ON
    }

    TfPyLock pyLock;

    std::string reprString("<invalid repr>");
    if (!obj) {
        return reprString;
    }

    PyObject *repr = PyObject_Repr(obj);
    if (!repr) {
        PyErr_Clear();
        return reprString;
    }

    if (const char *reprChars = PyUnicode_AsUTF8(repr)) {
        reprString = reprChars;
    } else {
        PyErr_Clear();
    }
    Py_DECREF(repr);

    // Python's repr() for NaN and Inf are not valid python which evaluates
    // to themselves.  Special case them here to produce python which has
    // this property.  This is unpleasant since we're not producing the real
    // python repr, but we want everything coming out of here (if at all
    // possible) to have this property.
    if (reprString == "nan")
        reprString = "float('nan')";
    if (reprString == "inf")
        reprString = "float('inf')";
    if (reprString == "-inf")
        reprString = "-float('inf')";

    return reprString;
}

PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_PYTHON_SUPPORT_ENABLED
