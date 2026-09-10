//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_BASE_TF_PY_ERROR_IMPL_H
#define PXR_BASE_TF_PY_ERROR_IMPL_H

#include "pxr/pxr.h"

#include "pxr/base/tf/api.h"

#include <exception>

PXR_NAMESPACE_OPEN_SCOPE

/// Binding-neutral sentinel for future use when Python's error indicator is
/// set and C++ control flow must exit immediately. Binding adapters should
/// translate this to their own "Python error already set" mechanism.
///
/// This is not thrown yet; current Boost.Python code still throws
/// pxr_boost::python::error_already_set for compatibility.
class TfPyErrorAlreadySet : public std::exception
{
public:
    const char *what() const noexcept override {
        return "Python error already set";
    }
};

void Tf_PySetIndexError(const char *msg);
void Tf_PySetRuntimeError(const char *msg);
void Tf_PySetStopIteration(const char *msg);
void Tf_PySetKeyError(const char *msg);
void Tf_PySetValueError(const char *msg);
void Tf_PySetTypeError(const char *msg);

TF_API void Tf_PyThrowErrorAlreadySet();

PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_BASE_TF_PY_ERROR_IMPL_H
