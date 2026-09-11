//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//

#include "pxr/pxr.h"
#include "pxr/base/tf/anyWeakPtr.h"

#if defined(PXR_PYTHON_SUPPORT_ENABLED) && !defined(Py_LIMITED_API)

#include "pxr/external/boost/python/handle.hpp"
#include "pxr/external/boost/python/object.hpp"

PXR_NAMESPACE_OPEN_SCOPE

pxr_boost::python::api::object
TfAnyWeakPtr::_GetPythonObject() const
{
    return pxr_boost::python::object(
        pxr_boost::python::handle<>(_GetPythonObjectPtr()));
}

PXR_NAMESPACE_CLOSE_SCOPE

#endif // defined(PXR_PYTHON_SUPPORT_ENABLED) && !defined(Py_LIMITED_API)
