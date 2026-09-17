//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_BASE_TF_PY_INVOKE_BOOST_H
#define PXR_BASE_TF_PY_INVOKE_BOOST_H

/// \file
/// Boost.Python adapters for TfPyInvoke utilities.

#include "pxr/pxr.h"

#include "pxr/base/tf/diagnosticLite.h"
#include "pxr/base/tf/pyError.h"
#include "pxr/base/tf/pyInterpreter.h"
#include "pxr/base/tf/pyInvoke.h"
#include "pxr/base/tf/pyLock.h"
#include "pxr/base/tf/pyObjWrapper.h"
#include "pxr/base/tf/pyObjWrapperBoost.h"

#include "pxr/external/boost/python/dict.hpp"
#include "pxr/external/boost/python/extract.hpp"
#include "pxr/external/boost/python/handle.hpp"
#include "pxr/external/boost/python/list.hpp"
#include "pxr/external/boost/python/object.hpp"

#include <cstddef>
#include <string>
#include <type_traits>

PXR_NAMESPACE_OPEN_SCOPE

#ifndef doxygen

template <typename T>
pxr_boost::python::object Tf_ArgToPy(const T &value)
{
    return pxr_boost::python::object(value);
}

inline pxr_boost::python::object Tf_ArgToPy(const std::nullptr_t &value)
{
    return pxr_boost::python::object();
}

#endif // !doxygen

/// Wrapper object for a keyword-argument pair in a call to TfPyInvoke*.  Any
/// value type may be provided, as long as it is convertible to Python.
struct TfPyKwArg
{
    template <typename T>
    TfPyKwArg(const std::string &nameIn, const T &valueIn)
        : name(nameIn)
    {
        TfPyLock lock;
        value = TfPyObjWrapperFromBoostObject(Tf_ArgToPy(valueIn));
    }

    std::string name;
    TfPyObjWrapper value;
};

#ifndef doxygen

inline void Tf_BuildPyInvokeKwArgs(
    pxr_boost::python::dict *kwArgsOut)
{
}

template <typename Arg, typename... RestArgs>
void Tf_BuildPyInvokeKwArgs(
    pxr_boost::python::dict *kwArgsOut,
    const Arg &kwArg,
    RestArgs... rest)
{
    static_assert(
        std::is_same<Arg, TfPyKwArg>::value,
        "Non-keyword args not allowed after keyword args");
}

template <typename... RestArgs>
void Tf_BuildPyInvokeKwArgs(
    pxr_boost::python::dict *kwArgsOut,
    const TfPyKwArg &kwArg,
    RestArgs... rest)
{
    (*kwArgsOut)[kwArg.name] = TfPyObjWrapperToBoostObject(kwArg.value);
    Tf_BuildPyInvokeKwArgs(kwArgsOut, rest...);
}

inline void Tf_BuildPyInvokeArgs(
    pxr_boost::python::list *posArgsOut,
    pxr_boost::python::dict *kwArgsOut)
{
}

template <typename Arg, typename... RestArgs>
void Tf_BuildPyInvokeArgs(
    pxr_boost::python::list *posArgsOut,
    pxr_boost::python::dict *kwArgsOut,
    const Arg &arg,
    RestArgs... rest)
{
    posArgsOut->append(Tf_ArgToPy(arg));
    Tf_BuildPyInvokeArgs(posArgsOut, kwArgsOut, rest...);
}

template <typename... RestArgs>
void Tf_BuildPyInvokeArgs(
    pxr_boost::python::list *posArgsOut,
    pxr_boost::python::dict *kwArgsOut,
    const TfPyKwArg &kwArg,
    RestArgs... rest)
{
    Tf_BuildPyInvokeKwArgs(kwArgsOut, kwArg, rest...);
}

inline bool Tf_PyInvokeBoostImpl(
    const std::string &moduleName,
    const std::string &callableExpr,
    const pxr_boost::python::list &posArgs,
    const pxr_boost::python::dict &kwArgs,
    pxr_boost::python::object *resultObjOut)
{
    PyObject *result = nullptr;
    if (!TfPyInvokeAndReturn(
            moduleName, callableExpr, posArgs.ptr(), kwArgs.ptr(), &result)) {
        return false;
    }

    *resultObjOut = pxr_boost::python::object(
        pxr_boost::python::handle<>(result));
    return true;
}

template <typename... Args>
bool TfPyInvokeAndReturn(
    const std::string &moduleName,
    const std::string &callableExpr,
    pxr_boost::python::object *resultOut,
    Args... args);

#endif // !doxygen

template <typename Result, typename... Args>
bool TfPyInvokeAndExtract(
    const std::string &moduleName,
    const std::string &callableExpr,
    Result *resultOut,
    Args... args)
{
    if (!resultOut) {
        TF_CODING_ERROR("Bad pointer to TfPyInvokeAndExtract");
        return false;
    }

    TfPyInitialize();
    TfPyLock lock;

    pxr_boost::python::object resultObj;
    if (!TfPyInvokeAndReturn(
            moduleName, callableExpr, &resultObj, args...)) {
        return false;
    }

    pxr_boost::python::extract<Result> extractor(resultObj);
    if (!extractor.check()) {
        TF_CODING_ERROR("Result type mismatched or not convertible");
        return false;
    }
    *resultOut = extractor();

    return true;
}

template <typename... Args>
bool TfPyInvokeAndReturn(
    const std::string &moduleName,
    const std::string &callableExpr,
    pxr_boost::python::object *resultOut,
    Args... args)
{
    if (!resultOut) {
        TF_CODING_ERROR("Bad pointer to TfPyInvokeAndExtract");
        return false;
    }

    TfPyInitialize();
    TfPyLock lock;

    try {
        pxr_boost::python::list posArgs;
        pxr_boost::python::dict kwArgs;
        Tf_BuildPyInvokeArgs(&posArgs, &kwArgs, args...);

        if (!Tf_PyInvokeBoostImpl(
                moduleName, callableExpr, posArgs, kwArgs, resultOut)) {
            return false;
        }
    }
    catch (pxr_boost::python::error_already_set const &) {
        TfPyConvertPythonExceptionToTfErrors();
        PyErr_Clear();
        return false;
    }

    return true;
}

template <typename... Args>
bool TfPyInvoke(
    const std::string &moduleName,
    const std::string &callableExpr,
    Args... args)
{
    TfPyInitialize();
    TfPyLock lock;

    pxr_boost::python::object ignoredResult;
    return TfPyInvokeAndReturn(
        moduleName, callableExpr, &ignoredResult, args...);
}

PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_BASE_TF_PY_INVOKE_BOOST_H
