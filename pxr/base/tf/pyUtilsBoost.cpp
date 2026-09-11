//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//

#include "pxr/pxr.h"

#if defined(PXR_PYTHON_SUPPORT_ENABLED) && !defined(Py_LIMITED_API)

#include "pxr/base/tf/pyErrorImpl.h"
#include "pxr/base/tf/pyError.h"
#include "pxr/base/tf/pyLock.h"
#include "pxr/base/tf/pyUtils.h"
#include "pxr/base/tf/pyUtilsImpl.h"

#include "pxr/external/boost/python/errors.hpp"
#include "pxr/external/boost/python/handle.hpp"
#include "pxr/external/boost/python/object.hpp"

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

std::string
TfPyObjectRepr(pxr_boost::python::object const &t)
{
    return Tf_PyObjectRepr(t.ptr());
}

std::string
TfPyGetClassName(pxr_boost::python::object const &obj)
{
    return Tf_PyGetClassName(obj.ptr());
}

pxr_boost::python::object
TfPyCopyBufferToByteArray(const char *buffer, size_t size)
{
    TfPyLock lock;
    pxr_boost::python::object result;

    try {
        pxr_boost::python::handle<> hbuf(
            Tf_PyCopyBufferToByteArray(buffer, size));
        result = pxr_boost::python::object(hbuf);
    } catch (pxr_boost::python::error_already_set const &) {
        TfPyConvertPythonExceptionToTfErrors();
        PyErr_Clear();
    }

    return result;
}

PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_PYTHON_SUPPORT_ENABLED && !Py_LIMITED_API
