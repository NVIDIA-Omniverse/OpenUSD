//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_BASE_TF_SCRIPT_MODULE_LOADER_BOOST_H
#define PXR_BASE_TF_SCRIPT_MODULE_LOADER_BOOST_H

/// \file tf/scriptModuleLoaderBoost.h
/// Boost.Python adapters for TfScriptModuleLoader.

#include "pxr/pxr.h"

#include "pxr/base/tf/scriptModuleLoader.h"

#include "pxr/external/boost/python/dict.hpp"
#include "pxr/external/boost/python/handle.hpp"

PXR_NAMESPACE_OPEN_SCOPE

inline pxr_boost::python::dict
TfScriptModuleLoader_GetModulesDict(TfScriptModuleLoader const &loader)
{
    PyObject *dict = loader.GetModulesDict();
    return dict ? pxr_boost::python::dict(
                      pxr_boost::python::handle<>(dict))
                : pxr_boost::python::dict();
}

PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_BASE_TF_SCRIPT_MODULE_LOADER_BOOST_H
