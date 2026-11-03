// Copyright 2026 Pixar
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
#include "pxr/usd/sdf/boost/pyUtils.h"
#include "pxr/external/boost/python.hpp"
PXR_NAMESPACE_USING_DIRECTIVE
static pxr_boost::python::tuple Convert(const pxr_boost::python::dict& input, bool withError)
{
    SdfLayer::FileFormatArguments arguments{{"original", "unchanged"}};
    std::string message = "preset";
    const bool success = SdfFileFormatArgumentsFromPython(input, &arguments,
        withError ? &message : nullptr);
    pxr_boost::python::dict output;
    for (const auto& entry : arguments) output[entry.first] = entry.second;
    return pxr_boost::python::make_tuple(success, output, message);
}
PXR_BOOST_PYTHON_MODULE(sdfBindingHelpers)
{
    pxr_boost::python::def("convert", Convert);
}
