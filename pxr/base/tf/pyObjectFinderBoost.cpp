//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//

#include "pxr/pxr.h"

#include "pxr/base/tf/pyObjectFinder.h"

#ifdef PXR_PYTHON_SUPPORT_ENABLED
#ifndef Py_LIMITED_API

#include "pxr/external/boost/python/handle.hpp"
#include "pxr/external/boost/python/object.hpp"

PXR_NAMESPACE_OPEN_SCOPE

pxr_boost::python::object
Tf_FindPythonObject(void const *objPtr, std::type_info const &type)
{
    PyObject *obj = Tf_PyFindPythonObject(objPtr, type);
    return obj
        ? pxr_boost::python::object(pxr_boost::python::handle<>(obj))
        : pxr_boost::python::object();
}

PXR_NAMESPACE_CLOSE_SCOPE

#endif // Py_LIMITED_API
#endif // PXR_PYTHON_SUPPORT_ENABLED
