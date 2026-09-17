//
// Copyright 2016 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//

#include "pxr/pxr.h"

#include "pxr/base/tf/pyErrorInternal.h"

#include "pxr/base/tf/enum.h"
#include "pxr/base/tf/error.h"
#include "pxr/base/tf/iterator.h"
#include "pxr/base/tf/pySafePython.h"
#include "pxr/base/tf/registryManager.h"
#include "pxr/base/tf/staticData.h"

#include <cstdint>
#include <cstring>
#include <exception>

PXR_NAMESPACE_OPEN_SCOPE

TF_REGISTRY_FUNCTION(TfEnum) {
    TF_ADD_ENUM_NAME(TF_PYTHON_EXCEPTION);
}

// The held reference is intentionally leaked to avoid Python refcount ops
// during shutdown, which is unsafe if Python has been finalized.
static TfStaticData<PyObject *> _ExceptionClass;
static TfStaticData<Tf_PyErrorToPythonFn> _ErrorToPython;
static TfStaticData<Tf_PyAppendErrorsFromExceptionFn> _AppendErrorsFromException;

PyObject *
Tf_PyGetErrorExceptionClass()
{
    return *_ExceptionClass;
}

void
Tf_PySetErrorExceptionClass(PyObject *cls)
{
    Py_XINCREF(cls);
    *_ExceptionClass = cls;
}

void
Tf_PySetErrorExceptionHandlers(
    Tf_PyErrorToPythonFn errorToPython,
    Tf_PyAppendErrorsFromExceptionFn appendErrorsFromException)
{
    *_ErrorToPython = errorToPython;
    *_AppendErrorsFromException = appendErrorsFromException;
}

PyObject *
Tf_PyCreateErrorException(TfErrorMark const &m)
{
    PyObject *exceptionClass = Tf_PyGetErrorExceptionClass();
    Tf_PyErrorToPythonFn errorToPython = *_ErrorToPython;
    if (!exceptionClass || !errorToPython) {
        PyErr_SetString(
            PyExc_RuntimeError,
            "Tf.ErrorException Python bindings are not initialized");
        return nullptr;
    }

    Py_ssize_t numErrors = 0;
    for (TfErrorMark::Iterator e = m.GetBegin(); e != m.GetEnd(); ++e) {
        if (e->GetErrorCode() != TF_PYTHON_EXCEPTION) {
            ++numErrors;
        }
    }

    PyObject *args = PyTuple_New(numErrors);
    if (!args) {
        return nullptr;
    }

    Py_ssize_t index = 0;
    for (TfErrorMark::Iterator e = m.GetBegin(); e != m.GetEnd(); ++e) {
        if (e->GetErrorCode() == TF_PYTHON_EXCEPTION) {
            continue;
        }

        PyObject *errorObj = errorToPython(*e);
        if (!errorObj) {
            Py_DECREF(args);
            return nullptr;
        }

        if (PyTuple_SetItem(args, index++, errorObj) != 0) {
            Py_DECREF(errorObj);
            Py_DECREF(args);
            return nullptr;
        }
    }

    PyObject *exception = PyObject_CallObject(exceptionClass, args);
    Py_DECREF(args);
    return exception;
}

bool
Tf_PyAppendErrorsFromException(PyObject *exception)
{
    Tf_PyAppendErrorsFromExceptionFn appendErrors = *_AppendErrorsFromException;
    return appendErrors ? appendErrors(exception) : false;
}

void
Tf_PyRethrowSavedTfException(PyObject *exception)
{
    if (!PyObject_HasAttrString(exception, "_pxr_SavedTfException")) {
        return;
    }

    PyObject *attr = PyObject_GetAttrString(exception, "_pxr_SavedTfException");
    if (!attr) {
        PyErr_Clear();
        return;
    }

    uintptr_t addr = static_cast<uintptr_t>(PyLong_AsUnsignedLongLong(attr));
    Py_DECREF(attr);
    if (PyErr_Occurred()) {
        PyErr_Clear();
        return;
    }

    std::exception_ptr *excPtrPtr;
    memcpy(&excPtrPtr, &addr, sizeof(addr));
    std::exception_ptr eptr = *excPtrPtr;
    delete excPtrPtr;
    std::rethrow_exception(eptr);
}

TfPyExceptionStateScope::TfPyExceptionStateScope() :
    _state(TfPyExceptionState::Fetch())
{
}

TfPyExceptionStateScope::~TfPyExceptionStateScope()
{
    _state.Restore();
}

PXR_NAMESPACE_CLOSE_SCOPE
