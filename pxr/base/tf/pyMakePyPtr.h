//
// Copyright 2016 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_BASE_TF_PY_MAKE_PY_PTR_H
#define PXR_BASE_TF_PY_MAKE_PY_PTR_H

/// \file tf/pyMakePyPtr.h
/// Helper for returning or creating Python objects for Tf pointer types.

#include "pxr/pxr.h"

#include "pxr/base/tf/pyIdentity.h"

#include <utility>

PXR_NAMESPACE_OPEN_SCOPE

// Return an existing PyObject for the pointer paired with false or create and
// return a new PyObject paired with true. The returned PyObject ref count must
// have been incremented. ObjectFactory must return a new reference holding p.
template <typename Ptr, typename ObjectFactory>
std::pair<PyObject*, bool>
Tf_MakePyPtrWithFactory(Ptr const& p, ObjectFactory const& objectFactory)
{
    // null pointers -> python None.
    if (!p.GetUniqueIdentifier()) {
        Py_INCREF(Py_None);
        return std::pair<PyObject*, bool>(Py_None, false);
    }

    // Force instantiation. We must do this before checking if we have a
    // python identity, otherwise the identity might be set during
    // instantiation and our caller will attempt to set it again, which isn't
    // allowed.
    get_pointer(p);

    if (PyObject *id = Tf_PyGetPythonIdentity(p)) {
        return std::pair<PyObject*, bool>(id, false);
    }

    // Just make a new python object holding this pointer.
    PyObject *res = objectFactory(p);
    // If we got back Py_None, no new object was made, so make sure to pass
    // back false in result.
    return std::pair<PyObject*, bool>(res, res != Py_None);
}

PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_BASE_TF_PY_MAKE_PY_PTR_H
