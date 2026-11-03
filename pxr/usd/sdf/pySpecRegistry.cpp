//
// Copyright 2016 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
/// \file pySpecRegistry.cpp

#include "pxr/pxr.h"
#include "pxr/usd/sdf/pySpecRegistry.h"
#include "pxr/usd/sdf/spec.h"
#include "pxr/usd/sdf/specType.h"
#include "pxr/base/arch/demangle.h"
#include "pxr/base/tf/diagnostic.h"
#include "pxr/base/tf/pyLock.h"
#include "pxr/base/tf/pyExceptionState.h"
#include "pxr/base/tf/staticData.h"
#include "pxr/base/tf/type.h"
#include <map>
#include <mutex>

PXR_NAMESPACE_OPEN_SCOPE

namespace Sdf_PySpecDetail {

namespace {
struct _HolderCreators {
    std::mutex mutex;
    std::map<TfType, _HolderCreator> creators;
};
// One compiled registry shared by _sdf and downstream binding modules, rather
// than per-module state instantiated from the binding adapter header.
TfStaticData<_HolderCreators> _holderCreators;

PyObject* _None()
{
    Py_INCREF(Py_None);
    return Py_None;
}
}

void
_RegisterHolderCreator(const std::type_info& ti, _HolderCreator creator)
{
    TfPyLock gil;
    if (!creator) {
        TF_CODING_ERROR("Cannot register a null spec holder creator");
        return;
    }
    TfType type = TfType::Find(ti);
    if (type.IsUnknown()) {
        TF_CODING_ERROR("No TfType registered for type \"%s\"",
                        ArchGetDemangled(ti).c_str());
    }
    else {
        bool inserted;
        {
            std::lock_guard<std::mutex> lock(_holderCreators->mutex);
            inserted = _holderCreators->creators.emplace(type, creator).second;
        }
        if (!inserted) {
            TF_CODING_ERROR("Duplicate conversion for \"%s\" ignored",
                            type.GetTypeName().c_str());
        }
    }
}

PyObject*
_CreateHolder(const std::type_info& ti, const SdfSpec& spec)
{
    TfPyLock gil;
    if (PyErr_Occurred()) return nullptr;
    if (spec.IsDormant()) {
        return _None();
    }
    else {
        // Get the TfType for the object's actual type.  If there's an
        // ambiguity (e.g. for SdfVariantSpec) then use type ti.
        TfType type = Sdf_SpecType::Cast(spec, ti);

        // Find the creator for the type and invoke it.
        _HolderCreator creator = nullptr;
        {
            std::lock_guard<std::mutex> lock(_holderCreators->mutex);
            const auto found = _holderCreators->creators.find(type);
            if (found != _holderCreators->creators.end()) creator = found->second;
        }
        if (!creator) {
            if (!type.IsUnknown()) {
                TF_CODING_ERROR("No conversion for registed for \"%s\"",
                                type.GetTypeName().c_str());
            }
            return _None();
        }
        else {
            PyObject* result = creator(spec);
            if (PyErr_Occurred()) {
                // A creator returning an object with an error pending must
                // not let that object's destructor replace the original error.
                auto error = TfPyExceptionState::Fetch();
                Py_XDECREF(result);
                error.Restore();
                return nullptr;
            }
            if (!result) {
                PyErr_SetString(PyExc_RuntimeError,
                    "Spec holder creator returned null without a Python exception");
            }
            return result;
        }
    }
}

}

PXR_NAMESPACE_CLOSE_SCOPE
