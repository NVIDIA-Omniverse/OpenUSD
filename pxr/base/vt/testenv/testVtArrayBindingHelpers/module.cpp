//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/base/vt/boost/valueFromPythonBoost.h"
#include "pxr/base/vt/boost/wrapArray.h"
#include "pxr/base/vt/boost/wrapArrayEdit.h"
#include "pxr/external/boost/python/module.hpp"
#include <ostream>

PXR_NAMESPACE_USING_DIRECTIVE
namespace bp = pxr_boost::python;
struct Element {
    explicit Element(int n = 0) : value(n) {}
    int value;
    bool operator==(Element const &other) const { return value == other.value; }
    bool operator!=(Element const &other) const { return !(*this == other); }
    template <class Hash>
    friend void TfHashAppend(Hash &hash, Element const &e) { hash.Append(e.value); }
};
std::ostream &operator<<(std::ostream &out, Element const &e) { return out << e.value; }
PXR_NAMESPACE_OPEN_SCOPE
namespace Vt_WrapArray {
template <> std::string GetVtArrayName<VtArray<Element>>() { return "ElementArray"; }
}
PXR_NAMESPACE_CLOSE_SCOPE

std::string BuiltinNameFromOtherTU();
unsigned int RankFromOtherTU(size_t, size_t, size_t *);
static VtValue Identity(VtValue const &value) { return value; }
static VtValue CastSequence(bp::object const &object) {
    return VtValue::Cast<VtArray<Element>>(
        VtValue(TfPyObjWrapper(object.ptr(), TfPyBorrowedReference)));
}
static bp::tuple Rank(size_t size, size_t dimension) {
    size_t last = 0;
    unsigned int rank = RankFromOtherTU(size, dimension, &last);
    return bp::make_tuple(rank, last);
}
static VtValue CastInt(bp::object const &object) {
    return VtValue::Cast<VtIntArray>(
        VtValue(TfPyObjWrapper(object.ptr(), TfPyBorrowedReference)));
}
static VtValue CastVector() {
    return VtValue::Cast<VtFloatArray>(
        VtValue(std::vector<VtValue>{VtValue(1), VtValue(2.5)}));
}
static void RequestBuffer(bp::object const &object, bool writable, bool fortran) {
    Py_buffer view{};
    int flags = PyBUF_FORMAT | PyBUF_STRIDES;
    if (writable) flags |= PyBUF_WRITABLE;
    if (fortran) flags |= PyBUF_F_CONTIGUOUS;
    if (PyObject_GetBuffer(object.ptr(), &view, flags) != 0)
        bp::throw_error_already_set();
    PyBuffer_Release(&view);
}
PXR_BOOST_PYTHON_MODULE(vtArrayHelpers) {
    bp::class_<Element>("Element", bp::init<int>())
        .def_readwrite("value", &Element::value)
        .def("__repr__", +[](Element const &e) {
            return "vtArrayHelpers.Element(" + std::to_string(e.value) + ")";
        });
    TfType::Define<Element>();
    TfType::Define<VtArray<Element>>();
    VtValueFromPython<Element>();
    VtWrapArray<VtArray<Element>>();
    VtWrapArrayEdit<VtArrayEdit<Element>>();
    VtValueFromPythonLValue<VtArray<Element>>();
    VtValueFromPythonLValue<VtArrayEdit<Element>>();
    VtRegisterValueCastsFromPythonSequencesToArray<Element>();
    bp::def("identity", Identity);
    bp::def("cast_sequence", CastSequence);
    bp::def("builtin_name", BuiltinNameFromOtherTU);
    bp::def("rank", Rank);
    bp::def("cast_int", CastInt);
    bp::def("cast_vector", CastVector);
    bp::def("request_buffer", RequestBuffer);
}
