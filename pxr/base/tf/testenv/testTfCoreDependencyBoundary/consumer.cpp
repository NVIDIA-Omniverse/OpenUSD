//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/base/tf/token.h"
#include "pxr/base/tf/pyInterpreter.h"
#include "pxr/base/tf/pyLock.h"

int main()
{
    const PXR_NS::TfToken token("core consumer");
    PXR_NS::TfPyInitialize();
    PXR_NS::TfPyLock lock;
    // Public Tf inline/template helpers also call the C API from the caller's
    // object file. Exercise that link requirement without naming Python in the
    // consumer's target_link_libraries: Tf must propagate the stable ABI.
    PyObject *value = PyLong_FromLong(42);
    if (!value) {
        return 1;
    }
    const bool valid = PyLong_AsLong(value) == 42;
    Py_DECREF(value);
    return valid && token.GetString() == "core consumer" ? 0 : 1;
}
