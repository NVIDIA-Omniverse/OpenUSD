//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "nativeTypes.h" // Core-first include order.
#include "pxr/base/vt/boost/valueBoost.h"
#include "pxr/external/boost/python/def.hpp"
#include "pxr/external/boost/python/extract.hpp"
#include "pxr/external/boost/python/module.hpp"
#include <memory>
PXR_NAMESPACE_USING_DIRECTIVE
namespace bp = pxr_boost::python;
static VtValue Stored() { return GetNativeValue(); }
static VtValue Typed() { return GetTypedProxy(); }
static VtValue Erased() { return GetErasedProxy(); }
static VtValue Ref(VtValueRef ref) { return VtValue(ref); }
static bp::object NativeView() {
    auto result = Vt_GetPythonObjectFromHeldValue(VtValueRef(GetNativeObject()));
    if (PyErr_Occurred()) bp::throw_error_already_set();
    return TfPyObjWrapperToBoostObject(result);
}
static VtValue PythonObject(bp::object const &obj) {
    VtValue value(obj);
    if (!value.IsHolding<TfPyObjWrapper>())
        bp::throw_error_already_set();
    VtValue copy;
    copy = obj;
    return copy;
}
static VtValue Shared(bp::object const &obj) {
    return VtValue(bp::extract<std::shared_ptr<NativeValue>>(obj)());
}
static VtValue OwnedShared() {
    return VtValue(std::make_shared<NativeValue>(91));
}
static VtValue NullShared() {
    return VtValue(std::shared_ptr<NativeValue>());
}
PXR_BOOST_PYTHON_MODULE(vtValueConsume) {
    bp::def("stored", Stored);
    bp::def("typed", Typed);
    bp::def("erased", Erased);
    bp::def("native_view", NativeView);
    bp::def("ref", Ref);
    bp::def("python_object", PythonObject);
    bp::def("pointer", GetNativePointer);
    bp::def("builtin", GetNativeBuiltin);
    bp::def("shared", Shared);
    bp::def("owned_shared", OwnedShared);
    bp::def("null_shared", NullShared);
}
