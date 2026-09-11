//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//

#include "pxr/pxr.h"

#include "pxr/base/tf/pyWeakObject.h"

#ifdef PXR_PYTHON_SUPPORT_ENABLED
#ifndef Py_LIMITED_API

#include "pxr/external/boost/python/borrowed.hpp"
#include "pxr/external/boost/python/handle.hpp"
#include "pxr/external/boost/python/object.hpp"

PXR_NAMESPACE_OPEN_SCOPE

Tf_PyWeakObjectPtr
Tf_PyWeakObject::GetOrCreate(pxr_boost::python::object const &obj)
{
    return GetOrCreate(obj.ptr());
}

pxr_boost::python::object
Tf_PyWeakObject::GetObject() const
{
    PyObject *obj = GetObjectPtr();
    return obj
        ? pxr_boost::python::object(
              pxr_boost::python::handle<>(pxr_boost::python::borrowed(obj)))
        : pxr_boost::python::object();
}

PXR_NAMESPACE_CLOSE_SCOPE

#endif // Py_LIMITED_API
#endif // PXR_PYTHON_SUPPORT_ENABLED
