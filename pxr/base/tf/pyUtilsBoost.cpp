//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//

#include "pxr/pxr.h"

#if defined(PXR_PYTHON_SUPPORT_ENABLED) && !defined(Py_LIMITED_API)

#include "pxr/base/tf/pyErrorImpl.h"
#include "pxr/base/tf/pyUtils.h"

#include "pxr/external/boost/python/errors.hpp"

PXR_NAMESPACE_OPEN_SCOPE

void
Tf_PyThrowErrorAlreadySet()
{
    // Preserve Boost.Python behavior until binding boundaries translate
    // TfPyErrorAlreadySet to their own error propagation mechanism.
    pxr_boost::python::throw_error_already_set();
}

void
TfPyThrowIndexError(const char *msg)
{
    Tf_PySetIndexError(msg);
    Tf_PyThrowErrorAlreadySet();
}

void
TfPyThrowRuntimeError(const char *msg)
{
    Tf_PySetRuntimeError(msg);
    Tf_PyThrowErrorAlreadySet();
}

void
TfPyThrowStopIteration(const char *msg)
{
    Tf_PySetStopIteration(msg);
    Tf_PyThrowErrorAlreadySet();
}

void
TfPyThrowKeyError(const char *msg)
{
    Tf_PySetKeyError(msg);
    Tf_PyThrowErrorAlreadySet();
}

void
TfPyThrowValueError(const char *msg)
{
    Tf_PySetValueError(msg);
    Tf_PyThrowErrorAlreadySet();
}

void
TfPyThrowTypeError(const char *msg)
{
    Tf_PySetTypeError(msg);
    Tf_PyThrowErrorAlreadySet();
}

PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_PYTHON_SUPPORT_ENABLED && !Py_LIMITED_API
