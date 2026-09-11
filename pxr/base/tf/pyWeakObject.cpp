//
// Copyright 2016 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/pxr.h"

#include "pxr/base/tf/pyWeakObject.h"
#include "pxr/base/tf/instantiateSingleton.h"

PXR_NAMESPACE_OPEN_SCOPE

struct Tf_PyWeakObjectRegistry
{
    typedef Tf_PyWeakObjectRegistry This;
    
    /// Return the singleton instance.
    static This &GetInstance();
    void Insert(PyObject *obj, Tf_PyWeakObjectPtr const &weakObj);
    Tf_PyWeakObjectPtr Lookup(PyObject *obj) const;
    void Remove(PyObject *obj);

  private:
    Tf_PyWeakObjectRegistry() = default;
    friend class TfSingleton<This>;

    TfHashMap<PyObject *, Tf_PyWeakObjectPtr, TfHash> _weakObjects;
};

TF_INSTANTIATE_SINGLETON(Tf_PyWeakObjectRegistry);

Tf_PyWeakObjectRegistry &
Tf_PyWeakObjectRegistry::GetInstance()
{
    return TfSingleton<This>::GetInstance();
}

void
Tf_PyWeakObjectRegistry::Insert(PyObject *obj,
                                Tf_PyWeakObjectPtr const &weakObj)
{
    _weakObjects[obj] = weakObj;
}

Tf_PyWeakObjectPtr
Tf_PyWeakObjectRegistry::Lookup(PyObject *obj) const
{
    auto iter = _weakObjects.find(obj);
    return iter == _weakObjects.end() ? Tf_PyWeakObjectPtr() : iter->second;
}

void
Tf_PyWeakObjectRegistry::Remove(PyObject *obj)
{
    _weakObjects.erase(obj);
}

namespace {

char const *_WeakObjectPtrCapsuleName = "pxr.Tf._PyWeakObjectPtr";

void
_DeleteWeakObjectPtrCapsule(PyObject *capsule)
{
    void *ptr = PyCapsule_GetPointer(capsule, _WeakObjectPtrCapsuleName);
    if (!ptr) {
        PyErr_Clear();
        return;
    }
    delete static_cast<Tf_PyWeakObjectPtr *>(ptr);
}

PyObject *
_WeakObjectDeleted(PyObject *self, PyObject * /* weakRef */)
{
    void *ptr = PyCapsule_GetPointer(self, _WeakObjectPtrCapsuleName);
    if (!ptr) {
        return nullptr;
    }

    Tf_PyWeakObjectPtr const &weakObj =
        *static_cast<Tf_PyWeakObjectPtr *>(ptr);
    if (weakObj) {
        weakObj->Delete();
    }

    Py_INCREF(Py_None);
    return Py_None;
}

PyObject *
_CreateWeakObjectDeletedCallback(Tf_PyWeakObjectPtr const &self)
{
    static PyMethodDef methodDef = {
        "_Tf_PyWeakObjectDeleted",
        _WeakObjectDeleted,
        METH_O,
        nullptr
    };

    Tf_PyWeakObjectPtr *ptr = new Tf_PyWeakObjectPtr(self);
    PyObject *capsule = PyCapsule_New(
        ptr, _WeakObjectPtrCapsuleName, _DeleteWeakObjectPtrCapsule);
    if (!capsule) {
        delete ptr;
        return nullptr;
    }

    PyObject *callback = PyCFunction_NewEx(&methodDef, capsule, nullptr);
    Py_DECREF(capsule);
    return callback;
}

} // anonymous namespace

Tf_PyWeakObjectPtr
Tf_PyWeakObject::GetOrCreate(PyObject *obj)
{
    // If it's in the registry, return it.
    if (Tf_PyWeakObjectPtr p =
        Tf_PyWeakObjectRegistry::GetInstance().Lookup(obj))
        return p;

    Tf_PyWeakObject *weakObj = new Tf_PyWeakObject(obj);
    Tf_PyWeakObjectPtr self(weakObj);

    PyObject *callback = _CreateWeakObjectDeletedCallback(self);
    if (callback) {
        weakObj->_weakRef = PyWeakref_NewRef(obj, callback);
        Py_DECREF(callback);
    }

    if (!weakObj->_weakRef) {
        PyErr_Clear();
        delete weakObj;
        return Tf_PyWeakObjectPtr();
    }

    // Set our python identity, but release it immediately, since we are a weak
    // reference and will expire as soon as the python object does.
    Tf_PyReleasePythonIdentity(self, weakObj->GetObjectPtr());

    // Install us in the registry.
    Tf_PyWeakObjectRegistry::GetInstance().Insert(obj, self);

    return self;
}

PyObject *
Tf_PyWeakObject::GetObjectPtr() const
{
    return _weakRef ? PyWeakref_GetObject(_weakRef) : nullptr;
}

void
Tf_PyWeakObject::Delete()
{
    Tf_PyWeakObjectRegistry::GetInstance().Remove(_objectKey);
    delete this;
}
    
Tf_PyWeakObject::Tf_PyWeakObject(PyObject *obj)
    : _objectKey(obj)
    , _weakRef(nullptr)
{
}

Tf_PyWeakObject::~Tf_PyWeakObject()
{
    Py_XDECREF(_weakRef);
}

PXR_NAMESPACE_CLOSE_SCOPE
