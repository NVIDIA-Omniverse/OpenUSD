//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//

#include "pxr/pxr.h"

#include "pxr/base/tf/pyObjWrapper.h"

#if defined(PXR_PYTHON_SUPPORT_ENABLED) && !defined(Py_LIMITED_API)

#include "pxr/base/tf/pyLock.h"

#include "pxr/external/boost/python/borrowed.hpp"
#include "pxr/external/boost/python/handle.hpp"
#include "pxr/external/boost/python/object.hpp"

PXR_NAMESPACE_OPEN_SCOPE

TfPyObjWrapper::TfPyObjWrapper(pxr_boost::python::object obj)
    : TfPyObjWrapper(obj.ptr(), TfPyBorrowedReference)
{
}

pxr_boost::python::object
TfPyObjWrapper::Get() const
{
    TfPyLock lock;
    return object(
        pxr_boost::python::handle<>(
            pxr_boost::python::borrowed(ptr())));
}

PXR_NAMESPACE_CLOSE_SCOPE

#endif
