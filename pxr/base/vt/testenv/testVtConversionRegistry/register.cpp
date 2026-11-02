//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "types.h"
#include "pxr/base/vt/boost/valueFromPythonBoost.h"
#include "pxr/external/boost/python/class.hpp"
#include "pxr/external/boost/python/def.hpp"
#include "pxr/external/boost/python/implicit.hpp"
#include "pxr/external/boost/python/module.hpp"

PXR_NAMESPACE_USING_DIRECTIVE
namespace bp = pxr_boost::python;
PXR_BOOST_PYTHON_MODULE(vtRegistryRegister) {
    bp::class_<Registered>("Registered", bp::init<int>())
        .def_readwrite("value", &Registered::value);
    bp::class_<Convertible>("Convertible", bp::init<int>());
    bp::implicitly_convertible<Convertible, Registered>();
    bp::def("register_lvalue", &VtValueFromPythonLValue<Registered>);
    bp::def("register_all", &VtValueFromPython<Registered>);
}
