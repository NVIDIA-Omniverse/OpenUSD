//
// Copyright 2016 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_USD_PCP_BOOST_PY_UTILS_H
#define PXR_USD_PCP_BOOST_PY_UTILS_H

#include "pxr/external/boost/python/dict.hpp"
#include "pxr/external/boost/python/extract.hpp"

#include "pxr/pxr.h"
#include "pxr/usd/pcp/types.h"
#include "pxr/base/tf/diagnosticLite.h"

#include <string>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

namespace Pcp_BoostPythonDetail {
template <typename T>
inline T*
ExtractValue(pxr_boost::python::object &pyObject, T* varPtr)
{
    pxr_boost::python::extract<T> extractor(pyObject);
    if (extractor.check()) {
        *varPtr = extractor();
        return varPtr;
    }
    return nullptr;
}
}

/// Convert the Python dictionary \p dict to a PcpVariantFallbackMap
/// object and return it via \p result, returning true if successful.
/// Empty keys or vectors are ignored. Entries are applied in iteration order;
/// entries preceding an invalid key or value remain in \p result on failure.
/// Include this adapter and link pcpBoostPython from binding code.
inline bool
PcpVariantFallbackMapFromPython(
    const pxr_boost::python::dict& dict,
    PcpVariantFallbackMap *result)
{
    using namespace pxr_boost::python;
    object iterItems = dict.items();
    for (int i = 0; i < len(iterItems); ++i) {
        object key = iterItems[i][0];
        object value = iterItems[i][1];
        std::string k;
        std::vector<std::string> v;
        if (!Pcp_BoostPythonDetail::ExtractValue(key, &k)) {
            TF_CODING_ERROR("unrecognized type for PcpVariantFallbackMap key");
            return false;
        }
        if (!Pcp_BoostPythonDetail::ExtractValue(value, &v)) {
            TF_CODING_ERROR("unrecognized type for PcpVariantFallbackMap val");
            return false;
        }
        if (!k.empty() && !v.empty()) {
            (*result)[k] = v;
        }
    }
    return true;
}

PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_USD_PCP_BOOST_PY_UTILS_H
