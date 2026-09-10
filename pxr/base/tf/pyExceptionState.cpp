//
// Copyright 2016 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//

#include "pxr/pxr.h"

#include "pxr/base/tf/pyExceptionState.h"
#include "pxr/base/tf/pyLock.h"
#include "pxr/base/tf/pySafePython.h"

using std::string;

PXR_NAMESPACE_OPEN_SCOPE

namespace {

void
XIncRef(PyObject *obj)
{
    if (obj) {
        Py_IncRef(obj);
    }
}

void
XDecRef(PyObject *obj)
{
    if (obj) {
        Py_DecRef(obj);
    }
}

void
XSetNewRef(PyObject **dst, PyObject *src)
{
    PyObject *old = *dst;
    *dst = src;
    XDecRef(old);
}

}

TfPyExceptionState::TfPyExceptionState(TfPyExceptionState const &other)
{
    TfPyLock lock;
    XIncRef(other._type);
    XIncRef(other._value);
    XIncRef(other._trace);
    _type = other._type;
    _value = other._value;
    _trace = other._trace;
}

TfPyExceptionState &
TfPyExceptionState::operator=(TfPyExceptionState const &other)
{
    if (this == &other) {
        return *this;
    }

    TfPyLock lock;
    XIncRef(other._type);
    XIncRef(other._value);
    XIncRef(other._trace);
    XSetNewRef(&_type, other._type);
    XSetNewRef(&_value, other._value);
    XSetNewRef(&_trace, other._trace);
    return *this;
}

TfPyExceptionState::~TfPyExceptionState()
{
    TfPyLock lock;
    XSetNewRef(&_type, nullptr);
    XSetNewRef(&_value, nullptr);
    XSetNewRef(&_trace, nullptr);
}

TfPyExceptionState
TfPyExceptionState::Fetch() {
    TfPyLock lock;
    PyObject *excType = nullptr;
    PyObject *excValue = nullptr;
    PyObject *excTrace = nullptr;
    PyErr_Fetch(&excType, &excValue, &excTrace);
    return TfPyExceptionState(excType, excValue, excTrace);
}

void
TfPyExceptionState::Restore()
{
    TfPyLock lock;
    PyErr_Restore(_type, _value, _trace);
    _type = nullptr;
    _value = nullptr;
    _trace = nullptr;
}

string 
TfPyExceptionState::GetExceptionString() const
{
    TfPyLock lock;
    string s;

    if (!_type) {
        return s;
    }

    PyObject *savedType = nullptr;
    PyObject *savedValue = nullptr;
    PyObject *savedTrace = nullptr;
    PyErr_Fetch(&savedType, &savedValue, &savedTrace);

    PyObject *tbModule = PyImport_ImportModule("traceback");
    PyObject *formatException = tbModule ?
        PyObject_GetAttrString(tbModule, "format_exception") : nullptr;

    PyObject *exception = nullptr;
    if (formatException) {
        exception = PyObject_CallFunctionObjArgs(
            formatException,
            _type,
            _value ? _value : Py_None,
            _trace ? _trace : Py_None,
            nullptr);
    }

    PyObject *iter = exception ? PyObject_GetIter(exception) : nullptr;
    if (iter) {
        PyObject *item = nullptr;
        while ((item = PyIter_Next(iter))) {
            const char *itemStr = PyUnicode_AsUTF8(item);
            if (itemStr) {
                s += itemStr;
            }
            Py_DecRef(item);
            if (!itemStr) {
                PyErr_Clear();
                break;
            }
        }
        Py_DecRef(iter);
    }

    XDecRef(exception);
    XDecRef(formatException);
    XDecRef(tbModule);

    PyErr_Clear();
    PyErr_Restore(savedType, savedValue, savedTrace);

    return s;
}

PXR_NAMESPACE_CLOSE_SCOPE
