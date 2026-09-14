//
// Copyright 2016 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//

#include "pxr/pxr.h"

#include "pxr/base/tf/pyTracing.h"

#ifdef PXR_PYTHON_SUPPORT_ENABLED
#include "pxr/base/tf/pyInterpreter.h"
#include "pxr/base/tf/pyUtils.h"
#include "pxr/base/tf/staticData.h"

#include <memory>

#include <tbb/spin_mutex.h>

#include <list>
#include <mutex>

using std::list;

PXR_NAMESPACE_OPEN_SCOPE

typedef list<std::weak_ptr<TfPyTraceFn> > TraceFnList;

static TfStaticData<TraceFnList> _traceFns;
static bool _traceFnInstalled;
static tbb::spin_mutex _traceFnMutex;


static void _SetTraceFnEnabled(bool enable);

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


static int _TracePythonFn(PyObject *, PyFrameObject *frame,
                          int what, PyObject *arg);

static void _SetTraceFnEnabled(bool enable) {
    // NOTE! mutex must be locked by caller!
    if (enable && !_traceFnInstalled && Py_IsInitialized()) {
        _traceFnInstalled = true;
        PyEval_SetTrace(_TracePythonFn, NULL);
    } else if (!enable && _traceFnInstalled) {
        _traceFnInstalled = false;
        PyEval_SetTrace(NULL, NULL);
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

static char const *
_GetUTF8(PyObject *obj)
{
    if (!obj) {
        PyErr_Clear();
        return "";
    }

    char const *result = PyUnicode_AsUTF8(obj);
    if (!result) {
        PyErr_Clear();
        return "";
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

static int _TracePythonFn(PyObject *, PyFrameObject *frame,
                          int what, PyObject *arg)
{
    // Build up a trace info struct.
    TfPyTraceInfo info;
    PyCodeObject *code = PyFrame_GetCode(frame);
    PyObject *codeObj = reinterpret_cast<PyObject *>(code);
    PyObject *codeName = codeObj
        ? PyObject_GetAttrString(codeObj, "co_name") : nullptr;
    PyObject *codeFileName = codeObj
        ? PyObject_GetAttrString(codeObj, "co_filename") : nullptr;
    PyObject *codeFirstLine = codeObj
        ? PyObject_GetAttrString(codeObj, "co_firstlineno") : nullptr;

    info.arg = arg;
    info.funcName = _GetUTF8(codeName);
    info.fileName = _GetUTF8(codeFileName);
    info.funcLine = _GetInt(codeFirstLine);
    info.what = what;

    _InvokeTraceFns(info);

    Py_XDECREF(codeFirstLine);
    Py_XDECREF(codeFileName);
    Py_XDECREF(codeName);
    Py_XDECREF(code);

    return 0;
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
