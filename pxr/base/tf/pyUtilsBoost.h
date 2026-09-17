//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_BASE_TF_PY_UTILS_BOOST_H
#define PXR_BASE_TF_PY_UTILS_BOOST_H

/// \file tf/pyUtilsBoost.h
/// Boost.Python utilities for dealing with script.

#include "pxr/pxr.h"

#include "pxr/base/tf/pyUtils.h"

#include "pxr/base/tf/diagnosticLite.h"
#include "pxr/base/tf/errorMark.h"
#include "pxr/base/tf/pyError.h"
#include "pxr/base/tf/pyInterpreterBoost.h"
#include "pxr/base/tf/pyLock.h"
#include "pxr/base/tf/scriptModuleLoaderBoost.h"

#include "pxr/external/boost/python/dict.hpp"
#include "pxr/external/boost/python/errors.hpp"
#include "pxr/external/boost/python/extract.hpp"
#include "pxr/external/boost/python/handle.hpp"
#include "pxr/external/boost/python/list.hpp"
#include "pxr/external/boost/python/object.hpp"
#include "pxr/external/boost/python/object/class_detail.hpp"
#include "pxr/external/boost/python/tuple.hpp"
#include "pxr/external/boost/python/type_id.hpp"

#include <functional>
#include <mutex>
#include <typeinfo>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

/// Return true iff \a obj is None.
inline bool TfPyIsNone(pxr_boost::python::object const &obj)
{
    return TfPyIsNone(obj.ptr());
}

/// Return true iff \a obj is None.
inline bool TfPyIsNone(pxr_boost::python::handle<> const &obj)
{
    return TfPyIsNone(obj.get());
}

/// Return a python object for the given C++ object, loading the appropriate
/// wrapper code if necessary. Spams users if complainOnFailure is true and
/// conversion fails.
template <typename T>
pxr_boost::python::object TfPyObject(
    T const &t, bool complainOnFailure = true) {
    // initialize python if it isn't already, so at least we can try to return
    // an object
    if (!TfPyIsInitialized()) {
        TF_CODING_ERROR("Called TfPyObject without python being initialized!");
        TfPyInitialize();
    }

    TfPyLock pyLock;

    // Will only be able to return objects which have been wrapped.
    // Returns None otherwise
    try {
        return pxr_boost::python::object(t);
    } catch (pxr_boost::python::error_already_set const &) {
        Tf_PyObjectError(complainOnFailure);
        return pxr_boost::python::object();
   }
}

inline
pxr_boost::python::object TfPyObject(
    PyObject* t, bool complainOnFailure = true) {
    TfPyLock pyLock;
    return pxr_boost::python::object(pxr_boost::python::handle<>(t));
}

/// Return repr(t).
///
/// Calls PyObject_Repr on the given python object.
inline std::string TfPyObjectRepr(pxr_boost::python::object const &t)
{
    return TfPyObjectRepr(t.ptr());
}

/// Return repr(t).
///
/// Converts t to its equivalent python object and then calls PyObject_Repr on
/// that.
template <typename T>
std::string TfPyRepr(T const &t) {
    if (!TfPyIsInitialized())
        return "<python not initialized>";
    TfPyLock lock;
    return TfPyObjectRepr(TfPyObject(t));
}

/// Return repr(t) for a vector as a python list.
template <typename T>
std::string TfPyRepr(const std::vector<T> &v) {
    std::string result("[");
    typename std::vector<T>::const_iterator i = v.begin();
    if (i != v.end()) {
        result += TfPyRepr(*i);
        ++i;
    }
    while (i != v.end()) {
        result += ", " + TfPyRepr(*i);
        ++i;
    }
    result += "]";
    return result;
}

/// Evaluate python expression \a expr with all the known script modules
/// imported under their standard names. Additional globals may be provided in
/// the \p extraGlobals dictionary.
inline
pxr_boost::python::object
TfPyEvaluate(
    std::string const &expr,
    pxr_boost::python::dict const &extraGlobals = pxr_boost::python::dict())
{
    TfPyLock lock;
    try {
        pxr_boost::python::dict modulesDict =
            TfScriptModuleLoader_GetModulesDict(
                TfScriptModuleLoader::GetInstance());

        pxr_boost::python::handle<> modHandle(
            PyImport_ImportModule("builtins"));
        modulesDict["__builtins__"] = pxr_boost::python::object(modHandle);
        modulesDict.update(extraGlobals);

        return pxr_boost::python::object(TfPyRunString(
            expr, Py_eval_input, modulesDict, modulesDict));
    } catch (pxr_boost::python::error_already_set const &) {
        TfPyConvertPythonExceptionToTfErrors();
        PyErr_Clear();
    }
    return pxr_boost::python::object();
}

/// Return the name of the class of \a obj.
inline std::string TfPyGetClassName(pxr_boost::python::object const &obj)
{
    return TfPyGetClassName(obj.ptr());
}

/// Return the python class object for \a type if \a type has been wrapped.
/// Otherwise return None.
inline pxr_boost::python::object
TfPyGetClassObject(std::type_info const &type)
{
    TfPyLock pyLock;
    return pxr_boost::python::object(
        pxr_boost::python::objects::registered_class_object(type));
}

/// Return the python class object for T if T has been wrapped.
/// Otherwise return None.
template <typename T>
pxr_boost::python::object
TfPyGetClassObject() {
    return TfPyGetClassObject(typeid(T));
}

inline void
Tf_PyWrapOnceImpl(pxr_boost::python::type_info const &,
                  std::function<void()> const&,
                  bool *);

inline void
Tf_PyWrapOnceImpl(
    pxr_boost::python::type_info const &type,
    std::function<void()> const &wrapFunc,
    bool *isTypeWrapped)
{
    static std::mutex pyWrapOnceMutex;

    if (!wrapFunc) {
        TF_CODING_ERROR("Got null wrapFunc");
        return;
    }

    TfPyLock pyLock;
    pyLock.BeginAllowThreads();
    std::lock_guard<std::mutex> lock(pyWrapOnceMutex);
    pyLock.EndAllowThreads();

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

/// Invokes \p wrapFunc to wrap type \c T if \c T is not already wrapped.
///
/// Executing \p wrapFunc *must* register \c T with boost python.  Otherwise,
/// \p wrapFunc may be executed more than once.
///
/// TfPyWrapOnce will acquire the GIL prior to invoking \p wrapFunc. Does not
/// invoke \p wrapFunc if Python has not been initialized.
template <typename T>
void
TfPyWrapOnce(std::function<void()> const &wrapFunc)
{
    // Don't try to wrap if python isn't initialized.
    if (!TfPyIsInitialized()) {
        return;
    }

    static bool isTypeWrapped = false;
    if (isTypeWrapped) {
        return;
    }

    Tf_PyWrapOnceImpl(pxr_boost::python::type_id<T>(), wrapFunc, &isTypeWrapped);
}

/// Creates a python dictionary from a std::map.
template <class Map>
pxr_boost::python::dict TfPyCopyMapToDictionary(Map const &map) {
    TfPyLock lock;
    pxr_boost::python::dict d;
    for (typename Map::const_iterator i = map.begin(); i != map.end(); ++i)
        d[i->first] = i->second;
    return d;
}

template<class Seq>
pxr_boost::python::list TfPyCopySequenceToList(Seq const &seq) {
    TfPyLock lock;
    pxr_boost::python::list l;
    for (typename Seq::const_iterator i = seq.begin();
         i != seq.end(); ++i)
        l.append(*i);
    return l;
}

/// Create a python set from an iterable sequence.
///
/// If Seq::value_type is not hashable, TypeError is raised via throwing
/// pxr_boost::python::error_already_set.
template <class Seq>
pxr_boost::python::object TfPyCopySequenceToSet(Seq const &seq) {
    TfPyLock lock;
    pxr_boost::python::handle<> set{
        pxr_boost::python::allow_null(PySet_New(nullptr))};
    if (!set) {
        pxr_boost::python::throw_error_already_set();
    }
    for (auto const& item : seq) {
        pxr_boost::python::object obj(item);
        if (PySet_Add(set.get(), obj.ptr()) == -1) {
            pxr_boost::python::throw_error_already_set();
        }
    }
    return pxr_boost::python::object(set);
}

template<class Seq>
pxr_boost::python::tuple TfPyCopySequenceToTuple(Seq const &seq) {
    return pxr_boost::python::tuple(TfPyCopySequenceToList(seq));
}

/// Create a python bytearray from an input buffer and size.
///
/// If a size of zero is passed in this function will return a valid python
/// bytearray of size zero.
///
/// An invalid object handle is returned on failure.
inline pxr_boost::python::object TfPyCopyBufferToByteArray(
    const char* buffer, size_t size)
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

// Private helper method to TfPyEvaluateAndExtract.
//
inline bool Tf_PyEvaluateWithErrorCheck(
    const std::string & expr, pxr_boost::python::object * obj)
{
    TfErrorMark m;
    *obj = TfPyEvaluate(expr);
    return m.IsClean();
}

/// Safely evaluates \p expr and extracts the return object of type T. If
/// successful, returns \c true and sets *t to the return value, otherwise
/// returns \c false.
template <typename T>
bool TfPyEvaluateAndExtract(const std::string & expr, T * t)
{
    if (expr.empty())
        return false;

    // Take the lock before doing anything with pxr_boost::python.
    TfPyLock lock;

    // Though TfPyEvaluate (called by Tf_PyEvaluateWithErroCheck) takes the
    // python lock, it is important that we lock before we initialize the
    // pxr_boost::python::object, since it will increment and decrement ref
    // counts outside of the call to TfPyEvaluate.
    pxr_boost::python::object obj;
    if (!Tf_PyEvaluateWithErrorCheck(expr, &obj))
        return false;

    pxr_boost::python::extract<T> extractor(obj);

    if (!extractor.check())
        return false;

    *t = extractor();

    return true;
}

PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_BASE_TF_PY_UTILS_BOOST_H
