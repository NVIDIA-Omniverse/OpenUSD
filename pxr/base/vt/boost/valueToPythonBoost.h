//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_BASE_VT_BOOST_VALUE_TO_PYTHON_BOOST_H
#define PXR_BASE_VT_BOOST_VALUE_TO_PYTHON_BOOST_H
#include "pxr/base/vt/valueToPython.h"
#include "pxr/external/boost/python/object.hpp"
#include "pxr/external/boost/python/errors.hpp"
#include "pxr/external/boost/python/refcount.hpp"

PXR_NAMESPACE_OPEN_SCOPE
namespace Vt_BoostPythonDetail {
template <class T>
PyObject *ToPython(void const *value)
{
    try {
        return pxr_boost::python::incref(
            pxr_boost::python::object(*static_cast<T const *>(value)).ptr());
    } catch (pxr_boost::python::error_already_set const &) {
        return nullptr;
    }
}
}

/// Register a typed Boost to-Python adapter. Most class conversions can use
/// _vt's runtime lookup without explicit registration. Use this for converters
/// selected by C++ templates (for example function pointers and object managers).
template <class T>
void VtValueToPython()
{
    Vt_ValueToPythonRegistry::Register(
        typeid(T), &Vt_BoostPythonDetail::ToPython<T>);
}
PXR_NAMESPACE_CLOSE_SCOPE
#endif // PXR_BASE_VT_BOOST_VALUE_TO_PYTHON_BOOST_H
