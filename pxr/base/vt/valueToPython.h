//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_BASE_VT_VALUE_TO_PYTHON_H
#define PXR_BASE_VT_VALUE_TO_PYTHON_H

#include "pxr/pxr.h"
#include "pxr/base/vt/api.h"
#include "pxr/base/vt/valueCommon.h"
#include "pxr/base/tf/pyObjWrapper.h"
#include "pxr/base/tf/pySafePython.h"
#include <memory>
#include <typeinfo>
#include <type_traits>

PXR_NAMESPACE_OPEN_SCOPE

class VtValue;
class VtValueRef;

#ifdef PXR_PYTHON_SUPPORT_ENABLED
/// Binding-neutral to-Python dispatch shared by values and value references.
/// Callbacks borrow the typed address for the duration of the call, execute
/// with the GIL held, and return a new Python reference. On failure, return
/// null with a Python exception set; never throw a binding-library exception.
/// Registered code and type information must outlive all uses of the registry.
/// Python must be initialized. Callers inspecting the Python error indicator
/// must keep the GIL held across invocation and inspection.
class Vt_ValueToPythonRegistry {
public:
    using Converter = PyObject *(*)(void const *);
    using Fallback = PyObject *(*)(
        std::type_info const &, void const *,
        std::shared_ptr<void const> const *);

    /// Register or replace a converter for a C++ type.
    VT_API static void Register(std::type_info const &, Converter);

    /// Return whether a type has an explicitly registered converter.
    VT_API static bool HasConverter(std::type_info const &);

    /// Install or replace the binding's runtime converter lookup.
    /// Missing conversions are not cached, so late registrations work.
    VT_API static void RegisterFallback(Fallback);

    /// Convert a borrowed typed address, acquiring the GIL internally.
    /// On failure, return a wrapper holding None and leave the exception set.
    /// The optional shared owner lets a binding recognize objects whose shared
    /// pointer came from Python. It is borrowed only for the callback's duration.
    VT_API static TfPyObjWrapper Invoke(
        std::type_info const &, void const *,
        std::shared_ptr<void const> const *sharedOwner = nullptr);
};

/// Obtain an owned Python wrapper for the held/viewed value. Empty values
/// produce None. On conversion failure, leave a Python exception set and
/// return a wrapper holding None. The caller must check the error indicator.
VT_API TfPyObjWrapper Vt_GetPythonObjectFromHeldValue(VtValue const &);
VT_API TfPyObjWrapper Vt_GetPythonObjectFromHeldValue(VtValueRef const &);

// This template deliberately contains no binding implementation. Type tables
// created before a binding loads always consult the current runtime registry.
template <class T> struct Vt_HasSharedObjectOwner : std::false_type {};
template <class T> struct Vt_HasSharedObjectOwner<std::shared_ptr<T>>
    : std::is_convertible<typename std::shared_ptr<T>::element_type *,
                          void const *> {};

template <class T>
TfPyObjWrapper Vt_ConvertValueToPython(T const &value)
{
    if constexpr (std::is_same_v<typename Vt_ValueGetStored<T>::Type,
                                 TfPyObjWrapper>) {
        if constexpr (std::is_same_v<T, TfPyObjWrapper>) {
            return value;
        } else {
            return TfPyObjWrapper(value.ptr(), TfPyBorrowedReference);
        }
    } else if constexpr (
        std::is_pointer_v<T> &&
        std::is_object_v<std::remove_pointer_t<T>> &&
        !std::is_volatile_v<std::remove_pointer_t<T>> &&
        !std::is_same_v<std::remove_cv_t<std::remove_pointer_t<T>>, char> &&
        !std::is_same_v<std::remove_cv_t<std::remove_pointer_t<T>>, PyObject>) {
        if (Vt_ValueToPythonRegistry::HasConverter(typeid(T))) {
            return Vt_ValueToPythonRegistry::Invoke(
                typeid(T), std::addressof(value));
        }
        // Raw object pointers use the pointee's copying conversion.
        return value ? Vt_ValueToPythonRegistry::Invoke(
            typeid(std::remove_pointer_t<T>), value) : TfPyObjWrapper();
    } else if constexpr (Vt_HasSharedObjectOwner<T>::value) {
        if (!value) {
            return {};
        }
        std::shared_ptr<void const> owner(value, value.get());
        return Vt_ValueToPythonRegistry::Invoke(
            typeid(T), std::addressof(value), &owner);
    } else {
        return Vt_ValueToPythonRegistry::Invoke(typeid(T), std::addressof(value));
    }
}
#endif // PXR_PYTHON_SUPPORT_ENABLED

PXR_NAMESPACE_CLOSE_SCOPE
#endif // PXR_BASE_VT_VALUE_TO_PYTHON_H
