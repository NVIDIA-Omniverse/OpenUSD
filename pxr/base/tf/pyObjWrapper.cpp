//
// Copyright 2016 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//

#include "pxr/pxr.h"

#include "pxr/base/tf/pyObjWrapper.h"

#ifdef PXR_PYTHON_SUPPORT_ENABLED
#include "pxr/base/tf/pyErrorImpl.h"
#include "pxr/base/tf/pyLock.h"
#include "pxr/base/tf/type.h"

PXR_NAMESPACE_OPEN_SCOPE

TF_REGISTRY_FUNCTION(TfType)
{
    TfType::Define<TfPyObjWrapper>();
}

namespace {

PyObject *
_ExpectNonNull(PyObject *obj)
{
    if (!obj) {
        Tf_PyThrowErrorAlreadySet();
    }
    return obj;
}

PyObject *
_NewReferenceFromBorrowed(PyObject *obj)
{
    TfPyLock lock;
    Py_INCREF(_ExpectNonNull(obj));
    return obj;
}

// A custom deleter for shared_ptr<PyObject> that takes the python lock before
// decrementing the python object's refcount.  This is necessary since it's
// invalid to decrement the python refcount without holding the lock.
struct _DecrefObjectWithLock {
    void operator()(PyObject *obj) const {
        PXR_NS::TfPyLock lock;
        Py_DECREF(obj);
    }
};

}

TfPyObjWrapper::TfPyObjWrapper()
    : TfPyObjWrapper(Py_None, TfPyBorrowedReference)
{
}

TfPyObjWrapper::TfPyObjWrapper(PyObject *obj, TfPyBorrowedReferenceTag)
    : _objectPtr(_NewReferenceFromBorrowed(obj), _DecrefObjectWithLock())
{
}

TfPyObjWrapper::TfPyObjWrapper(PyObject *obj, TfPyNewReferenceTag)
    : _objectPtr(_ExpectNonNull(obj), _DecrefObjectWithLock())
{
}

PyObject *
TfPyObjWrapper::ptr() const
{
    return _objectPtr.get();
}

bool
TfPyObjWrapper::operator==(TfPyObjWrapper const &other) const
{
    // If they point to the exact same object instance, we know they're equal.
    if (ptr() == other.ptr())
        return true;

    // Otherwise lock and let python determine equality.
    TfPyLock lock;
    int result = PyObject_RichCompareBool(ptr(), other.ptr(), Py_EQ);
    if (result == -1) {
        Tf_PyThrowErrorAlreadySet();
    }
    return result == 1;
}

bool
TfPyObjWrapper::operator!=(TfPyObjWrapper const &other) const
{
    return !(*this == other);
}

PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_PYTHON_SUPPORT_ENABLED
