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
#include "pxr/base/tf/pyErrorImpl.h"
#include "pxr/base/tf/pyLock.h"
#include "pxr/base/tf/pyUtilsImpl.h"

#include <string>

PXR_NAMESPACE_OPEN_SCOPE

using std::string;

Tf_PyNoticeCallback::Tf_PyNoticeCallback(TfPyObjWrapper const &callback)
{
    TfPyLock lock;

    PyObject *pyCallable = callback.ptr();
    if (pyCallable == Py_None) {
        return;
    }

    if (!PyCallable_Check(pyCallable)) {
        Tf_PySetTypeError("Notice callback must be callable");
        Tf_PyThrowErrorAlreadySet();
    }

    PyObject *self = PyObject_GetAttrString(pyCallable, "__self__");
    if (!self) {
        PyErr_Clear();
    }
    PyObject *func = NULL;
    if (self && self != Py_None) {
        func = PyObject_GetAttrString(pyCallable, "__func__");
        if (!func) {
            PyErr_Clear();
        }
    }

    if (self && self != Py_None && func) {
        if (PyObject *weakSelf = PyWeakref_NewRef(self, NULL)) {
            _func = TfPyObjWrapper(func, TfPyNewReference);
            _weakSelf = TfPyObjWrapper(weakSelf, TfPyNewReference);
        } else {
            Py_DECREF(self);
            Py_DECREF(func);
            Tf_PyThrowErrorAlreadySet();
        }

        Py_DECREF(self);
        _mode = _Mode::Method;
    } else {
        Py_XDECREF(self);
        Py_XDECREF(func);

        if (_IsLambda(pyCallable)) {
            _callable = callback;
            _mode = _Mode::Strong;
        } else if (PyObject *weakCallable =
                       PyWeakref_NewRef(pyCallable, NULL)) {
            _weakCallable = TfPyObjWrapper(weakCallable, TfPyNewReference);
            _mode = _Mode::Weak;
        } else {
            PyErr_Clear();
            _callable = callback;
            _mode = _Mode::Strong;
        }
    }
}

void
Tf_PyNoticeCallback::Invoke(PyObject *notice, PyObject *sender) const
{
    TfPyLock lock;

    switch (_mode) {
    case _Mode::Empty:
        return;
    case _Mode::Strong:
        Tf_PyNoticeInvokeCallback(_callable.ptr(), notice, sender);
        return;
    case _Mode::Weak: {
        PyObject *callable = PyWeakref_GetObject(_weakCallable.ptr());
        if (callable == Py_None) {
            TF_WARN("Tried to call an expired python callback");
            return;
        }
        Tf_PyNoticeInvokeCallback(callable, notice, sender);
        return;
    }
    case _Mode::Method: {
        PyObject *self = PyWeakref_GetObject(_weakSelf.ptr());
        if (self == Py_None) {
            TF_WARN("Tried to call a method on an expired python instance");
            return;
        }

        PyObject *senderArg = sender ? sender : Py_None;
        PyObject *result = PyObject_CallFunctionObjArgs(
            _func.ptr(), self, notice, senderArg, nullptr);
        if (!result) {
            TfPyConvertPythonExceptionToTfErrors();
            PyErr_Clear();
            return;
        }

        Py_DECREF(result);
        return;
    }
    }
}

bool
Tf_PyNoticeCallback::_IsLambda(PyObject *callable)
{
    PyObject *name = PyObject_GetAttrString(callable, "__name__");
    if (!name) {
        PyErr_Clear();
        return false;
    }

    string nameStr;
    const bool result =
        Tf_PyUnicodeToStdString(name, &nameStr) && nameStr == "<lambda>";
    Py_DECREF(name);

    if (nameStr.empty() && PyErr_Occurred()) {
        PyErr_Clear();
    }

    return result;
}

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
