//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/base/vt/valueFromPython.h"
#include "pxr/base/vt/value.h"
#include "pxr/base/tf/errorMark.h"
#include "pxr/base/tf/pyLock.h"
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <thread>

PXR_NAMESPACE_USING_DIRECTIVE
using Registry = Vt_ValueFromPythonRegistry;
#define CHECK(expr) do { if (!(expr)) throw std::runtime_error( \
    std::string("line ") + std::to_string(__LINE__) + ": " + #expr); } while (false)

namespace {
int oldCalls = 0, newCalls = 0, rvalueCalls = 0;
bool declineNew = false, raiseError = false;
PyObject *ownedType = nullptr;

VtValue None(PyObject *) { CHECK(PyGILState_Check()); return VtValue(); }
VtValue Old(PyObject *obj) {
    CHECK(PyGILState_Check());
    ++oldCalls;
    return PyLong_Check(obj) ? VtValue(1) : VtValue();
}
VtValue New(PyObject *obj) {
    CHECK(PyGILState_Check());
    ++newCalls;
    return PyLong_Check(obj) && !declineNew ? VtValue(2) : VtValue();
}
VtValue RValue(PyObject *obj) {
    CHECK(PyGILState_Check());
    ++rvalueCalls;
    return PyUnicode_Check(obj) ? VtValue(3) :
        PyLong_Check(obj) ? VtValue(9) : VtValue();
}
VtValue Error(PyObject *) {
    if (raiseError) PyErr_SetString(PyExc_ValueError, "conversion failed");
    return VtValue();
}
VtValue Reentrant(PyObject *obj) {
    if (!PyBytes_Check(obj)) return VtValue();
    Registry::Register(+[](PyObject *o) {
        return PyBytes_Check(o) ? VtValue(5) : VtValue();
    });
    return VtValue(4);
}
VtValue Owned(PyObject *obj) {
    PyObject *type = PyObject_Type(obj);
    const bool match = type == ownedType;
    Py_DECREF(type);
    return match ? VtValue(6) : VtValue();
}
int Result(PyObject *obj) {
    VtValue value = Registry::Invoke(obj);
    CHECK(value.IsHolding<int>());
    return value.UncheckedGet<int>();
}

int Run() {
    Py_Initialize();
    TfPyLock lock;
    CHECK(!Registry::HasConversions());
    PyObject *number = PyLong_FromLong(17);
    PyObject *text = PyUnicode_FromString("test");
    PyObject *bytes = PyBytes_FromString("test");
    CHECK(Registry::Invoke(number).IsEmpty());
    {
        TfErrorMark mark;
        Registry::Register(nullptr);
        CHECK(!mark.IsClean());
        mark.Clear();
    }
    CHECK(!Registry::HasConversions());
    Registry::Register(Old);
    CHECK(!Registry::HasConversions());
    const auto refs = Py_REFCNT(number);
    CHECK(Result(number) == 1);
    CHECK(Py_REFCNT(number) == refs);
    Registry::Register(New, RValue);
    CHECK(Registry::HasConversions());
    int before = rvalueCalls;
    // A new registration must replace the earlier cached choice.
    CHECK(Result(number) == 2);
    CHECK(rvalueCalls == before); // Lvalues take precedence over rvalues.
    before = oldCalls;
    CHECK(Result(number) == 2);
    CHECK(oldCalls == before);
    // Cached extractors can decline subsequent objects of the same type.
    declineNew = true;
    CHECK(Result(number) == 1);
    declineNew = false;
    Registry::Register(None); // Invalidate the fallback cache.
    CHECK(Result(number) == 2);
    before = rvalueCalls;
    CHECK(Result(text) == 3);
    CHECK(Result(text) == 3);
    CHECK(rvalueCalls == before + 2); // Rvalue conversions aren't cached.

    Registry::Register(Error);
    raiseError = true;
    CHECK(Registry::Invoke(number).IsEmpty());
    CHECK(PyErr_ExceptionMatches(PyExc_ValueError));
    PyErr_Clear();
    raiseError = false;
    before = newCalls;
    PyErr_SetString(PyExc_KeyError, "pending");
    CHECK(Registry::Invoke(number).IsEmpty());
    CHECK(PyErr_ExceptionMatches(PyExc_KeyError));
    CHECK(newCalls == before);
    PyErr_Clear();
    CHECK(Registry::Invoke(nullptr).IsEmpty());
    CHECK(PyErr_ExceptionMatches(PyExc_TypeError));
    PyErr_Clear();

    Registry::Register(Reentrant);
    CHECK(Result(bytes) == 4);
    CHECK(Result(bytes) == 5); // Reentrant registration wasn't hidden by cache.

    // Cache entries own their type and release it when invalidated.
    PyObject *globals = PyDict_New();
    PyDict_SetItemString(globals, "__builtins__", PyEval_GetBuiltins());
    ownedType = PyRun_String("type('Ephemeral', (), {})", Py_eval_input,
                             globals, globals);
    CHECK(ownedType);
    PyObject *weak = PyWeakref_NewRef(ownedType, nullptr);
    PyObject *instance = PyObject_CallObject(ownedType, nullptr);
    CHECK(weak && instance);
    Registry::Register(Owned);
    CHECK(Result(instance) == 6);
    Py_DECREF(instance);
    Py_DECREF(ownedType);
    ownedType = nullptr;
    PyGC_Collect();
    CHECK(PyWeakref_GetObject(weak) != Py_None);
    Registry::Register(None);
    PyGC_Collect();
    CHECK(PyWeakref_GetObject(weak) == Py_None);
    Py_DECREF(weak);
    Py_DECREF(globals);

    std::exception_ptr failure;
    lock.BeginAllowThreads();
    std::thread worker([&] {
        try {
            Registry::Register(None);
            CHECK(Result(number) == 2);
        } catch (...) { failure = std::current_exception(); }
    });
    worker.join();
    lock.EndAllowThreads();
    if (failure) std::rethrow_exception(failure);
    CHECK(!PyErr_Occurred());
    CHECK(!PyDict_GetItemString(PyImport_GetModuleDict(), "pxr.Vt"));
    Py_DECREF(number);
    Py_DECREF(text);
    Py_DECREF(bytes);
    std::puts("PASS: raw registry ordering, cache, errors, ownership, reentrancy and GIL");
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
