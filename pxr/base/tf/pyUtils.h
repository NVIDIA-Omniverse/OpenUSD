//
// Copyright 2016 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_BASE_TF_PY_UTILS_H
#define PXR_BASE_TF_PY_UTILS_H

/// \file tf/pyUtils.h
/// Miscellaneous Utilities for dealing with script.

#include "pxr/pxr.h"

#include "pxr/base/tf/refPtr.h"
#include "pxr/base/tf/weakPtr.h"
#include "pxr/base/tf/diagnosticLite.h"
#include "pxr/base/tf/preprocessorUtilsLite.h"
#include "pxr/base/tf/pySafePython.h"
#include "pxr/base/tf/pyInterpreter.h"
#include "pxr/base/tf/pyLock.h"
#include "pxr/base/tf/api.h"

#include <string>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

/// A macro which expands to the proper __repr__ prefix for a library.  This is
/// the "canonical" name of the module that the system uses to identify it
/// followed by a '.'.  This can be used in the implementation of __repr__
///
/// \hideinitializer
#define TF_PY_REPR_PREFIX \
    std::string(TF_PP_STRINGIZE(MFB_PACKAGE_MODULE) ".")

/// Returns true if python is initialized.
TF_API bool TfPyIsInitialized();

/// Raises a Python \c IndexError with the given error \p msg and throws a
/// TfPyErrorAlreadySet exception. Callers must hold the GIL before calling
/// this function.
TF_API void TfPyThrowIndexError(const char *msg);

/// \overload
inline void TfPyThrowIndexError(std::string const &msg)
{
    TfPyThrowIndexError(msg.c_str());
}

/// Raises a Python \c RuntimeError with the given error \p msg and throws a
/// TfPyErrorAlreadySet exception. Callers must hold the GIL before calling
/// this function.
TF_API void TfPyThrowRuntimeError(const char *msg);

/// \overload
inline void TfPyThrowRuntimeError(std::string const &msg)
{
    TfPyThrowRuntimeError(msg.c_str());
}

/// Raises a Python \c StopIteration with the given error \p msg and throws a
/// TfPyErrorAlreadySet exception. Callers must hold the GIL before calling
/// this function.
TF_API void TfPyThrowStopIteration(const char *msg);

/// \overload
inline void TfPyThrowStopIteration(std::string const &msg)
{
    TfPyThrowStopIteration(msg.c_str());
}

/// Raises a Python \c KeyError with the given error \p msg and throws a
/// TfPyErrorAlreadySet exception. Callers must hold the GIL before calling
/// this function.
TF_API void TfPyThrowKeyError(const char *msg);

/// \overload
inline void TfPyThrowKeyError(std::string const &msg)
{
    TfPyThrowKeyError(msg.c_str());
}

/// Raises a Python \c ValueError with the given error \p msg and throws a
/// TfPyErrorAlreadySet exception. Callers must hold the GIL before calling
/// this function.
TF_API void TfPyThrowValueError(const char *msg);

/// \overload
inline void TfPyThrowValueError(std::string const &msg)
{
    TfPyThrowValueError(msg.c_str());
}

/// Raises a Python \c TypeError with the given error \p msg and throws a
/// TfPyErrorAlreadySet exception. Callers must hold the GIL before calling
/// this function.
TF_API void TfPyThrowTypeError(const char *msg);

/// \overload
inline void TfPyThrowTypeError(std::string const &msg)
{
    TfPyThrowTypeError(msg.c_str());
}

/// Return true iff \a obj is null or None.
TF_API bool TfPyIsNone(PyObject *obj);

// Helper for \c TfPyObject().
TF_API void Tf_PyObjectError(bool printError);

/// Return repr(t).
///
/// Calls PyObject_Repr on the given python object.
TF_API std::string TfPyObjectRepr(PyObject *t);

/// Return a positive index in the range [0,size).  If \a throwError is true,
/// this will throw an index error if the resulting index is out of range.
TF_API 
int64_t
TfPyNormalizeIndex(int64_t index, uint64_t size, bool throwError = false);

/// Return the name of the class of \a obj.
TF_API std::string TfPyGetClassName(PyObject *obj);

/// Load the python module \a moduleName.  This is used by some low-level
/// infrastructure code to load python wrapper modules corresponding to C++
/// shared libraries when they are needed.  It should generally not need to be
/// called from normal user code.
TF_API
void Tf_PyLoadScriptModule(std::string const &name);

/// Create a python bytearray from an input buffer and size.
///
/// If a size of zero is passed in this function will return a valid python
/// bytearray of size zero.
///
/// A new reference is returned on success; null is returned on failure.
TF_API
PyObject *TfPyCopyBufferToPyByteArray(const char* buffer, size_t size);

/// Return a vector of strings containing the current python traceback.
///
/// The vector contains the same strings that python's traceback.format_stack()
/// returns.
TF_API
std::vector<std::string> TfPyGetTraceback();

/// Set an environment variable in \c os.environ.
///
/// This function is equivalent to
///
/// \code
///    def PySetenv(name, value):
///        try:
///            import os
///            os.environ[name] = value
///            return True
///        except:
///            return False
/// \endcode
///
/// Calling this function without first initializing Python is an error and
/// returns \c false.
///
/// Note that this function will import the \c os module, causing \c
/// os.environ to be poputated.  All modifications to the environment after \c
/// os has been imported must be made with this function or \c TfSetenv if it
/// important that they appear in \c os.environ.
TF_API
bool TfPySetenv(const std::string & name, const std::string & value);

/// Remove an environment variable from \c os.environ.
///
/// This function is equivalent to
///
/// \code
///    def PyUnsetenv(name):
///        try:
///            import os
///            if name in os.environ:
///                del os.environ[name]
///            return True
///        except:
///            return False
/// \endcode
///
/// Calling this function without first initializing Python is an error and
/// returns \c false.
///
/// Note that this function will import the \c os module, causing \c
/// os.environ to be poputated.  All modifications to the environment after \c
/// os has been imported must be made with this function or \c TfUnsetenv if
/// it important that they appear in \c os.environ.
TF_API
bool TfPyUnsetenv(const std::string & name);

/// Print a standard traceback to sys.stderr and clear the error indicator.
/// If the error is a KeyboardInterrupt then this does nothing.  Call this
/// function only when the error indicator is set.
TF_API
void TfPyPrintError();

PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_BASE_TF_PY_UTILS_H
