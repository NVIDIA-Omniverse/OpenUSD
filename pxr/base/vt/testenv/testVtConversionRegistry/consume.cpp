//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "types.h"
#include "pxr/base/vt/valueFromPython.h"
#include "pxr/base/vt/value.h"
#include "pxr/external/boost/python/def.hpp"
#include "pxr/external/boost/python/module.hpp"
#include "pxr/external/boost/python/object.hpp"

PXR_NAMESPACE_USING_DIRECTIVE
namespace bp = pxr_boost::python;
static int Read(VtValue const &value) {
    return value.IsHolding<Registered>() ?
        value.UncheckedGet<Registered>().value : -1;
}
static int Probe(bp::object const &object) {
    VtValue value = Vt_ValueFromPythonRegistry::Invoke(object.ptr());
    if (PyErr_Occurred()) bp::throw_error_already_set();
    return Read(value);
}
static VtValue Echo(VtValue const &value) { return value; }
PXR_BOOST_PYTHON_MODULE(vtRegistryConsume) {
    bp::def("probe", Probe);
    bp::def("read", Read);
    bp::def("echo", Echo);
}
