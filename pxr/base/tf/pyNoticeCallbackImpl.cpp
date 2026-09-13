//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//

#include "pxr/pxr.h"

#include "pxr/base/tf/pyNoticeCallbackImpl.h"

#include "pxr/base/tf/diagnostic.h"
#include "pxr/base/tf/pyError.h"
#include "pxr/base/tf/pyLock.h"

PXR_NAMESPACE_OPEN_SCOPE

bool
Tf_PyNoticeInvokeCallback(PyObject *callable, PyObject *notice, PyObject *sender)
{
    if (!TF_VERIFY(callable) || !TF_VERIFY(notice)) {
        return false;
    }

    TfPyLock lock;

    if (PyErr_Occurred()) {
        return false;
    }

    PyObject *senderArg = sender ? sender : Py_None;
    PyObject *result = PyObject_CallFunctionObjArgs(
        callable, notice, senderArg, nullptr);
    if (!result) {
        TfPyConvertPythonExceptionToTfErrors();
        PyErr_Clear();
        return false;
    }

    Py_DECREF(result);
    return true;
}

PXR_NAMESPACE_CLOSE_SCOPE
