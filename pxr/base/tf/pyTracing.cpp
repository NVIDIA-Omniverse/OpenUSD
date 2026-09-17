//
// Copyright 2016 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//

#include "pxr/pxr.h"

#include "pxr/base/tf/pyTracing.h"

#ifdef PXR_PYTHON_SUPPORT_ENABLED
#include "pxr/base/tf/pyError.h"
#include "pxr/base/tf/pyInterpreter.h"
#include "pxr/base/tf/pyUtilsImpl.h"
#include "pxr/base/tf/pyUtils.h"
#include "pxr/base/tf/staticData.h"

#include <memory>

#include <tbb/spin_mutex.h>

#include <list>
#include <mutex>

using std::list;

#ifndef PyTrace_CALL
#define PyTrace_CALL 0
#define PyTrace_EXCEPTION 1
#define PyTrace_LINE 2
#define PyTrace_RETURN 3
#define PyTrace_C_CALL 4
#define PyTrace_C_EXCEPTION 5
#define PyTrace_C_RETURN 6
#endif

PXR_NAMESPACE_OPEN_SCOPE

typedef list<std::weak_ptr<TfPyTraceFn> > TraceFnList;

static TfStaticData<TraceFnList> _traceFns;
static bool _traceFnInstalled;
static PyObject *_traceCallable;
static tbb::spin_mutex _traceFnMutex;


static void _SetTraceFnEnabled(bool enable);
static PyObject *_TracePythonCallable(PyObject *, PyObject *args);

static void _InvokeTraceFns(TfPyTraceInfo const &info)
{
    // Take the lock, and swap out the list of trace fns for an empty list.  We
    // do this so we don't hold the lock and call unknown code.  If functions
    // expire while we're executing, that's fine since we .lock() each one to
    // get a dereferenceable shared_ptr, and if new functions are added, that's
    // okay too since we splice the copy back into the official list when we're
    // done.
    TraceFnList local;
    {
        tbb::spin_mutex::scoped_lock lock(_traceFnMutex);
        local.splice(local.end(), *_traceFns);
    }

    // Walk the fns, and invoke them if they're present, erase them if they're
    // not.
    for (TraceFnList::iterator i = local.begin(); i != local.end();) {
        if (TfPyTraceFnId ptr = i->lock()) {
            (*ptr)(info);
            ++i;
        } else {
            local.erase(i++);
        }
    }

    // Now splice the local back into the real list.
    {
        tbb::spin_mutex::scoped_lock lock(_traceFnMutex);
        _traceFns->splice(_traceFns->end(), local);
        // If the list is empty, uninstall the trace fn.
        if (_traceFns->empty())
            _SetTraceFnEnabled(false);
    }
}


static PyMethodDef _tracePythonMethod = {
    "_TracePythonFn",
    _TracePythonCallable,
    METH_VARARGS,
    nullptr
};

static bool
_SetPythonTraceFn(PyObject *callable)
{
    PyObject *sys = PyImport_ImportModule("sys");
    if (!sys) {
        return false;
    }

    PyObject *settrace = PyObject_GetAttrString(sys, "settrace");
    Py_DECREF(sys);
    if (!settrace) {
        return false;
    }

    PyObject *result = PyObject_CallFunctionObjArgs(settrace, callable, nullptr);
    Py_DECREF(settrace);
    if (!result) {
        return false;
    }

    Py_DECREF(result);
    return true;
}

static void _SetTraceFnEnabled(bool enable) {
    // NOTE! mutex must be locked by caller!
    if (enable && !_traceFnInstalled && Py_IsInitialized()) {
        if (!_traceCallable) {
            _traceCallable = PyCFunction_NewEx(
                &_tracePythonMethod, nullptr, nullptr);
        }
        if (!_traceCallable) {
            TfPyConvertPythonExceptionToTfErrors();
            PyErr_Clear();
            return;
        }
        if (!_SetPythonTraceFn(_traceCallable)) {
            TfPyConvertPythonExceptionToTfErrors();
            PyErr_Clear();
            return;
        }
        _traceFnInstalled = true;
    } else if (!enable && _traceFnInstalled) {
        if (!_SetPythonTraceFn(Py_None)) {
            TfPyConvertPythonExceptionToTfErrors();
            PyErr_Clear();
        }
        _traceFnInstalled = false;
    }
}


#if PY_VERSION_HEX < 0x030900B1 && !defined(Py_LIMITED_API)
// Define PyFrame_GetCode() on Python 3.8 and older:
// https://docs.python.org/3.11/whatsnew/3.11.html#id6
static inline PyCodeObject* PyFrame_GetCode(PyFrameObject *frame)
{
    Py_INCREF(frame->f_code);
    return frame->f_code;
}
#endif

static std::string
_GetUTF8(PyObject *obj)
{
    if (!obj) {
        PyErr_Clear();
        return std::string();
    }

    std::string result;
    if (!Tf_PyUnicodeToStdString(obj, &result)) {
        PyErr_Clear();
        return std::string();
    }
    return result;
}

static int
_GetInt(PyObject *obj)
{
    if (!obj) {
        PyErr_Clear();
        return 0;
    }

    long result = PyLong_AsLong(obj);
    if (result == -1 && PyErr_Occurred()) {
        PyErr_Clear();
        return 0;
    }
    return static_cast<int>(result);
}

static int
_TraceEventNameToWhat(std::string const &eventName)
{
    if (eventName == "call") {
        return PyTrace_CALL;
    } else if (eventName == "exception") {
        return PyTrace_EXCEPTION;
    } else if (eventName == "line") {
        return PyTrace_LINE;
    } else if (eventName == "return") {
        return PyTrace_RETURN;
    } else if (eventName == "c_call") {
        return PyTrace_C_CALL;
    } else if (eventName == "c_exception") {
        return PyTrace_C_EXCEPTION;
    } else if (eventName == "c_return") {
        return PyTrace_C_RETURN;
    }

    return -1;
}

static void
_InvokeTraceForFrame(PyObject *frameObj, int what, PyObject *arg)
{
    // Build up a trace info struct.
    TfPyTraceInfo info;
    PyFrameObject *frame = reinterpret_cast<PyFrameObject *>(frameObj);
    PyCodeObject *code = PyFrame_GetCode(frame);
    PyObject *codeObj = reinterpret_cast<PyObject *>(code);
    PyObject *codeName = codeObj
        ? PyObject_GetAttrString(codeObj, "co_name") : nullptr;
    PyObject *codeFileName = codeObj
        ? PyObject_GetAttrString(codeObj, "co_filename") : nullptr;
    PyObject *codeFirstLine = codeObj
        ? PyObject_GetAttrString(codeObj, "co_firstlineno") : nullptr;

    std::string funcName = _GetUTF8(codeName);
    std::string fileName = _GetUTF8(codeFileName);

    info.arg = arg;
    info.funcName = funcName.c_str();
    info.fileName = fileName.c_str();
    info.funcLine = _GetInt(codeFirstLine);
    info.what = what;

    _InvokeTraceFns(info);

    Py_XDECREF(codeFirstLine);
    Py_XDECREF(codeFileName);
    Py_XDECREF(codeName);
    Py_XDECREF(code);
}

static PyObject *
_TracePythonCallable(PyObject *, PyObject *args)
{
    if (PyTuple_Size(args) != 3) {
        PyErr_SetString(PyExc_TypeError,
                        "trace function expects frame, event, and arg");
        return nullptr;
    }

    PyObject *frame = PyTuple_GetItem(args, 0);
    PyObject *event = PyTuple_GetItem(args, 1);
    PyObject *arg = PyTuple_GetItem(args, 2);
    if (!frame || !event || !arg) {
        return nullptr;
    }

    std::string eventName;
    if (!Tf_PyUnicodeToStdString(event, &eventName)) {
        return nullptr;
    }

    int what = _TraceEventNameToWhat(eventName);
    if (what == -1) {
        Py_INCREF(_traceCallable);
        return _traceCallable;
    }

    _InvokeTraceForFrame(frame, what, arg);

    Py_INCREF(_traceCallable);
    return _traceCallable;
}


void Tf_PyFabricateTraceEvent(TfPyTraceInfo const &info)
{
    // NOTE: assumes python lock is held by caller.  Due to that assumption, we
    // know that the list of trace functions could only be growing during this
    // function, and could not go to zero, and have the python trace function be
    // disabled.  So it's safe for us to check the _traceFnInstalled flag here.
    if (_traceFnInstalled)
        _InvokeTraceFns(info);
}

TfPyTraceFnId TfPyRegisterTraceFn(TfPyTraceFn const &f)
{
    tbb::spin_mutex::scoped_lock lock(_traceFnMutex);
    TfPyTraceFnId ret(new TfPyTraceFn(f));
    _traceFns->push_back(ret);
    _SetTraceFnEnabled(true);
    return ret;
}


void Tf_PyTracingPythonInitialized()
{
    static std::once_flag once;
    std::call_once(once, [](){
            TF_AXIOM(Py_IsInitialized());
            tbb::spin_mutex::scoped_lock lock(_traceFnMutex);
            if (!_traceFns->empty())
                _SetTraceFnEnabled(true);
        });
}
            
PXR_NAMESPACE_CLOSE_SCOPE
#endif // PXR_PYTHON_SUPPORT_ENABLED
