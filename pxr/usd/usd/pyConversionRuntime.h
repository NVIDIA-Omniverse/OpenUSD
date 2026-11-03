// Copyright 2026 Pixar
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
#ifndef PXR_USD_USD_PY_CONVERSION_RUNTIME_H
#define PXR_USD_USD_PY_CONVERSION_RUNTIME_H

/// \file usd/pyConversionRuntime.h
/// Binding-neutral Python conversion services.
///
/// These services acquire the GIL. Python must be initialized. The binding
/// registers Vt's generic VtValue extraction and to-Python dispatch, mandatory
/// TfTokenVector extraction, and checked string-vector/VtStringArray extraction.
/// Callbacks borrow inputs, preserve Python errors, and must not throw binding
/// exceptions through core code. Missing generic extraction reports TypeError;
/// an existing Python exception is preserved unchanged.

#include "pxr/pxr.h"
#include "pxr/usd/usd/api.h"
#include "pxr/base/tf/pyObjWrapper.h"

PXR_NAMESPACE_OPEN_SCOPE
class VtValue;
class TfToken;
class SdfValueTypeName;

/// Deprecated compatibility conversion; use Vt's to-Python dispatch directly.
USD_API TfPyObjWrapper UsdVtValueToPython(const VtValue& value);

/// Convert Python input through the registered binding backend, then attempt
/// VtValue::CastToTypeOf using the target type's default value. Failed casts
/// retain the original inferred value, including wrapped Python objects.
USD_API VtValue UsdPythonToSdfType(
    TfPyObjWrapper pyVal, const SdfValueTypeName& targetType);

/// Convert a known Sdf metadata field, including dictionary key paths and
/// fallback-directed extraction. Unknown fields produce a Tf coding error and
/// return false. Successful empty values return true. Failed conversion leaves
/// result unchanged and raises the existing Python error via TfPyErrorAlreadySet.
USD_API bool UsdPythonToMetadataValue(
    const TfToken& key, const TfToken& keyPath,
    TfPyObjWrapper pyVal, VtValue* result);

PXR_NAMESPACE_CLOSE_SCOPE
#endif // PXR_USD_USD_PY_CONVERSION_RUNTIME_H
