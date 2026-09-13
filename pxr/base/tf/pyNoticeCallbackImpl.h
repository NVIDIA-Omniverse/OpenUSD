//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_BASE_TF_PY_NOTICE_CALLBACK_IMPL_H
#define PXR_BASE_TF_PY_NOTICE_CALLBACK_IMPL_H

#include "pxr/pxr.h"

#include "pxr/base/tf/api.h"
#include "pxr/base/tf/pyObjWrapper.h"
#include "pxr/base/tf/pySafePython.h"

PXR_NAMESPACE_OPEN_SCOPE

class Tf_PyNoticeCallback
{
public:
    Tf_PyNoticeCallback() = default;

    TF_API explicit Tf_PyNoticeCallback(TfPyObjWrapper const &callback);

    TF_API void Invoke(PyObject *notice, PyObject *sender) const;

private:
    enum class _Mode
    {
        Empty,
        Strong,
        Weak,
        Method
    };

    static bool _IsLambda(PyObject *callable);

    _Mode _mode = _Mode::Empty;
    TfPyObjWrapper _callable;
    TfPyObjWrapper _weakCallable;
    TfPyObjWrapper _func;
    TfPyObjWrapper _weakSelf;
};

TF_API bool Tf_PyNoticeInvokeCallback(
    PyObject *callable,
    PyObject *notice,
    PyObject *sender);

PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_BASE_TF_PY_NOTICE_CALLBACK_IMPL_H
