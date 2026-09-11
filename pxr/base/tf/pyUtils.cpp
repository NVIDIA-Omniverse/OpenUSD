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

using std::string;

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

bool Tf_PyEvaluateWithErrorCheck(
    const std::string & expr, pxr_boost::python::object * obj)
{
    TfErrorMark m;
    *obj = TfPyEvaluate(expr);
    return m.IsClean();
}

PXR_NAMESPACE_CLOSE_SCOPE
