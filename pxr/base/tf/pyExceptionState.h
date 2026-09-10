//
// Copyright 2016 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//

#include "pxr/pxr.h"
#include "pxr/base/tf/api.h"

#include <string>

typedef struct _object PyObject;

PXR_NAMESPACE_OPEN_SCOPE

struct TfPyExceptionState {
    TF_API
    ~TfPyExceptionState();

    TF_API
    TfPyExceptionState (TfPyExceptionState const &);

    TF_API
    TfPyExceptionState &operator=(TfPyExceptionState const &);

    // Extract Python's current exception state as by PyErr_Fetch() and return
    // it in a TfPyExceptionState.  This leaves Python's current exception state
    // clear.
    TF_API
    static TfPyExceptionState Fetch();

    // Returned pointers are borrowed references.
    PyObject *GetType() const { return _type; }
    PyObject *GetValue() const { return _value; }
    PyObject *GetTrace() const { return _trace; }

    // Move this object's exception state into Python's current exception state,
    // as by PyErr_Restore().  This leaves this object's exception state clear.
    TF_API
    void Restore();

    // Format a Python traceback for the exception state held by this object, as
    // by traceback.format_exception().
    TF_API
    std::string GetExceptionString() const;

private:
    // Takes ownership of new references, as returned by PyErr_Fetch().
    TfPyExceptionState(PyObject *type, PyObject *value, PyObject *trace) :
        _type(type), _value(value), _trace(trace) {}

    PyObject *_type, *_value, *_trace;
};

PXR_NAMESPACE_CLOSE_SCOPE
