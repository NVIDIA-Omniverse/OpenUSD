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
#include "pxr/base/tf/pySafePython.h"

PXR_NAMESPACE_OPEN_SCOPE

TF_API bool Tf_PyNoticeInvokeCallback(
    PyObject *callable,
    PyObject *notice,
    PyObject *sender);

PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_BASE_TF_PY_NOTICE_CALLBACK_IMPL_H
