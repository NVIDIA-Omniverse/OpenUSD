//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_BASE_TF_PY_INVOKE_IMPL_H
#define PXR_BASE_TF_PY_INVOKE_IMPL_H

#include "pxr/pxr.h"

#include "pxr/base/tf/pySafePython.h"

#include <string>

PXR_NAMESPACE_OPEN_SCOPE

bool Tf_PyInvokeImpl(
    const std::string &moduleName,
    const std::string &callableExpr,
    PyObject *posArgs,
    PyObject *kwArgs,
    PyObject **resultObjOut);

PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_BASE_TF_PY_INVOKE_IMPL_H
