//
// Copyright 2016 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_BASE_VT_VALUE_FROM_PYTHON_H
#define PXR_BASE_VT_VALUE_FROM_PYTHON_H

/// \file vt/valueFromPython.h
#include "pxr/pxr.h"
#include "pxr/base/vt/api.h"
#include "pxr/base/tf/hash.h"
#include "pxr/base/tf/hashmap.h"
#include "pxr/base/tf/pyObjWrapper.h"
#include "pxr/base/tf/pySafePython.h"
#include "pxr/base/tf/singleton.h"
#include <vector>
#include <typeindex>
#include <unordered_map>

PXR_NAMESPACE_OPEN_SCOPE

class VtValue;

/// Shared, binding-independent registry of Python-to-VtValue conversions.
///
/// Registration and invocation acquire the GIL. Callbacks borrow their input
/// and run with the GIL held. Return an empty value for a non-match; set a
/// Python exception and return an empty value for a failed conversion. Errors
/// stop the search and are left for the caller to propagate. Callbacks must
/// not throw binding-library exceptions across this interface.
///
/// Callbacks are non-owning function pointers and must remain valid for the
/// lifetime of the registry. Unloading registered binding code is unsupported.
class Vt_ValueFromPythonRegistry {
public:
    using Extractor = VtValue (*)(PyObject *);
    using TypedExtractor = bool (*)(PyObject *, VtValue *);

    /// Register extraction with a requested C++ type, independently of the
    /// generic inference order. New registrations replace the previous callback.
    /// The callback borrows its input and returns true with a converted value,
    /// false for a non-match, or false with a pending Python exception on error.
    /// It must not throw binding-library exceptions. GIL and lifetime rules are
    /// the same as Register(). Requesting typeid(VtValue) denotes the backend's
    /// complete generic conversion, including builtins and empty values.
    VT_API static void RegisterTyped(const std::type_info&, TypedExtractor);

    /// Invoke a requested-type extractor, preserving backend conversion rules
    /// rather than casting a generically inferred VtValue. Missing registration
    /// or non-match returns false; errors leave a Python exception pending.
    /// Output is unchanged on failure. An existing Python error is preserved
    /// without invoking callbacks. Both operations acquire the GIL internally.
    VT_API static bool InvokeTyped(PyObject*, const std::type_info&, VtValue*);

    /// True once both lvalue and rvalue conversions have been registered.
    VT_API static bool HasConversions();

    /// Try lvalue callbacks before rvalue callbacks, newest first in each
    /// group. A successful lvalue callback is cached by Python type. A cached
    /// callback may decline an object, causing the normal search to resume.
    /// Lvalue callbacks should test type compatibility: a cached choice is
    /// assumed to remain appropriate until the next registration.
    /// Pending Python exceptions are preserved without invoking callbacks.
    VT_API static VtValue Invoke(PyObject *obj);

    /// Register a required lvalue callback and an optional rvalue callback.
    /// Registration invalidates cached choices. Registration from a callback
    /// takes effect on the next invocation, not the invocation in progress.
    VT_API static void Register(Extractor lvalue, Extractor rvalue = nullptr);

    Vt_ValueFromPythonRegistry(Vt_ValueFromPythonRegistry const&) = delete;
    Vt_ValueFromPythonRegistry& operator=(
        Vt_ValueFromPythonRegistry const&) = delete;
    Vt_ValueFromPythonRegistry(Vt_ValueFromPythonRegistry &&) = delete;
    Vt_ValueFromPythonRegistry& operator=(
        Vt_ValueFromPythonRegistry &&) = delete;

private:
    Vt_ValueFromPythonRegistry() = default;
    VT_API ~Vt_ValueFromPythonRegistry();
    friend class TfSingleton<Vt_ValueFromPythonRegistry>;
    VT_API static Vt_ValueFromPythonRegistry &_GetInstance();

    std::vector<Extractor> _lvalueExtractors;
    std::vector<Extractor> _rvalueExtractors;
    std::unordered_map<std::type_index, TypedExtractor> _typedExtractors;
    struct _CachedExtractor {
        TfPyObjWrapper type;
        Extractor extract;
    };
    // Compare type identity, without calling a user-defined metaclass hash.
    using _LValueExtractorCache =
        TfHashMap<PyObject *, _CachedExtractor, TfHash>;
    _LValueExtractorCache _lvalueExtractorCache;
    size_t _registrationGeneration = 0;
};

VT_API_TEMPLATE_CLASS(TfSingleton<Vt_ValueFromPythonRegistry>);

PXR_NAMESPACE_CLOSE_SCOPE
#endif // PXR_BASE_VT_VALUE_FROM_PYTHON_H
