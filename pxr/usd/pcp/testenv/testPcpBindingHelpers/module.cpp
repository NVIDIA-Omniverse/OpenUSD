// Copyright 2026 Pixar
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
#include "pxr/usd/pcp/boost/pyUtils.h"
#include "pxr/base/tf/errorMark.h"
#include "pxr/external/boost/python.hpp"

PXR_NAMESPACE_USING_DIRECTIVE
namespace bp = pxr_boost::python;

static bp::tuple Convert(const bp::dict& input)
{
    PcpVariantFallbackMap result{{"original", {"unchanged"}}};
    TfErrorMark mark;
    bool success;
    try {
        success = PcpVariantFallbackMapFromPython(input, &result);
    } catch (...) {
        mark.Clear();
        throw;
    }
    bp::list errors;
    for (auto const& error : mark) errors.append(error.GetCommentary());
    mark.Clear();
    bp::dict output;
    for (auto const& entry : result) {
        bp::list values;
        for (auto const& value : entry.second) values.append(value);
        output[entry.first] = values;
    }
    return bp::make_tuple(success, output, errors);
}

PXR_BOOST_PYTHON_MODULE(pcpBindingHelpers)
{
    bp::def("convert", Convert);
}
