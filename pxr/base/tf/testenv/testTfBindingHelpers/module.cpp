//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/base/tf/boost/pyErrorBoost.h"
#include "pxr/base/tf/boost/pyResultConversions.h"
#include "pxr/base/tf/diagnostic.h"
#include "pxr/external/boost/python/def.hpp"
#include "pxr/external/boost/python/module.hpp"
#include "pxr/external/boost/python/return_value_policy.hpp"

#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

static std::vector<int> Values() { return {1, 2, 3}; }

static int RaiseError()
{
    TF_RUNTIME_ERROR("expected binding-helper error");
    return 0;
}

PXR_BOOST_PYTHON_MODULE(tfBindingConsumer)
{
    using namespace pxr_boost::python;
    def("values", &Values, return_value_policy<PXR_NS::TfPySequenceToList>());
    def("raise_error", &RaiseError, PXR_NS::TfPyRaiseOnError<>());
}
