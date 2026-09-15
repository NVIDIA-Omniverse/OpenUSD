//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//

#include "pxr/pxr.h"

#if defined(PXR_PYTHON_SUPPORT_ENABLED) && !defined(Py_LIMITED_API)

#include "pxr/base/tf/diagnosticLite.h"
#include "pxr/base/tf/errorMark.h"
#include "pxr/base/tf/pyError.h"
#include "pxr/base/tf/pyInterpreter.h"
#include "pxr/base/tf/pyLock.h"
#include "pxr/base/tf/pyUtils.h"
#include "pxr/base/tf/scriptModuleLoader.h"

#include "pxr/external/boost/python/dict.hpp"
#include "pxr/external/boost/python/errors.hpp"
#include "pxr/external/boost/python/handle.hpp"
#include "pxr/external/boost/python/object.hpp"
#include "pxr/external/boost/python/object/class_detail.hpp"
#include "pxr/external/boost/python/type_id.hpp"

#include <mutex>

PXR_NAMESPACE_OPEN_SCOPE

using namespace pxr_boost::python;

bool
TfPyIsNone(pxr_boost::python::object const &obj)
{
    return TfPyIsNone(obj.ptr());
}

bool
TfPyIsNone(pxr_boost::python::handle<> const &obj)
{
    return TfPyIsNone(obj.get());
}

std::string
TfPyObjectRepr(pxr_boost::python::object const &t)
{
    return TfPyObjectRepr(t.ptr());
}

std::string
TfPyGetClassName(pxr_boost::python::object const &obj)
{
    return TfPyGetClassName(obj.ptr());
}

pxr_boost::python::object
TfPyEvaluate(std::string const &expr, dict const& extraGlobals)
{
    TfPyLock lock;
    try {
        // Get the modules dict for the loaded script modules.
        dict modulesDict =
            TfScriptModuleLoader::GetInstance().GetModulesDict();

        // Make sure the builtins are available.
        handle<> modHandle(PyImport_ImportModule("builtins"));
        modulesDict["__builtins__"] = object(modHandle);
        modulesDict.update(extraGlobals);

        // Eval the expression in that environment.
        return object(TfPyRunString(expr, Py_eval_input,
                                    modulesDict, modulesDict));
    } catch (pxr_boost::python::error_already_set const &) {
        TfPyConvertPythonExceptionToTfErrors();
        PyErr_Clear();
    }
    return pxr_boost::python::object();
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

pxr_boost::python::object
TfPyCopyBufferToByteArray(const char *buffer, size_t size)
{
    TfPyLock lock;
    pxr_boost::python::object result;

    try {
        pxr_boost::python::handle<> hbuf(
            TfPyCopyBufferToPyByteArray(buffer, size));
        result = pxr_boost::python::object(hbuf);
    } catch (pxr_boost::python::error_already_set const &) {
        TfPyConvertPythonExceptionToTfErrors();
        PyErr_Clear();
    }

    return result;
}

PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_PYTHON_SUPPORT_ENABLED && !Py_LIMITED_API
