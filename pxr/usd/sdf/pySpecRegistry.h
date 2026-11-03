// Copyright 2026 Pixar
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
#ifndef PXR_USD_SDF_PY_SPEC_REGISTRY_H
#define PXR_USD_SDF_PY_SPEC_REGISTRY_H

/// \file sdf/pySpecRegistry.h
/// Binding-neutral shared dispatch for Python holders of Sdf specs.
#include "pxr/pxr.h"
#include "pxr/usd/sdf/api.h"
#include "pxr/base/tf/pySafePython.h"
#include <typeinfo>

PXR_NAMESPACE_OPEN_SCOPE
class SdfSpec;

namespace Sdf_PySpecDetail {

/// The callback borrows the spec for the duration of the call and returns a
/// new Python reference, or null with a Python exception set. It runs with
/// the GIL held and must not throw binding-library exceptions into the core.
/// Registered code must outlive registry use; unloading is not supported.
using _HolderCreator = PyObject* (*)(const SdfSpec&);

/// Register a creator for an existing TfType. The first registration wins;
/// duplicates, unknown types and null callbacks report Tf coding errors.
/// Registration acquires the GIL when Python is initialized and synchronizes
/// access to the shared registry, including registration before Python starts.
SDF_API void _RegisterHolderCreator(const std::type_info&, _HolderCreator);

/// Create a holder using the native spec/schema casting rules and current
/// registrations. Dormant specs or incompatible casts return a new None
/// reference. A missing creator also returns None and reports a Tf coding error.
/// Python must be initialized. This function acquires the GIL; callers must
/// hold it across invocation and inspection of a pending Python error.
/// Existing Python errors are preserved without invoking the creator.
/// Registry locks are released before invoking callbacks, allowing reentrancy.
SDF_API PyObject* _CreateHolder(const std::type_info&, const SdfSpec&);

} // namespace Sdf_PySpecDetail
PXR_NAMESPACE_CLOSE_SCOPE
#endif
