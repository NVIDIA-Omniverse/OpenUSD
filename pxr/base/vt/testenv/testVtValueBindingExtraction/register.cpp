//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "nativeTypes.h"
#include "pxr/base/vt/boost/valueFromPythonBoost.h"
#include "pxr/external/boost/python/class.hpp"
#include "pxr/external/boost/python/def.hpp"
#include "pxr/external/boost/python/module.hpp"
#include <memory>
PXR_NAMESPACE_USING_DIRECTIVE
namespace bp = pxr_boost::python;
PXR_BOOST_PYTHON_MODULE(vtValueRegister) {
    // Intentionally no Vt-specific registration until explicitly requested.
    bp::class_<NativeValue, std::shared_ptr<NativeValue>>(
        "NativeValue", bp::init<int>())
        .def_readwrite("number", &NativeValue::number);
    bp::def("register_from_python", &VtValueFromPython<NativeValue>);
}
