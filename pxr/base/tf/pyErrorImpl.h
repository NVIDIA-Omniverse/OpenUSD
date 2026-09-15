//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_BASE_TF_PY_ERROR_IMPL_H
#define PXR_BASE_TF_PY_ERROR_IMPL_H

#include "pxr/pxr.h"

#include "pxr/base/tf/api.h"

PXR_NAMESPACE_OPEN_SCOPE

void Tf_PySetIndexError(const char *msg);
void Tf_PySetRuntimeError(const char *msg);
void Tf_PySetStopIteration(const char *msg);
void Tf_PySetKeyError(const char *msg);
void Tf_PySetValueError(const char *msg);
void Tf_PySetTypeError(const char *msg);

TF_API void Tf_PyThrowErrorAlreadySet();

PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_BASE_TF_PY_ERROR_IMPL_H
