// Copyright 2026 Pixar
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
#include "native.h"
#include "pxr/usd/sdf/pySpecRegistry.h"
#include "pxr/usd/sdf/primSpec.h"
#include "pxr/usd/sdf/attributeSpec.h"
#include "pxr/usd/sdf/relationshipSpec.h"
#include "pxr/usd/sdf/variantSpec.h"
#include "pxr/base/tf/errorMark.h"
#include "pxr/base/tf/pyLock.h"
#include <exception>
#include <fstream>
#include <iterator>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>

PXR_NAMESPACE_USING_DIRECTIVE
using namespace Sdf_PySpecDetail;
#define CHECK(expr) do { if (!(expr)) throw std::runtime_error(#expr); } while (false)
namespace {
int mode = 0, calls = 0, destroyed = 0;
struct UnregisteredType {};
PyObject* Relation(const SdfSpec&) { return PyUnicode_FromString("relationship"); }
void Destroy(PyObject*) { ++destroyed; PyErr_SetString(PyExc_TypeError, "destructor error"); }
PyObject* Prim(const SdfSpec&)
{
    PyGILState_STATE state = PyGILState_Ensure();
    const bool held = state == PyGILState_LOCKED;
    PyGILState_Release(state);
    CHECK(held);
    ++calls;
    if (mode == 1) { PyErr_SetString(PyExc_ValueError, "holder failed"); return nullptr; }
    if (mode == 2) return nullptr;
    if (mode == 3) {
        PyObject* capsule = PyCapsule_New(&destroyed, "holder-result", Destroy);
        PyErr_SetString(PyExc_ValueError, "original holder error");
        return capsule;
    }
    if (mode == 4) _RegisterHolderCreator(typeid(SdfRelationshipSpec), Relation);
    return PyUnicode_FromString("prim");
}
PyObject* Other(const SdfSpec&) { return PyUnicode_FromString("other"); }
PyObject* Variant(const SdfSpec&) { return PyUnicode_FromString("variant"); }
void Expect(const std::type_info& type, const SdfSpec& spec, const char* expected)
{
    PyObject* result = _CreateHolder(type, spec);
    CHECK(result && !PyErr_Occurred());
    CHECK(PyUnicode_CompareWithASCIIString(result, expected) == 0);
    Py_DECREF(result);
}
int Run()
{
    std::puts("Creating native specs and registering before Python initialization"); std::fflush(stdout);
    CHECK(!Py_IsInitialized());
    CHECK(InitializeNativeSpecs());
    // Layer edits can load _sdf once Python is running. Create an expired spec
    // beforehand; real binding tests cover edits while Python is active.
    SdfSpec expired = NativePrim();
    RemoveNativePrim();
    CHECK(expired.IsDormant());
    CHECK(InitializeNativeSpecs());
    _RegisterHolderCreator(typeid(SdfPrimSpec), Prim); // Also safe before Python starts.
    CHECK(!Py_IsInitialized());
    Py_Initialize();
    TfPyLock gil;
    std::puts("Checking dispatch and duplicate registration"); std::fflush(stdout);
    Expect(typeid(SdfSpec), NativePrim(), "prim");
    SdfSpec dormant;
    PyObject* none = _CreateHolder(typeid(SdfSpec), dormant);
    CHECK(none == Py_None);
    Py_DECREF(none);
    none = _CreateHolder(typeid(SdfSpec), expired);
    CHECK(none == Py_None);
    Py_DECREF(none);
    {
        TfErrorMark errors;
        none = _CreateHolder(typeid(SdfSpec), NativeAttribute());
        CHECK(none == Py_None && !errors.IsClean());
        Py_DECREF(none);
        errors.Clear();
        _RegisterHolderCreator(typeid(SdfAttributeSpec), Other);
        _RegisterHolderCreator(typeid(SdfPrimSpec), Other);
        CHECK(!errors.IsClean());
        errors.Clear();
        _RegisterHolderCreator(typeid(UnregisteredType), Other);
        CHECK(!errors.IsClean());
        errors.Clear();
        _RegisterHolderCreator(typeid(SdfPrimSpec), nullptr);
        CHECK(!errors.IsClean());
        errors.Clear();
        none = _CreateHolder(typeid(SdfRelationshipSpec), NativePrim());
        CHECK(none == Py_None && errors.IsClean());
        Py_DECREF(none);
    }
    Expect(typeid(SdfSpec), NativePrim(), "prim"); // Duplicate did not replace it.
    Expect(typeid(SdfSpec), NativeAttribute(), "other");
    _RegisterHolderCreator(typeid(SdfVariantSpec), Variant);
    Expect(typeid(SdfSpec), NativeVariant(), "variant");
    Expect(typeid(SdfPrimSpec), NativeVariant(), "prim");
    PyErr_SetString(PyExc_KeyError, "existing error");
    std::puts("Checking pending errors and creator failures"); std::fflush(stdout);
    const int before = calls;
    CHECK(!_CreateHolder(typeid(SdfSpec), NativePrim()));
    CHECK(calls == before && PyErr_ExceptionMatches(PyExc_KeyError));
    PyErr_Clear();
    mode = 1;
    CHECK(!_CreateHolder(typeid(SdfSpec), NativePrim()));
    CHECK(PyErr_ExceptionMatches(PyExc_ValueError));
    PyErr_Clear();
    mode = 2;
    CHECK(!_CreateHolder(typeid(SdfSpec), NativePrim()));
    CHECK(PyErr_ExceptionMatches(PyExc_RuntimeError));
    PyErr_Clear();
    mode = 3;
    CHECK(!_CreateHolder(typeid(SdfSpec), NativePrim()));
    CHECK(destroyed == 1 && PyErr_ExceptionMatches(PyExc_ValueError));
    PyErr_Clear();
    mode = 4;
    std::puts("Checking reentrancy and worker-thread GIL"); std::fflush(stdout);
    Expect(typeid(SdfSpec), NativePrim(), "prim"); // Registration inside callback.
    mode = 0;
    Expect(typeid(SdfSpec), NativeRelationship(), "relationship");
    std::exception_ptr failure;
    gil.BeginAllowThreads();
    std::thread worker([&] {
        try {
            PyObject* result = _CreateHolder(typeid(SdfSpec), NativePrim());
            TfPyLock lock;
            CHECK(result && PyUnicode_CompareWithASCIIString(result, "prim") == 0);
            Py_DECREF(result);
        } catch (...) { failure = std::current_exception(); }
    });
    worker.join();
    gil.EndAllowThreads();
    if (failure) std::rethrow_exception(failure);
    CHECK(!PyDict_GetItemString(PyImport_GetModuleDict(), "pxr.Sdf"));
    CHECK(!PyErr_Occurred());
    std::puts("PASS: shared neutral spec registry, casting, ownership, errors and GIL");
    return 0;
}

int RunModules(int argc, char** argv)
{
    // Native specs precede both the interpreter and the independent modules.
    CHECK(InitializeNativeSpecs() && !Py_IsInitialized());
    Py_Initialize();
    TfPyLock gil;
    PyObject* args = PyList_New(argc - 1);
    CHECK(args);
    for (int i = 1; i < argc; ++i) {
        CHECK(PyList_SetItem(args, i - 1, PyUnicode_FromString(argv[i])) == 0);
    }
    CHECK(PySys_SetObject("argv", args) == 0);
    Py_DECREF(args);
    std::ifstream input(argv[1]);
    CHECK(input.good());
    std::string source((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    PyObject* code = Py_CompileString(source.c_str(), argv[1], Py_file_input);
    CHECK(code);
    PyObject* globals = PyModule_GetDict(PyImport_AddModule("__main__"));
    PyObject* result = PyEval_EvalCode(code, globals, globals);
    Py_DECREF(code);
    if (!result) { PyErr_Print(); return 1; }
    Py_DECREF(result);
    CHECK(!PyDict_GetItemString(PyImport_GetModuleDict(), "pxr.Sdf"));
    return 0;
}
}
int main(int argc, char** argv)
{
    try {
        const int result = argc > 1 ? RunModules(argc, argv) : Run();
        Py_Finalize();
        // Release native scene objects before dependent DLL static teardown.
        ClearNativeSpecs();
        return result;
    }
    catch (std::exception const& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
    catch (...) {
        std::fputs("Unexpected non-standard exception\n", stderr);
        TfPyLock lock;
        if (PyErr_Occurred()) PyErr_Print();
        return 1;
    }
}
