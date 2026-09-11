//
// Copyright 2016 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//

#include "pxr/pxr.h"

#include "pxr/base/tf/error.h"
#include "pxr/base/tf/errorMark.h"
#include "pxr/base/tf/pyEnum.h"
#include "pxr/base/tf/pyError.h"
#include "pxr/base/tf/pyErrorInternal.h"
#include "pxr/base/tf/pyInterpreter.h"
#include "pxr/base/tf/pyLock.h"
#include "pxr/base/tf/pyUtils.h"
#include "pxr/base/tf/scriptModuleLoader.h"

#include "pxr/base/arch/defines.h"
#include "pxr/base/tf/registryManager.h"

#include "pxr/external/boost/python.hpp"
#include "pxr/external/boost/python/detail/api_placeholder.hpp"

#include <mutex>
#include <functional>
#include <vector>

using std::string;
using std::vector;

PXR_NAMESPACE_OPEN_SCOPE

using namespace pxr_boost::python;

bool
TfPyIsNone(pxr_boost::python::object const &obj)
{
    return obj.ptr() == Py_None;
}

bool
TfPyIsNone(pxr_boost::python::handle<> const &obj)
{
    return !obj.get() || obj.get() == Py_None;
}

void Tf_PyLoadScriptModule(std::string const &moduleName)
{
    if (TfPyIsInitialized()) {
        TfPyLock pyLock;
        string tmp(moduleName);
        PyObject *result =
            PyImport_ImportModule(const_cast<char *>(tmp.c_str()));
        if (!result) {
            // CODE_COVERAGE_OFF
            TF_WARN("Import failed for module '%s'!", moduleName.c_str());
            TfPyPrintError();
            // CODE_COVERAGE_ON
        }
    } else {
        // CODE_COVERAGE_OFF
        TF_WARN("Attempted to load module '%s' but Python is not initialized.",
                moduleName.c_str());
        // CODE_COVERAGE_ON
    }
}

bool
TfPyIsInitialized()
{
    return Py_IsInitialized();
}

pxr_boost::python::object
TfPyEvaluate(std::string const &expr, dict const& extraGlobals)
{
    TfPyLock lock;
    try {
        // Get the modules dict for the loaded script modules.
        dict modulesDict =
            TfScriptModuleLoader::GetInstance().GetModulesDict();

        // Make sure the builtins are available
        handle<> modHandle(PyImport_ImportModule("builtins"));
        modulesDict["__builtins__"] = object(modHandle);
        modulesDict.update(extraGlobals);

        // Eval the expression in that enviornment.
        return object(TfPyRunString(expr, Py_eval_input,
                                    modulesDict, modulesDict));
    } catch (pxr_boost::python::error_already_set const &) {
        TfPyConvertPythonExceptionToTfErrors();
        PyErr_Clear();
    }
    return pxr_boost::python::object();
}


int64_t
TfPyNormalizeIndex(int64_t index, uint64_t size, bool throwError)
{
    if (index < 0) 
        index += size;

    if (throwError && (index < 0 || static_cast<uint64_t>(index) >= size)) {
        TfPyThrowIndexError("Index out of range.");
    }

    return index < 0 ? 0 :
                   static_cast<uint64_t>(index) >= size ? size - 1 : index;
}


TF_API void
Tf_PyWrapOnceImpl(
    pxr_boost::python::type_info const &type,
    std::function<void()> const &wrapFunc,
    bool * isTypeWrapped)
{
    static std::mutex pyWrapOnceMutex;

    if (!wrapFunc) {
        TF_CODING_ERROR("Got null wrapFunc");
        return;
    }

    // Acquire the GIL here, just so we can be sure that it is released before
    // attempting to acquire our internal mutex.
    TfPyLock pyLock;
    pyLock.BeginAllowThreads();
    std::lock_guard<std::mutex> lock(pyWrapOnceMutex);
    pyLock.EndAllowThreads();

    // XXX: Double-checked locking
    if (*isTypeWrapped) {
        return;
    }

    pxr_boost::python::type_handle pyType =
        pxr_boost::python::objects::registered_class_object(type);

    if (!pyType) {
        wrapFunc();
    }

    *isTypeWrapped = true;
}


pxr_boost::python::object
TfPyGetClassObject(std::type_info const &type) {
    TfPyLock pyLock;
    return pxr_boost::python::object
        (pxr_boost::python::objects::registered_class_object(type));
}

vector<string> TfPyGetTraceback()
{
    vector<string> result;

    if (!TfPyIsInitialized())
        return result;

    TfPyLock lock;
    // Save the exception state so we can restore it -- getting a traceback
    // should not affect the exception state.
    TfPyExceptionStateScope exceptionStateScope;
    try {
        object tbModule(handle<>(PyImport_ImportModule("traceback")));
        object stack = tbModule.attr("format_stack")();
        size_t size = len(stack);
        result.reserve(size);
        for (size_t i = 0; i < size; ++i) {
            string s = extract<string>(stack[i]);
            result.push_back(s);
        }
    } catch (pxr_boost::python::error_already_set const &) {
        TfPyConvertPythonExceptionToTfErrors();
    }
    return result;
}

static object
_GetOsEnviron()
{
    // In theory, we could just check that the os module has been imported,
    // rather than forcing an import ourself.  However, it's possible that
    // os.environ is actually a re-export from another module (ie. posix,
    // which is the case as of CPython 2.6) that may have been imported
    // without importing os.  Rather than check a hardcoded list of potential
    // modules, we always import os if Python is initialized.  If this turns
    // out to be problematic, we may want to consider the other approach.
    pxr_boost::python::object
        module(pxr_boost::python::handle<>(PyImport_ImportModule("os")));
    pxr_boost::python::object environObj(module.attr("environ"));
    return environObj;
}

bool
TfPySetenv(const std::string & name, const std::string & value)
{
    if (!TfPyIsInitialized()) {
        TF_CODING_ERROR("Python is uninitialized.");
        return false;
    }

    TfPyLock lock;

    try {
        object environObj(_GetOsEnviron());
        environObj[name] = value;
        return true;
    }
    catch (pxr_boost::python::error_already_set&) {
        PyErr_Clear();
    }

    return false;
}

bool
TfPyUnsetenv(const std::string & name)
{
    if (!TfPyIsInitialized()) {
        TF_CODING_ERROR("Python is uninitialized.");
        return false;
    }

    TfPyLock lock;

    try {
        object environObj(_GetOsEnviron());
        object has_key = environObj.attr("__contains__");
        if (has_key(name)) {
            environObj[name].del();
        }
        return true;
    }
    catch (pxr_boost::python::error_already_set&) {
        PyErr_Clear();
    }

    return false;
}

bool Tf_PyEvaluateWithErrorCheck(
    const std::string & expr, pxr_boost::python::object * obj)
{
    TfErrorMark m;
    *obj = TfPyEvaluate(expr);
    return m.IsClean();
}

void
TfPyPrintError()
{
    if (!PyErr_ExceptionMatches(PyExc_KeyboardInterrupt)) {
        PyErr_Print();
    }
}

void
Tf_PyObjectError(bool printError)
{
    // Silently pass these exceptions through.
    if (PyErr_ExceptionMatches(PyExc_SystemExit) ||
        PyErr_ExceptionMatches(PyExc_KeyboardInterrupt)) {
        return;
    }

    // Report and clear.
    if (printError) {
        PyErr_Print();
    }
    else {
        PyErr_Clear();
    }
}

PXR_NAMESPACE_CLOSE_SCOPE
