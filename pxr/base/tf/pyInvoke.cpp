//
// Copyright 2021 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//

#include "pxr/pxr.h"

#include "pxr/base/tf/pyInvoke.h"

#include "pxr/base/tf/diagnosticLite.h"
#include "pxr/base/tf/pyError.h"
#include "pxr/base/tf/pyInterpreter.h"
#include "pxr/base/tf/pyInvokeImpl.h"
#include "pxr/base/tf/pyLock.h"

PXR_NAMESPACE_OPEN_SCOPE

bool
TfPyInvokeAndReturn(
    const std::string &moduleName,
    const std::string &callableExpr,
    PyObject *posArgs,
    PyObject *kwArgs,
    PyObject **resultObjOut)
{
    if (!resultObjOut) {
        TF_CODING_ERROR("Bad pointer to TfPyInvokeAndReturn");
        return false;
    }

    TfPyInitialize();
    TfPyLock lock;

    PyObject *ownedPosArgs = nullptr;
    PyObject *ownedKwArgs = nullptr;
    if (!posArgs) {
        ownedPosArgs = PyTuple_New(0);
        if (!ownedPosArgs) {
            TfPyConvertPythonExceptionToTfErrors();
            PyErr_Clear();
            return false;
        }
        posArgs = ownedPosArgs;
    }
    if (!kwArgs) {
        ownedKwArgs = PyDict_New();
        if (!ownedKwArgs) {
            Py_DECREF(ownedPosArgs);
            TfPyConvertPythonExceptionToTfErrors();
            PyErr_Clear();
            return false;
        }
        kwArgs = ownedKwArgs;
    }

    const bool result = Tf_PyInvokeImpl(
        moduleName, callableExpr, posArgs, kwArgs, resultObjOut);

    Py_XDECREF(ownedKwArgs);
    Py_XDECREF(ownedPosArgs);
    return result;
}

bool
TfPyInvoke(
    const std::string &moduleName,
    const std::string &callableExpr,
    PyObject *posArgs,
    PyObject *kwArgs)
{
    PyObject *result = nullptr;
    if (!TfPyInvokeAndReturn(
            moduleName, callableExpr, posArgs, kwArgs, &result)) {
        return false;
    }

    Py_DECREF(result);
    return true;
}

PXR_NAMESPACE_CLOSE_SCOPE
