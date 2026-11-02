//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_BASE_VT_BOOST_VALUE_BOOST_H
#define PXR_BASE_VT_BOOST_VALUE_BOOST_H

#include "pxr/base/vt/valueCommon.h"
#include "pxr/base/tf/boost/pyObjWrapperBoost.h"

PXR_NAMESPACE_OPEN_SCOPE
// Keep the stored representation independent of the binding implementation.
// This specialization must be visible before constructing a value from object.
template <>
struct Vt_ValueStoredType<pxr_boost::python::object> {
    using Type = TfPyObjWrapper;
};
PXR_NAMESPACE_CLOSE_SCOPE

#include "pxr/base/vt/value.h"
#include "pxr/base/vt/valueRef.h"
#include "pxr/base/vt/boost/valueToPythonBoost.h"
#include "pxr/external/boost/python/converter/arg_from_python.hpp"
#include <optional>

namespace PXR_BOOST_NAMESPACE::python::converter {

// Own both the temporary VtValue and the non-owning view for the argument's
// lifetime. Keeping this inline places the binding code in each module.
template <>
struct arg_rvalue_from_python<PXR_NS::VtValueRef>
    : public arg_rvalue_from_python<PXR_NS::VtValue>
{
    using base_type = arg_rvalue_from_python<PXR_NS::VtValue>;
    using base_type::convertible;
    using result_type = PXR_NS::VtValueRef &;

    explicit arg_rvalue_from_python(PyObject *obj) : base_type(obj) {}
    result_type operator()() {
        optValueRef = PXR_NS::VtValueRef{base_type::operator()()};
        return *optValueRef;
    }
    std::optional<PXR_NS::VtValueRef> optValueRef;
};
}
#endif // PXR_BASE_VT_BOOST_VALUE_BOOST_H
