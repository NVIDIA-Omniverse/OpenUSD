//
// Copyright 2016 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_BASE_TF_PY_ERROR_H
#define PXR_BASE_TF_PY_ERROR_H

/// \file tf/error.h
/// Provide facilities for error handling in script.

#include "pxr/pxr.h"

#include "pxr/base/tf/api.h"
#include "pxr/base/tf/errorMark.h"

#include <exception>

PXR_NAMESPACE_OPEN_SCOPE

/// Binding-neutral sentinel for when Python's error indicator is set and C++
/// control flow must exit immediately. Binding adapters should translate this
/// to their own "Python error already set" mechanism.
class TfPyErrorAlreadySet : public std::exception
{
public:
    TF_API
    virtual ~TfPyErrorAlreadySet();

    TF_API
    const char *what() const noexcept override;
};

/// Converts any \a TfError objects in \a m into python exceptions.  User code
/// should generally not have to call this.  User code should generally not
/// have to call this, unless it's manually bridging between C++ & Python.
TF_API
bool TfPyConvertTfErrorsToPythonException(TfErrorMark const &m);

/// Convert the current python exception to \a TfError objects and post them
/// to the error system.  User code should generally not have to call this,
/// unless it's manually bridging between C++ & Python.
TF_API
void TfPyConvertPythonExceptionToTfErrors();

PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_BASE_TF_PY_ERROR_H
