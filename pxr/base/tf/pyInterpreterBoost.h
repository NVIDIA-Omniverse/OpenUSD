//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_BASE_TF_PY_INTERPRETER_BOOST_H
#define PXR_BASE_TF_PY_INTERPRETER_BOOST_H

/// \file tf/pyInterpreterBoost.h
/// Boost.Python adapters for Python runtime utilities.

#include "pxr/pxr.h"

#include "pxr/base/tf/pyInterpreter.h"

#include "pxr/external/boost/python/handle.hpp"
#include "pxr/external/boost/python/object.hpp"

#include <string>

PXR_NAMESPACE_OPEN_SCOPE

/// Runs the given string using PyRun_String().
///
/// Callers must hold the GIL before calling; see TfPyLock. This is true for
/// any pxr_boost::python call, including constructing and destroying the
/// default values of \p globals and \p locals. Holding the GIL will also make
/// it safe to inspect the returned pxr_boost::python::handle.
inline pxr_boost::python::handle<>
TfPyRunString(const std::string &cmd, int start,
              pxr_boost::python::object const &globals =
                  pxr_boost::python::object(),
              pxr_boost::python::object const &locals =
                  pxr_boost::python::object())
{
    PyObject *result = TfPyRunString(
        cmd, start,
        globals.ptr() == Py_None ? nullptr : globals.ptr(),
        locals.ptr() == Py_None ? nullptr : locals.ptr());
    return result ? pxr_boost::python::handle<>(result)
                  : pxr_boost::python::handle<>();
}

/// Runs the given file using PyRun_File().
///
/// Callers must hold the GIL before calling; see TfPyLock. This is true for
/// any pxr_boost::python call, including constructing and destroying the
/// default values of \p globals and \p locals. Holding the GIL will also make
/// it safe to inspect the returned pxr_boost::python::handle.
inline pxr_boost::python::handle<>
TfPyRunFile(const std::string &filename, int start,
            pxr_boost::python::object const &globals =
                pxr_boost::python::object(),
            pxr_boost::python::object const &locals =
                pxr_boost::python::object())
{
    PyObject *result = TfPyRunFile(
        filename, start,
        globals.ptr() == Py_None ? nullptr : globals.ptr(),
        locals.ptr() == Py_None ? nullptr : locals.ptr());
    return result ? pxr_boost::python::handle<>(result)
                  : pxr_boost::python::handle<>();
}

PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_BASE_TF_PY_INTERPRETER_BOOST_H
