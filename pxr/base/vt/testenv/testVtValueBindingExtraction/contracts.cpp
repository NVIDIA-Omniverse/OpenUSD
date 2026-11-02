//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "nativeTypes.h"
#include "pxr/base/tf/errorMark.h"
#include "pxr/base/tf/pyLock.h"
#include "pxr/base/vt/valueToPython.h"
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <thread>

PXR_NAMESPACE_USING_DIRECTIVE
using Registry = Vt_ValueToPythonRegistry;
#define CHECK(expr) do { if (!(expr)) throw std::runtime_error( \
    std::string("line ") + std::to_string(__LINE__) + ": " + #expr); } while (false)
namespace {
int calls = 0;
PyObject *Convert(void const *ptr) {
    ++calls;
    return PyLong_FromLong(static_cast<NativeValue const *>(ptr)->number);
}
PyObject *Replace(void const *ptr) {
    return PyLong_FromLong(static_cast<NativeValue const *>(ptr)->number + 100);
}
PyObject *Reentrant(void const *ptr) {
    Registry::Register(typeid(NativeValue), Replace);
    return Convert(ptr);
}
PyObject *Error(void const *) {
    PyErr_SetString(PyExc_ValueError, "native conversion failed");
    return nullptr;
}
PyObject *Invalid(void const *) { return nullptr; }
PyObject *Inconsistent(void const *) {
    PyErr_SetString(PyExc_ValueError, "result with exception");
    return PyLong_FromLong(99);
}
long Read(VtValue const &value) {
    TfPyObjWrapper result = Vt_GetPythonObjectFromHeldValue(value);
    CHECK(!PyErr_Occurred());
    return PyLong_AsLong(result.ptr());
}
int Run() {
    Py_Initialize();
    TfPyLock lock;
    TfPyObjWrapper missing = Vt_GetPythonObjectFromHeldValue(GetNativeValue());
    CHECK(missing.ptr() == Py_None);
    CHECK(PyErr_ExceptionMatches(PyExc_TypeError));
    PyErr_Clear();
    Registry::Register(typeid(NativeValue), Inconsistent);
    CHECK(Vt_GetPythonObjectFromHeldValue(GetNativeValue()).ptr() == Py_None);
    CHECK(PyErr_ExceptionMatches(PyExc_ValueError));
    PyErr_Clear();
    Registry::Register(typeid(NativeValue), Convert);
    CHECK(Read(GetNativeValue()) == 37);
    CHECK(Read(GetTypedProxy()) == 37);
    CHECK(Read(GetErasedProxy()) == 37);
    CHECK(Read(GetNativePointer(false)) == 37);
    Registry::Register(typeid(NativeValue *), +[](void const *value) {
        auto *pointed = *static_cast<NativeValue *const *>(value);
        return PyLong_FromLong(pointed ? pointed->number + 500 : 0);
    });
    CHECK(Read(GetNativePointer(false)) == 537);
    VtValueRef view(GetNativeObject());
    CHECK(PyLong_AsLong(Vt_GetPythonObjectFromHeldValue(view).ptr()) == 37);
    CHECK(PyLong_AsLong(Vt_GetPythonObjectFromHeldValue(
        VtValueRef(GetTypedProxy())).ptr()) == 37);
    CHECK(PyLong_AsLong(Vt_GetPythonObjectFromHeldValue(
        VtValueRef(GetErasedProxy())).ptr()) == 37);
    CHECK(Vt_GetPythonObjectFromHeldValue(VtValue()).ptr() == Py_None);
    CHECK(Vt_GetPythonObjectFromHeldValue(VtValueRef()).ptr() == Py_None);
    CHECK(!PyErr_Occurred());
    VtValue integer(12);
    CHECK(Vt_GetPythonObjectFromHeldValue(integer).ptr() == Py_None);
    CHECK(PyErr_ExceptionMatches(PyExc_TypeError));
    PyErr_Clear();
    Registry::RegisterFallback(+[](
        std::type_info const &type, void const *value,
        std::shared_ptr<void const> const *) -> PyObject * {
            return type == typeid(int) ?
                PyLong_FromLong(*static_cast<int const *>(value)) : nullptr;
        });
    CHECK(Read(integer) == 12);
    Registry::Register(typeid(NativeValue), Reentrant);
    CHECK(Read(GetNativeValue()) == 37);
    CHECK(Read(GetNativeValue()) == 137);
    {
        TfErrorMark mark;
        Registry::Register(typeid(NativeValue), nullptr);
        Registry::RegisterFallback(nullptr);
        CHECK(!mark.IsClean());
        mark.Clear();
    }
    CHECK(Read(GetNativeValue()) == 137);
    Registry::Register(typeid(NativeValue), Error);
    CHECK(Vt_GetPythonObjectFromHeldValue(GetNativeValue()).ptr() == Py_None);
    CHECK(PyErr_ExceptionMatches(PyExc_ValueError));
    PyErr_Clear();
    Registry::Register(typeid(NativeValue), Invalid);
    CHECK(Vt_GetPythonObjectFromHeldValue(GetNativeValue()).ptr() == Py_None);
    CHECK(PyErr_ExceptionMatches(PyExc_TypeError));
    PyErr_Clear();
    Registry::Register(typeid(NativeValue), Convert);
    int before = calls;
    PyErr_SetString(PyExc_KeyError, "pending");
    CHECK(Vt_GetPythonObjectFromHeldValue(GetNativeValue()).ptr() == Py_None);
    CHECK(PyErr_ExceptionMatches(PyExc_KeyError));
    CHECK(calls == before);
    PyErr_Clear();
    CHECK(Registry::Invoke(typeid(NativeValue), nullptr).ptr() == Py_None);
    CHECK(PyErr_ExceptionMatches(PyExc_TypeError));
    PyErr_Clear();
    std::exception_ptr failure;
    lock.BeginAllowThreads();
    std::thread worker([&] {
        try {
            auto result = Vt_GetPythonObjectFromHeldValue(GetNativeValue());
            TfPyLock workerLock;
            CHECK(PyLong_AsLong(result.ptr()) == 37);
        } catch (...) { failure = std::current_exception(); }
    });
    worker.join();
    lock.EndAllowThreads();
    if (failure) std::rethrow_exception(failure);
    CHECK(!PyDict_GetItemString(PyImport_GetModuleDict(), "pxr.Vt"));
    CHECK(!PyErr_Occurred());
    std::puts("PASS: neutral to-Python dispatch, late registration, proxies, views, errors and GIL");
    return 0;
}
}
int main() {
    try { return Run(); }
    catch (std::exception const &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
