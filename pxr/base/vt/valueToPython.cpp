//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/base/vt/valueToPython.h"
#include "pxr/base/tf/diagnostic.h"
#include "pxr/base/tf/pyLock.h"
#include "pxr/base/tf/pyUtils.h"
#include <map>
#include <typeindex>

PXR_NAMESPACE_OPEN_SCOPE
namespace {
struct State {
    std::map<std::type_index, Vt_ValueToPythonRegistry::Converter> converters;
    Vt_ValueToPythonRegistry::Fallback fallback = nullptr;
};
State &GetState()
{
    // Only non-owning callbacks and RTTI keys, like binding registries generally.
    // Never destroy state while Python or binding libraries are shutting down.
    static State *state = new State;
    return *state;
}
void RequirePython()
{
    if (!TfPyIsInitialized()) {
        TF_FATAL_ERROR("Vt Python conversion requires an initialized interpreter");
    }
}
}

void Vt_ValueToPythonRegistry::Register(std::type_info const &type, Converter fn)
{
    RequirePython();
    if (!fn) {
        TF_CODING_ERROR("A Vt to-Python converter cannot be null");
        return;
    }
    TfPyLock lock;
    GetState().converters[type] = fn;
}

void Vt_ValueToPythonRegistry::RegisterFallback(Fallback fn)
{
    RequirePython();
    if (!fn) {
        TF_CODING_ERROR("A Vt to-Python fallback cannot be null");
        return;
    }
    TfPyLock lock;
    GetState().fallback = fn;
}

bool Vt_ValueToPythonRegistry::HasConverter(std::type_info const &type)
{
    TfPyLock lock;
    return GetState().converters.count(type) != 0;
}

TfPyObjWrapper
Vt_ValueToPythonRegistry::Invoke(
    std::type_info const &type, void const *value,
    std::shared_ptr<void const> const *sharedOwner)
{
    TfPyLock lock;
    if (PyErr_Occurred()) {
        return {};
    }
    if (!value) {
        PyErr_SetString(PyExc_TypeError, "Cannot convert a null C++ value address");
        return {};
    }
    State &state = GetState();
    auto found = state.converters.find(type);
    // Copy the callback before invoking it: callbacks may register converters.
    Converter convert = found == state.converters.end() ? nullptr : found->second;
    Fallback fallback = state.fallback;
    PyObject *result = convert ? convert(value) :
        fallback ? fallback(type, value, sharedOwner) : nullptr;
    if (!result) {
        if (!PyErr_Occurred()) {
            PyErr_Format(PyExc_TypeError,
                         "No to-Python converter registered for C++ type '%s'",
                         type.name());
        }
        return {};
    }
    if (PyErr_Occurred()) {
        Py_DECREF(result);
        return {};
    }
    return TfPyObjWrapper(result, TfPyNewReference);
}

PXR_NAMESPACE_CLOSE_SCOPE
