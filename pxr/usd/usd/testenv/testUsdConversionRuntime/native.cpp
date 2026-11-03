// Copyright 2026 Pixar
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
#include "pxr/usd/usd/pyConversionRuntime.h"
#include "pxr/usd/sdf/schema.h"
#include "pxr/base/vt/value.h"
#include "pxr/base/tf/errorMark.h"
#include "pxr/base/tf/pyError.h"
#include "pxr/base/arch/export.h"
#include <exception>
#ifdef CHECK_RUNTIME_CONTRACTS
#include "pxr/base/vt/valueFromPython.h"
#include "pxr/base/vt/valueToPython.h"
#endif

PXR_NAMESPACE_USING_DIRECTIVE
namespace {
bool unchanged = true;
PyObject* Pack(const VtValue& value)
{
    auto object = UsdVtValueToPython(value);
    if (PyErr_Occurred()) return nullptr;
    return Py_BuildValue("sO", value.GetTypeName().c_str(), object.ptr());
}
PyObject* Failure()
{
    if (!PyErr_Occurred()) PyErr_SetString(PyExc_RuntimeError, "Native conversion failed without a Python error");
    return nullptr;
}
}
extern "C" ARCH_EXPORT PyObject* ConvertType(PyObject* object, const char* name)
{
    try {
        return Pack(UsdPythonToSdfType(TfPyObjWrapper(object, TfPyBorrowedReference),
            SdfSchema::GetInstance().FindType(name)));
    } catch (...) { return Failure(); }
}
extern "C" ARCH_EXPORT PyObject* ConvertMetadata(PyObject* object, const char* key, const char* path)
{
    TfErrorMark mark;
    VtValue result(std::string("unchanged"));
    unchanged = true;
    try {
        bool ok = UsdPythonToMetadataValue(TfToken(key), TfToken(path),
            TfPyObjWrapper(object, TfPyBorrowedReference), &result);
        unchanged = result == VtValue(std::string("unchanged"));
        PyObject* errors = PyList_New(0);
        for (const auto& error : mark) {
            PyObject* text = PyUnicode_FromString(error.GetCommentary().c_str());
            if (!text || PyList_Append(errors, text) < 0) {
                Py_XDECREF(text); Py_DECREF(errors); mark.Clear(); return nullptr;
            }
            Py_DECREF(text);
        }
        mark.Clear();
        PyObject* converted = Pack(result);
        if (!converted) { Py_DECREF(errors); return nullptr; }
        return Py_BuildValue("NNN", PyBool_FromLong(ok), converted, errors);
    } catch (...) {
        unchanged = result == VtValue(std::string("unchanged"));
        mark.Clear();
        return Failure();
    }
}
extern "C" ARCH_EXPORT int OutputUnchanged() { return unchanged; }

#ifdef CHECK_RUNTIME_CONTRACTS
namespace {
int mode = 0, calls = 0;
bool ExtractNative(PyObject* object, VtValue* value)
{
    ++calls;
    if (mode == 1 || mode == 2) {
        PyErr_SetString(PyExc_ValueError, "native backend error");
        if (mode == 2) *value = VtValue(99);
        return mode == 2;
    }
    if (object == Py_None) { *value = VtValue(); return true; }
    long number = PyLong_AsLong(object);
    if (PyErr_Occurred()) return false;
    *value = VtValue(int(number));
    return true;
}
PyObject* ToNativeInt(const void* address)
{
    return PyLong_FromLong(*static_cast<const int*>(address));
}
}
#define CHECK_RUNTIME(condition) do { if (!(condition)) { \
    PyErr_SetString(PyExc_RuntimeError, "Runtime contract failed: " #condition); return 0; } } while (false)
extern "C" ARCH_EXPORT int CheckRuntimeContracts()
{
    try {
        TfPyObjWrapper input(PyLong_FromLong(7), TfPyNewReference);
        const auto type = SdfSchema::GetInstance().FindType("int");
        bool caught = false;
        try { UsdPythonToSdfType(input, type); }
        catch (TfPyErrorAlreadySet const&) { caught = true; }
        CHECK_RUNTIME(caught && PyErr_ExceptionMatches(PyExc_TypeError));
        PyErr_Clear();
        Vt_ValueFromPythonRegistry::RegisterTyped(typeid(VtValue), ExtractNative);
        Vt_ValueToPythonRegistry::Register(typeid(int), ToNativeInt);
        CHECK_RUNTIME(UsdPythonToSdfType(input, type).Get<int>() == 7);
        CHECK_RUNTIME(PyLong_AsLong(UsdVtValueToPython(VtValue(7)).ptr()) == 7);
        VtValue output(std::string("unchanged"));
        CHECK_RUNTIME(UsdPythonToMetadataValue(TfToken("hidden"), TfToken(), input, &output));
        CHECK_RUNTIME(output.IsHolding<bool>() && output.Get<bool>());
        TfPyObjWrapper none;
        CHECK_RUNTIME(UsdPythonToMetadataValue(TfToken("hidden"), TfToken(), none, &output));
        CHECK_RUNTIME(output.IsEmpty());
        // The runtime's lock covers casts and backend callbacks, not just input extraction.
        PyThreadState* state = PyEval_SaveThread();
        std::exception_ptr failure;
        VtValue converted;
        try { converted = UsdPythonToSdfType(input, type); }
        catch (...) { failure = std::current_exception(); }
        PyEval_RestoreThread(state);
        if (failure) std::rethrow_exception(failure);
        CHECK_RUNTIME(converted.Get<int>() == 7);
        for (int errorMode : {1, 2}) {
            mode = errorMode;
            output = VtValue(std::string("unchanged"));
            caught = false;
            try { UsdPythonToMetadataValue(TfToken("hidden"), TfToken(), input, &output); }
            catch (TfPyErrorAlreadySet const&) { caught = true; }
            CHECK_RUNTIME(caught && PyErr_ExceptionMatches(PyExc_ValueError));
            CHECK_RUNTIME(output == VtValue(std::string("unchanged")));
            PyErr_Clear();
        }
        mode = 0;
        const int before = calls;
        PyErr_SetString(PyExc_KeyError, "existing error");
        caught = false;
        try { UsdPythonToSdfType(input, type); }
        catch (TfPyErrorAlreadySet const&) { caught = true; }
        CHECK_RUNTIME(caught && PyErr_ExceptionMatches(PyExc_KeyError) && calls == before);
        PyErr_Clear();
        // An invalid backend result must not escape under the requested type.
        Vt_ValueFromPythonRegistry::RegisterTyped(typeid(TfTokenVector), ExtractNative);
        output = VtValue(std::string("unchanged"));
        caught = false;
        try { UsdPythonToMetadataValue(TfToken("primChildren"), TfToken(), input, &output); }
        catch (TfPyErrorAlreadySet const&) { caught = true; }
        CHECK_RUNTIME(caught && PyErr_ExceptionMatches(PyExc_TypeError));
        CHECK_RUNTIME(output == VtValue(std::string("unchanged")));
        PyErr_Clear();
        return 1;
    } catch (...) { Failure(); return 0; }
}
#endif
