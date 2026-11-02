//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/base/tf/boost/pyErrorBoost.h"
#include "pxr/base/tf/boost/pyResultConversions.h"
#include "pxr/base/tf/boost/pyPtrHelpersBoost.h"
#include "pxr/base/tf/anyWeakPtr.h"
#include "pxr/base/tf/diagnostic.h"
#include "pxr/base/tf/type.h"
#include "pxr/base/tf/weakBase.h"
#include "pxr/external/boost/python/def.hpp"
#include "pxr/external/boost/python/module.hpp"
#include "pxr/external/boost/python/return_value_policy.hpp"

#include <vector>
#include <memory>

PXR_NAMESPACE_USING_DIRECTIVE

static std::vector<int> Values() { return {1, 2, 3}; }

static int RaiseError()
{
    TF_RUNTIME_ERROR("expected binding-helper error");
    return 0;
}

namespace {
struct ContractObject : TfWeakBase {};
std::unique_ptr<ContractObject> owner;
TfAnyWeakPtr stored;

TfAnyWeakPtr NewAnyWeak(bool constFirst)
{
    owner = std::make_unique<ContractObject>();
    stored = constFirst ? TfAnyWeakPtr(TfWeakPtr<const ContractObject>(owner.get()))
                        : TfAnyWeakPtr(TfWeakPtr<ContractObject>(owner.get()));
    return stored;
}
TfAnyWeakPtr GetAnyWeak() { return TfWeakPtr<ContractObject>(owner.get()); }
TfAnyWeakPtr GetConstAnyWeak() { return TfWeakPtr<const ContractObject>(owner.get()); }
TfAnyWeakPtr GetStoredAnyWeak() { return stored; }
void DropOwner()
{
    // This test owner explicitly releases its strong Python identity when it
    // retires. Expired weak handles can still keep the identity remnant alive.
    if (owner) {
        Tf_PyIdentityHelper::Release(stored.GetUniqueIdentifier());
        owner.reset();
    }
}
}

PXR_BOOST_PYTHON_MODULE(tfBindingConsumer)
{
    using namespace pxr_boost::python;
    def("values", &Values, return_value_policy<PXR_NS::TfPySequenceToList>());
    def("raise_error", &RaiseError, PXR_NS::TfPyRaiseOnError<>());
    TfType::Define<ContractObject>();
    class_<ContractObject, TfWeakPtr<ContractObject>, noncopyable>("ContractObject", no_init)
        .def(TfPyWeakPtr());
    def("new_any_weak", &NewAnyWeak);
    def("get_any_weak", &GetAnyWeak);
    def("get_const_any_weak", &GetConstAnyWeak);
    def("get_stored_any_weak", &GetStoredAnyWeak);
    def("drop_owner", &DropOwner);
}
