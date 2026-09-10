//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//

#include "pxr/pxr.h"

#include "pxr/base/tf/pyErrorImpl.h"

#ifdef PXR_PYTHON_SUPPORT_ENABLED

#include "pxr/base/tf/pySafePython.h"

PXR_NAMESPACE_OPEN_SCOPE

void
Tf_PySetIndexError(const char *msg)
{
    PyErr_SetString(PyExc_IndexError, msg);
}

void
Tf_PySetRuntimeError(const char *msg)
{
    PyErr_SetString(PyExc_RuntimeError, msg);
}

void
Tf_PySetStopIteration(const char *msg)
{
    PyErr_SetString(PyExc_StopIteration, msg);
}

void
Tf_PySetKeyError(const char *msg)
{
    PyErr_SetString(PyExc_KeyError, msg);
}

void
Tf_PySetValueError(const char *msg)
{
    PyErr_SetString(PyExc_ValueError, msg);
}

void
Tf_PySetTypeError(const char *msg)
{
    PyErr_SetString(PyExc_TypeError, msg);
}

PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_PYTHON_SUPPORT_ENABLED
