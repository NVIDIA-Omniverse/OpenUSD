//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_BASE_TF_PY_UTILS_IMPL_H
#define PXR_BASE_TF_PY_UTILS_IMPL_H

#include "pxr/pxr.h"

#ifdef PXR_PYTHON_SUPPORT_ENABLED

#include "pxr/base/tf/api.h"
#include "pxr/base/tf/pySafePython.h"

#include <string>

PXR_NAMESPACE_OPEN_SCOPE

TF_API bool TfPyIsInitialized();

std::string Tf_PyObjectRepr(PyObject *obj);
std::string Tf_PyGetClassName(PyObject *obj);
PyObject *Tf_PyCopyBufferToByteArray(const char *buffer, size_t size);

PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_PYTHON_SUPPORT_ENABLED

#endif // PXR_BASE_TF_PY_UTILS_IMPL_H
