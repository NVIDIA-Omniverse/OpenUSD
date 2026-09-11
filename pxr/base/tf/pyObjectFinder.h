//
// Copyright 2016 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_BASE_TF_PY_OBJECT_FINDER_H
#define PXR_BASE_TF_PY_OBJECT_FINDER_H

#include "pxr/pxr.h"

#include "pxr/base/tf/api.h"
#include "pxr/base/tf/pyIdentity.h"

#ifndef Py_LIMITED_API
#include "pxr/external/boost/python/handle.hpp"
#include "pxr/external/boost/python/object.hpp"
#endif

#include <typeinfo>

PXR_NAMESPACE_OPEN_SCOPE

struct Tf_PyObjectFinderBase {
    TF_API virtual ~Tf_PyObjectFinderBase();
    // Returns a new reference, or nullptr if no Python object was found.
    virtual PyObject *Find(void const *objPtr) const = 0;
};

template <class T, class PtrType>
struct Tf_PyObjectFinder : public Tf_PyObjectFinderBase {
    virtual ~Tf_PyObjectFinder() {}
    virtual PyObject *Find(void const *objPtr) const {
        TfPyLock lock;
        void *p = const_cast<void *>(objPtr);
        return Tf_PyGetPythonIdentity(PtrType(static_cast<T *>(p)));
    }
};

TF_API
void Tf_RegisterPythonObjectFinderInternal(std::type_info const &type,
                                           Tf_PyObjectFinderBase const *finder);

template <class T, class PtrType>
void Tf_RegisterPythonObjectFinder() {
    Tf_RegisterPythonObjectFinderInternal(typeid(T),
                                          new Tf_PyObjectFinder<T, PtrType>());
}

PyObject *
Tf_PyFindPythonObject(void const *objPtr, std::type_info const &type);

#ifndef Py_LIMITED_API
TF_API
pxr_boost::python::object
Tf_FindPythonObject(void const *objPtr, std::type_info const &type);
#endif

PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_BASE_TF_PY_OBJECT_FINDER_H
