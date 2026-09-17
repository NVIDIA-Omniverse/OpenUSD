//
// Copyright 2021 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_BASE_TF_PY_INVOKE_H
#define PXR_BASE_TF_PY_INVOKE_H

/// \file
/// Flexible, high-level interface for calling Python functions.

#include "pxr/pxr.h"
#include "pxr/base/tf/api.h"
#include "pxr/base/tf/pySafePython.h"

#include <string>

PXR_NAMESPACE_OPEN_SCOPE

/// Call a Python function and return its Python result object.
///
/// \p moduleName is imported directly. \p callableExpr is appended to the
/// module name with a dot and evaluated as the callable to invoke.
///
/// \p posArgs must be a Python tuple or list. \p kwArgs must be a Python dict.
/// Null \p posArgs and \p kwArgs are treated as empty tuple/dict respectively.
///
/// On success, \p resultObjOut receives a new reference. On failure, false is
/// returned and any Python exception has been converted to TfErrors.
TF_API
bool TfPyInvokeAndReturn(
    const std::string &moduleName,
    const std::string &callableExpr,
    PyObject *posArgs,
    PyObject *kwArgs,
    PyObject **resultObjOut);

/// Call a Python function and ignore its result.
TF_API
bool TfPyInvoke(
    const std::string &moduleName,
    const std::string &callableExpr,
    PyObject *posArgs,
    PyObject *kwArgs);

PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_BASE_TF_PY_INVOKE_H
