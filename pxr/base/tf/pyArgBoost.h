//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_BASE_TF_PY_ARG_BOOST_H
#define PXR_BASE_TF_PY_ARG_BOOST_H

/// \file tf/pyArgBoost.h
/// Boost.Python adapters for TfPyArg utilities.

#include "pxr/pxr.h"

#include "pxr/base/tf/pyArg.h"
#include "pxr/base/tf/pyUtils.h"
#include "pxr/base/tf/stringUtils.h"

#include "pxr/external/boost/python/dict.hpp"
#include "pxr/external/boost/python/extract.hpp"
#include "pxr/external/boost/python/list.hpp"
#include "pxr/external/boost/python/slice.hpp"
#include "pxr/external/boost/python/stl_iterator.hpp"
#include "pxr/external/boost/python/tuple.hpp"

#include <algorithm>
#include <string>
#include <utility>

PXR_NAMESPACE_OPEN_SCOPE

/// Helper function for processing optional arguments given as a tuple of
/// positional arguments and a dictionary of keyword arguments.
///
/// This function will match the given positional arguments in \p args with
/// the ordered list of allowed arguments in \p optionalArgs. Arguments that
/// are matched up in this way will be stored as (name, value) pairs and
/// merged with \p kwargs in the returned dictionary.
///
/// If \p allowExtraArgs is \c false, any unrecognized keyword or positional
/// arguments will cause a Python TypeError to be emitted. Otherwise,
/// unmatched arguments will be added to the returned tuple or dict.
inline std::pair<pxr_boost::python::tuple, pxr_boost::python::dict>
TfPyProcessOptionalArgs(
    const pxr_boost::python::tuple& args,
    const pxr_boost::python::dict& kwargs,
    const TfPyArgs& expectedArgs,
    bool allowExtraArgs = false)
{
    using namespace pxr_boost::python;

    std::pair<tuple, dict> rval;

    const unsigned int numArgs = static_cast<unsigned int>(len(args));
    const unsigned int numExpectedArgs =
        static_cast<unsigned int>(expectedArgs.size());

    if (!allowExtraArgs) {
        if (numArgs > numExpectedArgs) {
            TfPyThrowTypeError("Too many arguments for function");
        }

        const list keys = kwargs.keys();

        typedef stl_input_iterator<std::string> KeyIterator;
        for (KeyIterator it(keys), it_end; it != it_end; ++it) {
            if (std::find_if(
                    expectedArgs.begin(), expectedArgs.end(),
                    [&it](const TfPyArg &arg) {
                        return arg.GetName() == *it;
                    }) == expectedArgs.end()) {

                TfPyThrowTypeError("Unexpected keyword argument '%s'");
            }
        }
    }

    rval.second = kwargs;

    for (unsigned int i = 0; i < std::min(numArgs, numExpectedArgs); ++i) {
        const std::string& argName = expectedArgs[i].GetName();
        if (rval.second.has_key(argName)) {
            TfPyThrowTypeError(
                TfStringPrintf("Multiple values for keyword argument '%s'",
                               argName.c_str()));
        }

        rval.second[argName] = args[i];
    }

    if (numArgs > numExpectedArgs) {
        rval.first = tuple(args[slice(numExpectedArgs, numArgs)]);
    }

    return rval;
}

PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_BASE_TF_PY_ARG_BOOST_H
