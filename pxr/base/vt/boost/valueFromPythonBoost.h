//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_BASE_VT_BOOST_VALUE_FROM_PYTHON_BOOST_H
#define PXR_BASE_VT_BOOST_VALUE_FROM_PYTHON_BOOST_H

#include "pxr/base/vt/valueFromPython.h"
#include "pxr/base/vt/boost/valueBoost.h"
#include "pxr/external/boost/python/extract.hpp"
#include "pxr/external/boost/python/errors.hpp"

PXR_NAMESPACE_OPEN_SCOPE

namespace Vt_BoostPythonDetail {
template <class T>
bool ExtractTyped(PyObject *obj, VtValue *value)
{
    try {
        pxr_boost::python::extract<T> extract(obj);
        if (extract.check()) {
            *value = VtValue(extract());
            return true;
        }
    } catch (pxr_boost::python::error_already_set const &) {
        // Only the pending Python error crosses the core callback boundary.
    }
    return false;
}

template <class T>
VtValue Extract(PyObject *obj)
{
    try {
        pxr_boost::python::extract<T> extract(obj);
        if (extract.check()) {
            return VtValue(extract());
        }
    } catch (pxr_boost::python::error_already_set const &) {
        // Preserve the Python error for the registry's module caller.
    }
    return VtValue();
}
}

/// Register requested-type extraction without altering generic type inference.
template <class T>
void VtValueFromPythonTyped()
{
    Vt_ValueFromPythonRegistry::RegisterTyped(
        typeid(T), &Vt_BoostPythonDetail::ExtractTyped<T>);
}

/// Register Boost.Python lvalue and converting rvalue extraction for T.
/// Include this adapter and link vtBoostPython in binding code.
template <class T>
void VtValueFromPython()
{
    VtValueToPython<T>();
    VtValueFromPythonTyped<T>();
    Vt_ValueFromPythonRegistry::Register(
        &Vt_BoostPythonDetail::Extract<T &>,
        &Vt_BoostPythonDetail::Extract<T>);
}

/// Register only non-converting Boost.Python lvalue extraction for T.
template <class T>
void VtValueFromPythonLValue()
{
    VtValueToPython<T>();
    Vt_ValueFromPythonRegistry::RegisterTyped(
        typeid(T), &Vt_BoostPythonDetail::ExtractTyped<T &>);
    Vt_ValueFromPythonRegistry::Register(
        &Vt_BoostPythonDetail::Extract<T &>);
}

PXR_NAMESPACE_CLOSE_SCOPE
#endif // PXR_BASE_VT_BOOST_VALUE_FROM_PYTHON_BOOST_H
