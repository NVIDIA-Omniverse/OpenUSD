//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_BASE_TF_PY_LOCK_IMPL_H
#define PXR_BASE_TF_PY_LOCK_IMPL_H

#include "pxr/pxr.h"

#ifdef PXR_PYTHON_SUPPORT_ENABLED

#include "pxr/base/tf/pySafePython.h"

PXR_NAMESPACE_OPEN_SCOPE

inline bool
Tf_PyGilIsHeldByCurrentThread()
{
    if (!Py_IsInitialized()) {
        return false;
    }

#ifdef Py_LIMITED_API
    return PyThreadState_GetDict() != nullptr;
#else
    return PyGILState_Check();
#endif
}

PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_PYTHON_SUPPORT_ENABLED

#endif // PXR_BASE_TF_PY_LOCK_IMPL_H
