//
// Copyright 2016 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_BASE_TF_PY_OBJ_WRAPPER_H
#define PXR_BASE_TF_PY_OBJ_WRAPPER_H

#include "pxr/pxr.h"

#include "pxr/base/tf/api.h"
#include "pxr/base/arch/pragmas.h"

#ifdef PXR_PYTHON_SUPPORT_ENABLED
// Include this header first to pick up additional mitigations
// for build issues when including Python.h
#include "pxr/base/tf/pySafePython.h"

#include <iosfwd>
#include <memory>
#include <type_traits>

#else

#include <type_traits>

#endif

PXR_NAMESPACE_OPEN_SCOPE

struct TfPyBorrowedReferenceTag {};
struct TfPyNewReferenceTag {};

inline constexpr TfPyBorrowedReferenceTag TfPyBorrowedReference = {};
inline constexpr TfPyNewReferenceTag TfPyNewReference = {};

// We define the empty stub for ABI compatibility even if Python support is
// enabled so we can make sure size and alignment is the same.
class TfPyObjWrapperStub
{
public:
    static constexpr std::size_t Size = 16;
    static constexpr std::size_t Align = 8;

private:
    ARCH_PRAGMA_PUSH
    ARCH_PRAGMA_UNUSED_PRIVATE_FIELD
    std::aligned_storage<Size, Align>::type _stub;
    ARCH_PRAGMA_POP
};


/// \class TfPyObjWrapper
///
/// Python object wrapper.
///
/// Provides a wrapper around a Python object that works correctly for the
/// following basic operations regardless of the GIL state: default construction,
/// copy construction, assignment, (in)equality comparison, hash_value(), and
/// destruction.
///
/// Python refcount operations require the GIL. This class acquires the GIL for
/// default construction, construction from PyObject*, destruction, and for some
/// (in)equality comparisons. The other operations do not require taking the GIL.
///
/// This is primarily useful in cases where a Python object might be
/// destroyed without a locked GIL by a client blind to that fact.  This occurs
/// when a registry, for example, holds type-erased objects.  If one
/// of the type-erased objects in the registry happens to hold a
/// Python object, that type-erased object must be destroyed while the
/// GIL is held but it's unreasonable to require that the registry know that.
/// This class helps solve that problem.
///
#ifdef PXR_PYTHON_SUPPORT_ENABLED
class TfPyObjWrapper
{
public:

    /// Default construct a TfPyObjWrapper holding a reference to python None.
    /// The GIL need not be held by the caller.
    TF_API TfPyObjWrapper();

    /// Construct a TfPyObjectWrapper from a borrowed reference to \a obj.
    /// The GIL need not be held by the caller.
    TF_API TfPyObjWrapper(PyObject *obj, TfPyBorrowedReferenceTag);

    /// Construct a TfPyObjectWrapper from a new reference to \a obj.
    /// The GIL need not be held by the caller. This function steals the
    /// reference.
    TF_API TfPyObjWrapper(PyObject *obj, TfPyNewReferenceTag);

    /// Underlying PyObject* access.
    /// This method returns a pointer, so technically, the GIL need not be
    /// held to call this. However, the caller is strongly advised to ensure
    /// the GIL is held, since assigning this object to another or otherwise
    /// operating on the returned object requires it.  The returned PyObject *
    /// is a "borrowed reference", meaning that the underlying object's
    /// reference count has not been incremented on behalf of the caller.
    TF_API PyObject *ptr() const;

    /// Produce a hash code for this object.
    /// Note that this does not attempt to hash the underlying python object,
    /// it returns a hash code that's suitable for hash-table lookup of
    /// TfPyObjWrapper instances, and does not require taking the python lock.
    friend inline size_t hash_value(TfPyObjWrapper const &o) {
        return (size_t) o.ptr();
    }

    /// Equality.
    /// Returns true if \a other refers to the same python object.
    TF_API bool operator==(TfPyObjWrapper const &other) const;

    /// Inequality.
    /// Returns false if \a other refers to the same python object.
    TF_API bool operator!=(TfPyObjWrapper const &other) const;

private:

    // Store a shared_ptr to a PyObject. The shared_ptr preserves this class's
    // existing size while keeping Boost.Python out of the owned representation.
    std::shared_ptr<PyObject> _objectPtr;
};

static_assert(sizeof(TfPyObjWrapper) == sizeof(TfPyObjWrapperStub),
              "ABI break: Incompatible class sizes.");
static_assert(alignof(TfPyObjWrapper) == alignof(TfPyObjWrapperStub),
              "ABI break: Incompatible class alignments.");

#else // PXR_PYTHON_SUPPORT_ENABLED

class TfPyObjWrapper : TfPyObjWrapperStub
{
};

#endif // PXR_PYTHON_SUPPORT_ENABLED

PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_BASE_TF_PY_OBJ_WRAPPER_H
