//
// Copyright 2021 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//

#include "pxr/pxr.h"

#include "pxr/base/tf/pyInvoke.h"

#include "pxr/base/tf/pyInvokeImpl.h"

#include "pxr/external/boost/python.hpp"

#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

// Convert nullptr to None.
pxr_boost::python::object Tf_ArgToPy(const std::nullptr_t &value)
{
    return pxr_boost::python::object();
}

void Tf_BuildPyInvokeKwArgs(
    pxr_boost::python::dict *kwArgsOut)
{
    // Variadic template recursion base case: all args already processed, do
    // nothing.
}

void Tf_BuildPyInvokeArgs(
    pxr_boost::python::list *posArgsOut,
    pxr_boost::python::dict *kwArgsOut)
{
    // Variadic template recursion base case: all args already processed, do
    // nothing.
}

bool Tf_PyInvokeImpl(
    const std::string &moduleName,
    const std::string &callableExpr,
    const pxr_boost::python::list &posArgs,
    const pxr_boost::python::dict &kwArgs,
    pxr_boost::python::object *resultObjOut)
{
    PyObject *result = nullptr;
    if (!Tf_PyInvokeImpl(
            moduleName, callableExpr, posArgs.ptr(), kwArgs.ptr(), &result)) {
        return false;
    }

    *resultObjOut = pxr_boost::python::object(
        pxr_boost::python::handle<>(result));
    return true;
}

PXR_NAMESPACE_CLOSE_SCOPE
