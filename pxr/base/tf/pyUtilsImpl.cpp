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

TF_API
void TfPyPrintError();

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

PyObject *
_GetOsEnviron()
{
    // In theory, we could just check that the os module has been imported,
    // rather than forcing an import ourself.  However, it's possible that
    // os.environ is actually a re-export from another module (ie. posix,
    // which is the case as of CPython 2.6) that may have been imported
    // without importing os.  Rather than check a hardcoded list of potential
    // modules, we always import os if Python is initialized.  If this turns
    // out to be problematic, we may want to consider the other approach.
    PyObject *module = PyImport_ImportModule("os");
    if (!module) {
        return nullptr;
    }

    PyObject *environObj = PyObject_GetAttrString(module, "environ");
    Py_DECREF(module);
    return environObj;
}

PyObject *
_PyStringFromStdString(const std::string &s)
{
    return PyUnicode_FromStringAndSize(
        s.data(), static_cast<Py_ssize_t>(s.size()));
}

}

TF_API
void Tf_PyLoadScriptModule(std::string const &moduleName)
{
    if (Py_IsInitialized()) {
        TfPyLock pyLock;
        PyObject *result = PyImport_ImportModule(moduleName.c_str());
        if (!result) {
            // CODE_COVERAGE_OFF
            TF_WARN("Import failed for module '%s'!", moduleName.c_str());
            TfPyPrintError();
            // CODE_COVERAGE_ON
        } else {
            Py_DECREF(result);
        }
    } else {
        // CODE_COVERAGE_OFF
        TF_WARN("Attempted to load module '%s' but Python is not initialized.",
                moduleName.c_str());
        // CODE_COVERAGE_ON
    }
}

TF_API
bool
TfPyIsInitialized()
{
    return Py_IsInitialized();
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
bool
TfPySetenv(const std::string & name, const std::string & value)
{
    if (!Py_IsInitialized()) {
        TF_CODING_ERROR("Python is uninitialized.");
        return false;
    }

    TfPyLock lock;

    PyObject *environObj = _GetOsEnviron();
    if (!environObj) {
        PyErr_Clear();
        return false;
    }

    PyObject *keyObj = _PyStringFromStdString(name);
    if (!keyObj) {
        Py_DECREF(environObj);
        PyErr_Clear();
        return false;
    }

    PyObject *valueObj = _PyStringFromStdString(value);
    if (!valueObj) {
        Py_DECREF(keyObj);
        Py_DECREF(environObj);
        PyErr_Clear();
        return false;
    }

    bool result = PyObject_SetItem(environObj, keyObj, valueObj) == 0;
    Py_DECREF(valueObj);
    Py_DECREF(keyObj);
    Py_DECREF(environObj);

    if (!result) {
        PyErr_Clear();
    }

    return result;
}

TF_API
bool
TfPyUnsetenv(const std::string & name)
{
    if (!Py_IsInitialized()) {
        TF_CODING_ERROR("Python is uninitialized.");
        return false;
    }

    TfPyLock lock;

    PyObject *environObj = _GetOsEnviron();
    if (!environObj) {
        PyErr_Clear();
        return false;
    }

    PyObject *keyObj = _PyStringFromStdString(name);
    if (!keyObj) {
        Py_DECREF(environObj);
        PyErr_Clear();
        return false;
    }

    const int contains = PySequence_Contains(environObj, keyObj);
    bool result = false;
    if (contains == 0) {
        result = true;
    } else if (contains > 0) {
        result = PyObject_DelItem(environObj, keyObj) == 0;
    }

    Py_DECREF(keyObj);
    Py_DECREF(environObj);

    if (!result) {
        PyErr_Clear();
    }

    return result;
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
