//
// Copyright 2016 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/base/vt/valueFromPython.h"
#include "pxr/base/vt/value.h"
#include "pxr/base/tf/diagnostic.h"
#include "pxr/base/tf/pyLock.h"
#include "pxr/base/tf/pyUtils.h"
#include "pxr/base/tf/instantiateSingleton.h"
#include <memory>

PXR_NAMESPACE_OPEN_SCOPE

TF_INSTANTIATE_SINGLETON(Vt_ValueFromPythonRegistry);

Vt_ValueFromPythonRegistry::~Vt_ValueFromPythonRegistry() = default;

Vt_ValueFromPythonRegistry &
Vt_ValueFromPythonRegistry::_GetInstance()
{
    return TfSingleton<Vt_ValueFromPythonRegistry>::GetInstance();
}

bool
Vt_ValueFromPythonRegistry::HasConversions()
{
    TfPyLock lock;
    auto &self = _GetInstance();
    return !self._lvalueExtractors.empty() && !self._rvalueExtractors.empty();
}

void
Vt_ValueFromPythonRegistry::Register(Extractor lvalue, Extractor rvalue)
{
    if (!TfPyIsInitialized()) {
        TF_FATAL_ERROR("Tried to register a VtValue from python conversion "
                       "but python is not initialized!");
    }
    if (!lvalue) {
        TF_CODING_ERROR("A VtValue conversion requires an lvalue callback");
        return;
    }
    TfPyLock lock;
    auto &self = _GetInstance();
    self._lvalueExtractors.push_back(lvalue);
    if (rvalue) {
        self._rvalueExtractors.push_back(rvalue);
    }
    ++self._registrationGeneration;
    // Releasing type owners may reenter Python and register more callbacks.
    // Retire entries outside the live container.
    _LValueExtractorCache retired;
    retired.swap(self._lvalueExtractorCache);
}

VtValue
Vt_ValueFromPythonRegistry::Invoke(PyObject *obj)
{
    TfPyLock lock;
    if (PyErr_Occurred()) {
        return VtValue();
    }
    if (!obj) {
        PyErr_SetString(PyExc_TypeError, "Cannot convert a null Python object");
        return VtValue();
    }
    auto &self = _GetInstance();
    std::unique_ptr<PyObject, decltype(&Py_DecRef)> type(
        PyObject_Type(obj), &Py_DecRef);
    if (!type) {
        return VtValue();
    }
    // Callbacks can register more callbacks. Copy function pointers before
    // calling them and keep this invocation's original search bounds.
    const size_t generation = self._registrationGeneration;
    const size_t lvalueCount = self._lvalueExtractors.size();
    const size_t rvalueCount = self._rvalueExtractors.size();
    auto cached = self._lvalueExtractorCache.find(type.get());
    if (cached != self._lvalueExtractorCache.end()) {
        Extractor extract = cached->second.extract;
        VtValue result = extract(obj);
        if (PyErr_Occurred()) {
            return VtValue();
        }
        if (!result.IsEmpty()) {
            return result;
        }
    }
    for (size_t i = lvalueCount; i != 0; --i) {
        Extractor extract = self._lvalueExtractors[i - 1];
        VtValue result = extract(obj);
        if (PyErr_Occurred()) {
            return VtValue();
        }
        if (!result.IsEmpty()) {
            if (generation == self._registrationGeneration) {
                self._lvalueExtractorCache.erase(type.get());
                self._lvalueExtractorCache.insert(
                    {type.get(), _CachedExtractor{
                        TfPyObjWrapper(type.get(), TfPyBorrowedReference),
                        extract}});
            }
            return result;
        }
    }
    for (size_t i = rvalueCount; i != 0; --i) {
        Extractor extract = self._rvalueExtractors[i - 1];
        VtValue result = extract(obj);
        if (PyErr_Occurred()) {
            return VtValue();
        }
        if (!result.IsEmpty()) {
            return result;
        }
    }
    return VtValue();
}

PXR_NAMESPACE_CLOSE_SCOPE
