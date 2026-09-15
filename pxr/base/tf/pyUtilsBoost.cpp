//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//

#include "pxr/pxr.h"

#if defined(PXR_PYTHON_SUPPORT_ENABLED) && !defined(Py_LIMITED_API)

#include "pxr/base/tf/pyError.h"
#include "pxr/base/tf/pyLock.h"
#include "pxr/base/tf/pyUtils.h"
#include "pxr/base/tf/pyUtilsImpl.h"

#include "pxr/external/boost/python/errors.hpp"
#include "pxr/external/boost/python/handle.hpp"
#include "pxr/external/boost/python/object.hpp"

PXR_NAMESPACE_OPEN_SCOPE

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
