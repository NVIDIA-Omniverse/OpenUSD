//
// Copyright 2016 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifdef PXR_BASE_TF_PY_MODULE_BOOST_H
#error This file should only be included once in any given source (.cpp) file.
#endif
#define PXR_BASE_TF_PY_MODULE_BOOST_H

/// \file tf/pyModuleBoost.h
/// Boost.Python module initialization helpers.

#include "pxr/pxr.h"
#include "pxr/base/arch/attributes.h"
#include "pxr/base/arch/defines.h"
#include "pxr/base/tf/error.h"
#include "pxr/base/tf/errorMark.h"
#include "pxr/base/tf/hash.h"
#include "pxr/base/tf/hashset.h"
#include "pxr/base/tf/mallocTag.h"
#include "pxr/base/tf/preprocessorUtilsLite.h"
#include "pxr/base/tf/pyError.h"
#include "pxr/base/tf/pyModuleNotice.h"
#include "pxr/base/tf/pyTracing.h"
#include "pxr/base/tf/pyUtils.h"
#include "pxr/base/tf/pyWrapContext.h"
#include "pxr/base/tf/scriptModuleLoader.h"
#include "pxr/base/tf/stringUtils.h"
#include "pxr/base/tf/token.h"

#include "pxr/external/boost/python/dict.hpp"
#include "pxr/external/boost/python/docstring_options.hpp"
#include "pxr/external/boost/python/extract.hpp"
#include "pxr/external/boost/python/handle.hpp"
#include "pxr/external/boost/python/module.hpp"
#include "pxr/external/boost/python/object.hpp"
#include "pxr/external/boost/python/object/function.hpp"
#include "pxr/external/boost/python/raw_function.hpp"
#include "pxr/external/boost/python/scope.hpp"
#include "pxr/external/boost/python/tuple.hpp"
#include "pxr/external/boost/python/type_list.hpp"

#include <cstring>
#include <string>

// Helper macros for module files. If you implement your wrappers for classes
// as functions named wrapClassName(), then you can create your module like
// this:
//
// TF_WRAP_MODULE(ModuleName) {
//    TF_WRAP(ClassName1);
//    TF_WRAP(ClassName2);
//    TF_WRAP(ClassName3);
// }
//

// Forward declare the function that will be provided that does the wrapping.
static void WrapModule();

PXR_NAMESPACE_OPEN_SCOPE

class Tf_BoostModuleProcessor {
public:

    typedef Tf_BoostModuleProcessor This;

    using Object = pxr_boost::python::object;
    using WalkCallbackFn =
        bool (This::*) (char const *, Object const &, Object const &);

    inline bool IsBoostPythonFunc(Object const &obj)
    {
        if (!_cachedBPFuncType) {
            pxr_boost::python::handle<> typeStr(
                PyObject_Str((PyObject *)obj.ptr()->ob_type));
            if (strstr(PyUnicode_AsUTF8(typeStr.get()),
                       "Boost.Python.function")) {
                _cachedBPFuncType = (PyObject *)obj.ptr()->ob_type;
                return true;
            }
            return false;
        }
        return (PyObject *)obj.ptr()->ob_type == _cachedBPFuncType;
    }

    inline bool IsBoostPythonClass(Object const &obj)
    {
        if (!_cachedBPClassType) {
            pxr_boost::python::handle<> typeStr(
                PyObject_Str((PyObject *)obj.ptr()->ob_type));
            if (strstr(PyUnicode_AsUTF8(typeStr.get()),
                       "Boost.Python.class")) {
                _cachedBPClassType = (PyObject *)obj.ptr()->ob_type;
                return true;
            }
            return false;
        }
        return (PyObject *)obj.ptr()->ob_type == _cachedBPClassType;
    }

    inline bool IsProperty(Object const &obj)
    {
        return PyObject_TypeCheck(obj.ptr(), &PyProperty_Type);
    }

    inline bool IsStaticMethod(Object const &obj)
    {
        return PyObject_TypeCheck(obj.ptr(), &PyStaticMethod_Type);
    }

    inline bool IsClassMethod(Object const &obj)
    {
        return PyObject_TypeCheck(obj.ptr(), &PyClassMethod_Type);
    }

private:
    void _WalkModule(Object const &obj, WalkCallbackFn callback,
                     TfHashSet<PyObject *, TfHash> *visitedObjs)
    {
        if (PyObject_HasAttrString(obj.ptr(), "__dict__")) {
            // In python 3 dict.items() returns a proxy view object, not a list.
            Object itemsView = obj.attr("__dict__").attr("items")();
            pxr_boost::python::list items(itemsView);
            size_t lenItems = pxr_boost::python::len(items);
            for (size_t i = 0; i < lenItems; ++i) {
                Object value = items[i][1];
                if (!visitedObjs->count(value.ptr())) {
                    const std::string name =
                        PyUnicode_AsUTF8(Object(items[i][0]).ptr());
                    bool keepGoing = (this->*callback)(name.c_str(), obj, value);
                    visitedObjs->insert(value.ptr());
                    if (IsBoostPythonClass(value) && keepGoing) {
                        _WalkModule(value, callback, visitedObjs);
                    }
                }
            }
        }
    }

public:
    void WalkModule(Object const &obj, WalkCallbackFn callback)
    {
        TfHashSet<PyObject *, TfHash> visited;
        _WalkModule(obj, callback, &visited);
    }

    class _InvokeWithErrorHandling
    {
    public:
        _InvokeWithErrorHandling(Object const &fn,
                                 std::string const &funcName,
                                 std::string const &fileName)
            : _fn(fn)
            , _funcName(funcName)
            , _fileName(fileName)
        {}

        PyObject *operator()(PyObject *args, PyObject *kw) const {

            // Fabricate a python tracing event to record the python -> c++ ->
            // python transition.
            TfPyTraceInfo info;
            info.arg = NULL;
            info.funcName = _funcName.c_str();
            info.fileName = _fileName.c_str();
            info.funcLine = 0;

            info.what = PyTrace_CALL;
            Tf_PyFabricateTraceEvent(info);

            TfErrorMark m;
            PyObject *ret = PyObject_Call(_fn.ptr(), args, kw);

            info.what = PyTrace_RETURN;
            Tf_PyFabricateTraceEvent(info);

            if (ARCH_UNLIKELY(!ret)) {
                TF_VERIFY(PyErr_Occurred());
                pxr_boost::python::throw_error_already_set();
            }

            if (ARCH_UNLIKELY(!m.IsClean() &&
                              TfPyConvertTfErrorsToPythonException(m))) {
                Py_DECREF(ret);
                pxr_boost::python::throw_error_already_set();
            }

            return ret;
        }

    private:
        Object _fn;
        std::string _funcName;
        std::string _fileName;
    };

    Object DecorateForErrorHandling(
        const char *name, Object const &owner, Object const &fn)
    {
        Object ret = fn;
        if (ARCH_LIKELY(fn.ptr() != Py_None)) {
            std::string *fullNamePrefix = &_newModuleName;
            std::string localPrefix;
            if (PyObject_HasAttrString(owner.ptr(), "__module__")) {
                char const *ownerName =
                    PyUnicode_AsUTF8(PyObject_GetAttrString
                                       (owner.ptr(), "__name__"));
                localPrefix.append(_newModuleName);
                localPrefix.push_back('.');
                localPrefix.append(ownerName);
                fullNamePrefix = &localPrefix;
            }

            ret = pxr_boost::python::detail::make_raw_function(
                pxr_boost::python::objects::py_function(
                    _InvokeWithErrorHandling(
                        fn, *fullNamePrefix + "." + name, *fullNamePrefix),
                    pxr_boost::python::type_list<PyObject *>(),
                    /*min_args =*/ 0,
                    /*max_args =*/ ~0
                    )
                );

            ret.attr("__doc__") = fn.attr("__doc__");
        }

        return ret;
    }

    inline Object ReplaceFunctionOnOwner(char const *name,
                                         Object owner,
                                         Object fn)
    {
        Object newFn = DecorateForErrorHandling(name, owner, fn);
        PyObject_DelAttrString(owner.ptr(), name);
        pxr_boost::python::objects::function::add_to_namespace(
            owner, name, newFn);
        return newFn;
    }

    bool WrapForErrorHandlingCB(
        char const *name, Object const &owner, Object const &obj)
    {
        if (!strcmp(name, "RepostErrors") ||
            !strcmp(name, "ReportActiveMarks")) {
            return false;
        } else if (IsBoostPythonFunc(obj)) {
            ReplaceFunctionOnOwner(name, owner, obj);
            return false;
        } else if (IsProperty(obj)) {
            // XXX: In Python 3.9+ this is equivalent to
            // if (!Py_IS_TYPE(obj.ptr(), &PyProperty_Type)) {
            if (Py_TYPE(obj.ptr()) != &PyProperty_Type) {
                // Static properties are not wrapped with error handling.
            } else {
                Object propType(
                    pxr_boost::python::handle<>(
                        pxr_boost::python::borrowed(&PyProperty_Type)));
                Object newfget =
                    DecorateForErrorHandling(name, owner, obj.attr("fget"));
                Object newfset =
                    DecorateForErrorHandling(name, owner, obj.attr("fset"));
                Object newfdel =
                    DecorateForErrorHandling(name, owner, obj.attr("fdel"));
                Object newProp =
                    propType(newfget, newfset, newfdel,
                             Object(obj.attr("__doc__")));
                pxr_boost::python::setattr(owner, name, newProp);
            }
            return false;
        } else if (IsStaticMethod(obj)) {
            Object underlyingFn = obj.attr("__get__")(owner);
            if (IsBoostPythonFunc(underlyingFn)) {
                Object newFn =
                    ReplaceFunctionOnOwner(name, owner, underlyingFn);
                pxr_boost::python::setattr(owner, name,
                    Object(pxr_boost::python::handle<>(
                        PyStaticMethod_New(newFn.ptr()))));
            }
            return false;
        } else if (IsClassMethod(obj)) {
            Object underlyingFn =
                obj.attr("__get__")(owner).attr("__func__");
            if (IsBoostPythonFunc(underlyingFn)) {
                Object newFn =
                    ReplaceFunctionOnOwner(name, owner, underlyingFn);
                pxr_boost::python::setattr(owner, name,
                    Object(pxr_boost::python::handle<>(
                        PyClassMethod_New(newFn.ptr()))));
            }
            return false;
        }

        return true;
    }

    void WrapForErrorHandling() {
        WalkModule(_module, &This::WrapForErrorHandlingCB);
    }

    bool FixModuleAttrsCB(
        char const *name, Object const& owner, Object const &obj)
    {
        if (PyObject_HasAttrString(obj.ptr(), "__module__")) {
            PyObject_SetAttrString(obj.ptr(), "__module__",
                                   _newModuleNameObj.ptr());
            if (PyErr_Occurred()) {
                // Boost python functions still screw up here.
                PyErr_Clear();
            }
        }
        return true;
    }

    void FixModuleAttrs() {
        WalkModule(_module, &This::FixModuleAttrsCB);
    }

    Tf_BoostModuleProcessor(Object const &module)
        : _module(module)
        , _cachedBPFuncType(0)
        , _cachedBPClassType(0)
    {
        auto obj = Object(module.attr("__name__"));
        _oldModuleName =
            PyUnicode_AsUTF8(obj.ptr());
        _newModuleName = TfStringGetBeforeSuffix(_oldModuleName);
        _newModuleNameObj = Object(_newModuleName);
    }

private:

    std::string _oldModuleName, _newModuleName;
    Object _newModuleNameObj;

    Object _module;

    PyObject *_cachedBPFuncType;
    PyObject *_cachedBPClassType;
};

inline void Tf_PyPostProcessBoostModule()
{
    pxr_boost::python::scope module;
    try {
        Tf_BoostModuleProcessor mp(module);
        mp.FixModuleAttrs();
        mp.WrapForErrorHandling();
        if (PyErr_Occurred())
            pxr_boost::python::throw_error_already_set();
    } catch (pxr_boost::python::error_already_set const &) {
        std::string name =
            pxr_boost::python::extract<std::string>(module.attr("__name__"));
        TF_WARN("Error occurred postprocessing module %s!", name.c_str());
        TfPyPrintError();
    }
}

inline void Tf_PyInitWrapModule(
    void (*wrapModule)(),
    const char* packageModule,
    const char* packageName,
    const char* packageTag,
    const char* packageTag2)
{
    // Starting with Python 3.7, the GIL is initialized as part of
    // Py_Initialize(). Python 3.9 deprecated explicit GIL initialization.
#if PY_VERSION_HEX < 0x03070000
    // Ensure the python GIL is created.
    PyEval_InitThreads();
#endif

    Tf_PyTracingPythonInitialized();

    TfScriptModuleLoader::GetInstance().
        LoadModulesForLibrary(TfToken(packageName));
    if (PyErr_Occurred()) {
        pxr_boost::python::throw_error_already_set();
    }

    TfAutoMallocTag tag(packageTag2, "WrapModule", packageTag);

    Tf_PyPushWrapContext(packageModule);

    // Provide a way to find the full mfb name of the package. Can't use the
    // TfToken, because when we get here in loading Tf, TfToken has not yet been
    // wrapped.
    pxr_boost::python::scope().attr("__MFB_FULL_PACKAGE_NAME") = packageName;

    pxr_boost::python::docstring_options docOpts(
        true /*show user-defined*/, false /*show signatures*/);

    wrapModule();
    Tf_PyPostProcessBoostModule();

    Tf_PyPopWrapContext();

    TfPyModuleWasLoaded(packageName).Send();
}

ARCH_EXPORT
void TF_PP_CAT(init_module_, MFB_PACKAGE_NAME)() {

    Tf_PyInitWrapModule(
        WrapModule,
        TF_PP_STRINGIZE(MFB_PACKAGE_MODULE),
        TF_PP_STRINGIZE(MFB_ALT_PACKAGE_NAME),
        "Wrap " TF_PP_STRINGIZE(MFB_ALT_PACKAGE_NAME),
        TF_PP_STRINGIZE(MFB_PACKAGE_NAME)
        );
}

PXR_NAMESPACE_CLOSE_SCOPE

// When we generate boost python bindings for a library named Foo,
// we generate a python package that has __init__.py and _Foo.so,
// and we put all the python bindings in _Foo.so. The __init__.py
// file imports _Foo and then publishes _Foo's symbols as its own.
// Since the module with the bindings is named _Foo, the PyInit routine
// must be named PyInit_Foo. This little block produces that function.
//
// See https://docs.python.org/3/c-api/module.html#initializing-c-modules_
//
extern "C"
ARCH_EXPORT
PyObject* TF_PP_CAT(PyInit__, MFB_PACKAGE_NAME)() {

    static struct PyModuleDef moduledef = {
        PyModuleDef_HEAD_INIT,
        TF_PP_STRINGIZE(TF_PP_CAT(_, MFB_PACKAGE_NAME)),    // m_name
        0,                                                  // m_doc
        -1,                                                 // m_size
        NULL,                                               // m_methods
        0,                                                  // m_reload
        0,                                                  // m_traverse
        0,                                                  // m_clear
        0,                                                  // m_free
    };

    PXR_NAMESPACE_USING_DIRECTIVE
    return pxr_boost::python::detail::init_module(moduledef,
                TF_PP_CAT(init_module_, MFB_PACKAGE_NAME));
}

// We also support the case where both the library contents and the
// python bindings go into libfoo.so. We still generate a package named foo
// but the __init__.py file in the package foo imports libfoo and
// publishes it's symbols as its own. Since the module with the
// bindings is named libfoo, the init routine must be named PyInit_libfoo.
// This little block produces that function.
//
// So there are two init routines in every library, but only the one
// that matches the name of the python module will be called by python
// when the module is imported. So the total cost is a 1-line
// function that doesn't get called.
//
extern "C"
ARCH_EXPORT
PyObject* TF_PP_CAT(PyInit_lib, MFB_PACKAGE_NAME)() {

    static struct PyModuleDef moduledef = {
        PyModuleDef_HEAD_INIT,
        TF_PP_STRINGIZE(TF_PP_CAT(lib, MFB_PACKAGE_NAME)),    // m_name
        0,                                                    // m_doc
        -1,                                                   // m_size
        NULL,                                                 // m_methods
        0,                                                    // m_reload
        0,                                                    // m_traverse
        0,                                                    // m_clear
        0,                                                    // m_free
    };

    PXR_NAMESPACE_USING_DIRECTIVE
    return pxr_boost::python::detail::init_module(moduledef,
                TF_PP_CAT(init_module_, MFB_PACKAGE_NAME));
}

#define TF_WRAP_MODULE static void WrapModule()

// Declares and calls the class wrapper for x
#define TF_WRAP(x) ARCH_HIDDEN void wrap ## x (); wrap ## x ()
