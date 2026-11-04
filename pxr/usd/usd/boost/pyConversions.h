// Copyright 2016 Pixar
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
#ifndef PXR_USD_USD_BOOST_PY_CONVERSIONS_H
#define PXR_USD_USD_BOOST_PY_CONVERSIONS_H

// Boost.Python adapters. Native callers include pyConversionRuntime.h.
#include "pxr/usd/usd/pyConversionRuntime.h"
#ifdef PXR_PYTHON_SUPPORT_ENABLED
#include "pxr/base/tf/boost/pyObjWrapperBoost.h"
#include "pxr/base/vt/value.h"

PXR_NAMESPACE_OPEN_SCOPE
inline VtValue
UsdPythonToSdfType(const pxr_boost::python::object& pyVal,
                   const SdfValueTypeName& targetType)
{
    return UsdPythonToSdfType(TfPyObjWrapperFromBoostObject(pyVal), targetType);
}

inline bool
UsdPythonToMetadataValue(const TfToken& key, const TfToken& keyPath,
                         const pxr_boost::python::object& pyVal, VtValue* result)
{
    return UsdPythonToMetadataValue(
        key, keyPath, TfPyObjWrapperFromBoostObject(pyVal), result);
}
PXR_NAMESPACE_CLOSE_SCOPE
#endif
#endif // PXR_USD_USD_BOOST_PY_CONVERSIONS_H
