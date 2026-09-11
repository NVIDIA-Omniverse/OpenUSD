//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//

#include "pxr/pxr.h"

#include "pxr/base/tf/pyUtilsImpl.h"

#ifdef PXR_PYTHON_SUPPORT_ENABLED

#include "pxr/base/tf/api.h"
#include "pxr/base/tf/error.h"
#include "pxr/base/tf/pyError.h"
#include "pxr/base/tf/pyExceptionState.h"
#include "pxr/base/tf/pyLock.h"

#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

void
_ConvertCurrentPythonExceptionToTfErrors()
{
    if (PyErr_Occurred()) {
        TfPyConvertPythonExceptionToTfErrors();
    }
}

class _RestorePythonExceptionState
{
public:
    _RestorePythonExceptionState()
        : _state(TfPyExceptionState::Fetch())
    {
    }

    ~_RestorePythonExceptionState()
    {
        _state.Restore();
    }

private:
    TfPyExceptionState _state;
};

}

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

std::string
Tf_PyGetClassName(PyObject *obj)
{
    TfPyLock pyLock;

    if (obj) {
        PyObject *classObject = PyObject_GetAttrString(obj, "__class__");
        if (classObject) {
            PyObject *typeNameObject =
                PyObject_GetAttrString(classObject, "__name__");
            Py_DECREF(classObject);

            if (typeNameObject) {
                if (const char *typeName = PyUnicode_AsUTF8(typeNameObject)) {
                    std::string result(typeName);
                    Py_DECREF(typeNameObject);
                    return result;
                }
                Py_DECREF(typeNameObject);
            }
        }
    }

    PyErr_Clear();

    // CODE_COVERAGE_OFF This shouldn't really happen.
    TF_WARN("Couldn't get class name for python object '%s'",
            Tf_PyObjectRepr(obj).c_str());
    return "<unknown>";
    // CODE_COVERAGE_ON
}

PyObject *
Tf_PyCopyBufferToByteArray(const char *buffer, size_t size)
{
    TfPyLock lock;
    return PyByteArray_FromStringAndSize(buffer, size);
}

TF_API
std::vector<std::string>
TfPyGetTraceback()
{
    std::vector<std::string> result;

    if (!Py_IsInitialized()) {
        return result;
    }

    TfPyLock lock;
    // Save the exception state so we can restore it -- getting a traceback
    // should not affect the exception state.
    _RestorePythonExceptionState restoreExceptionState;

    PyObject *tbModule = PyImport_ImportModule("traceback");
    if (!tbModule) {
        _ConvertCurrentPythonExceptionToTfErrors();
        return result;
    }

    PyObject *formatStack = PyObject_GetAttrString(tbModule, "format_stack");
    Py_DECREF(tbModule);
    if (!formatStack) {
        _ConvertCurrentPythonExceptionToTfErrors();
        return result;
    }

    PyObject *stack = PyObject_CallFunctionObjArgs(formatStack, nullptr);
    Py_DECREF(formatStack);
    if (!stack) {
        _ConvertCurrentPythonExceptionToTfErrors();
        return result;
    }

    Py_ssize_t size = PySequence_Size(stack);
    if (size < 0) {
        Py_DECREF(stack);
        _ConvertCurrentPythonExceptionToTfErrors();
        return result;
    }

    result.reserve(static_cast<size_t>(size));
    for (Py_ssize_t i = 0; i != size; ++i) {
        PyObject *item = PySequence_GetItem(stack, i);
        if (!item) {
            _ConvertCurrentPythonExceptionToTfErrors();
            break;
        }

        if (const char *itemStr = PyUnicode_AsUTF8(item)) {
            result.push_back(itemStr);
        } else {
            Py_DECREF(item);
            _ConvertCurrentPythonExceptionToTfErrors();
            break;
        }
        Py_DECREF(item);
    }

    Py_DECREF(stack);
    return result;
}

TF_API
void
TfPyPrintError()
{
    if (!PyErr_ExceptionMatches(PyExc_KeyboardInterrupt)) {
        PyErr_Print();
    }
}

TF_API
void
Tf_PyObjectError(bool printError)
{
    // Silently pass these exceptions through.
    if (PyErr_ExceptionMatches(PyExc_SystemExit) ||
        PyErr_ExceptionMatches(PyExc_KeyboardInterrupt)) {
        return;
    }

    // Report and clear.
    if (printError) {
        PyErr_Print();
    }
    else {
        PyErr_Clear();
    }
}

PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_PYTHON_SUPPORT_ENABLED
