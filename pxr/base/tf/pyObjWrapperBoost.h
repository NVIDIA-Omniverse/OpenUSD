//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_BASE_TF_PY_OBJ_WRAPPER_BOOST_H
#define PXR_BASE_TF_PY_OBJ_WRAPPER_BOOST_H

/// \file tf/pyObjWrapperBoost.h
/// Boost.Python adapters for TfPyObjWrapper.

#include "pxr/pxr.h"

#include "pxr/base/tf/pyLock.h"
#include "pxr/base/tf/pyObjWrapper.h"

#include "pxr/external/boost/python/borrowed.hpp"
#include "pxr/external/boost/python/handle.hpp"
#include "pxr/external/boost/python/object.hpp"

PXR_NAMESPACE_OPEN_SCOPE

inline TfPyObjWrapper
TfPyObjWrapperFromBoostObject(pxr_boost::python::object const &obj)
{
    return TfPyObjWrapper(obj.ptr(), TfPyBorrowedReference);
}

inline pxr_boost::python::object
TfPyObjWrapperToBoostObject(TfPyObjWrapper const &obj)
{
    TfPyLock lock;
    return pxr_boost::python::object(
        pxr_boost::python::handle<>(
            pxr_boost::python::borrowed(obj.ptr())));
}

PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_BASE_TF_PY_OBJ_WRAPPER_BOOST_H
