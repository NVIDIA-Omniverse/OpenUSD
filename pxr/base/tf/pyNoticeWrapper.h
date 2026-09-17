//
// Copyright 2016 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_BASE_TF_PY_NOTICE_WRAPPER_H
#define PXR_BASE_TF_PY_NOTICE_WRAPPER_H

#include "pxr/pxr.h"
#include "pxr/base/tf/notice.h"
#include "pxr/base/tf/stringUtils.h"
#include "pxr/base/tf/staticData.h"
#include "pxr/base/tf/type.h"
#include "pxr/base/tf/pyLock.h"
#include "pxr/base/tf/pyObjectFinder.h"

#include <type_traits>
#include <map>
#include <string>

PXR_NAMESPACE_OPEN_SCOPE

struct Tf_PyNoticeObjectGenerator {
    typedef Tf_PyNoticeObjectGenerator This;
    typedef PyObject *(*MakeObjectFunc)(TfNotice const &);

    // Register the generator for notice type T.
    template <typename T>
    static void Register(MakeObjectFunc func) {
        // XXX this stuff should be keyed directly off TfType now
        (*_generators)[typeid(T).name()] = func;
    }
    
    // Produce a new reference to a Python object for the correct derived type
    // of \a n.
    TF_API static PyObject *Invoke(TfNotice const &n);

private:

    static MakeObjectFunc _Lookup(TfNotice const &n);

    TF_API static TfStaticData<std::map<std::string, MakeObjectFunc> > _generators;

};

struct TfPyNoticeWrapperBase : public TfType::PyPolymorphicBase {
    TF_API virtual ~TfPyNoticeWrapperBase();
    // Return a new reference to the Python object implementing this notice.
    virtual PyObject *GetNoticePythonObjectNewRef() const = 0;
};

template <class Notice>
struct Tf_PyNoticeObjectFinder : public Tf_PyObjectFinderBase {
    virtual ~Tf_PyNoticeObjectFinder() {}
    virtual PyObject *Find(void const *objPtr) const {
        TfPyLock lock;
        Notice const *wrapper = static_cast<Notice const *>(objPtr);
        return wrapper ? wrapper->GetNoticePythonObjectNewRef() : nullptr;
    }
};

PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_BASE_TF_PY_NOTICE_WRAPPER_H
