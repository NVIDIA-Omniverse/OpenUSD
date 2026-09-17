//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_BASE_TF_PY_NOTICE_WRAPPER_BOOST_H
#define PXR_BASE_TF_PY_NOTICE_WRAPPER_BOOST_H

/// \file tf/pyNoticeWrapperBoost.h
/// Boost.Python adapters for notice wrapping.

#include "pxr/pxr.h"

#include "pxr/base/tf/pyNoticeWrapper.h"
#include "pxr/base/tf/wrapTypeHelpers.h"

#include "pxr/external/boost/python/bases.hpp"
#include "pxr/external/boost/python/class.hpp"
#include "pxr/external/boost/python/object.hpp"

PXR_NAMESPACE_OPEN_SCOPE

template <typename NoticeType, typename BaseType>
struct TfPyNoticeWrapper : public NoticeType, public TfPyNoticeWrapperBase {
private:
    static_assert(std::is_base_of<TfNotice, NoticeType>::value
                  || std::is_same<TfNotice, NoticeType>::value,
                  "Notice type must be derived from or equal to TfNotice.");

    static_assert(std::is_base_of<TfNotice, BaseType>::value
                  || std::is_same<TfNotice, BaseType>::value,
                  "BaseType type must be derived from or equal to TfNotice.");

    static_assert(std::is_base_of<BaseType, NoticeType>::value
                  || (std::is_same<NoticeType, TfNotice>::value
                      && std::is_same<BaseType, TfNotice>::value),
                  "BaseType type must be a base of notice, unless both "
                  "BaseType and Notice type are equal to TfNotice.");

public:

    typedef TfPyNoticeWrapper<NoticeType, BaseType> This;

    // If Notice is really TfNotice, then this is the root of the hierarchy and
    // bases is empty, otherwise bases contains the base class.
    using Bases = std::conditional_t<std::is_same<NoticeType, TfNotice>::value,
                                     pxr_boost::python::bases<>,
                                     pxr_boost::python::bases<BaseType>>;

    typedef pxr_boost::python::class_<NoticeType, This, Bases> ClassType;

    static ClassType Wrap(std::string const &name = std::string()) {
        std::string wrappedName = name;
        if (wrappedName.empty()) {
            // Assume they want the last bit of a qualified name.
            wrappedName = TfType::Find<NoticeType>().GetTypeName();
            if (!TfStringGetSuffix(wrappedName, ':').empty())
                wrappedName = TfStringGetSuffix(wrappedName, ':'); 
        }
        Tf_PyNoticeObjectGenerator::Register<NoticeType>(&This::_Generate);
        Tf_RegisterPythonObjectFinderInternal
            (typeid(TfPyNoticeWrapper),
             new Tf_PyNoticeObjectFinder<TfPyNoticeWrapper>);
        return ClassType(wrappedName.c_str(), pxr_boost::python::no_init)
            .def(TfTypePythonClass());
    }

    // Implement the base class's virtual method.
    virtual PyObject *GetNoticePythonObjectNewRef() const {
        TfPyLock lock;
        Py_INCREF(_self);
        return _self;
    }

    // Arbitrary argument constructor (with a leading PyObject *) which
    // forwards to the base Notice class's constructor.
    template <typename... Args>
    TfPyNoticeWrapper(PyObject *self, Args... args)
        : NoticeType(args...)
        , _self(self) {}
    
private:
    static PyObject *_Generate(TfNotice const &n) {
        // Python locking is left to the caller.
        return pxr_boost::python::incref(
            pxr_boost::python::object(static_cast<NoticeType const &>(n)).ptr());
    }

    PyObject *_self;

};

#define TF_INSTANTIATE_NOTICE_WRAPPER(T, Base) \
TF_REGISTRY_FUNCTION(TfType) \
{ \
    TfType::Define< TfPyNoticeWrapper<T, Base>, \
                    TfType::Bases<Base> >(); \
}

PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_BASE_TF_PY_NOTICE_WRAPPER_BOOST_H
