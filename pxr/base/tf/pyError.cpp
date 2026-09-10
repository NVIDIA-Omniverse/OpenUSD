//
// Copyright 2016 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//

#include "pxr/pxr.h"

#include "pxr/base/tf/diagnostic.h"
#include "pxr/base/tf/error.h"
#include "pxr/base/tf/errorMark.h"
#include "pxr/base/tf/iterator.h"
#include "pxr/base/tf/pyError.h"
#include "pxr/base/tf/pyErrorInternal.h"
#include "pxr/base/tf/pySafePython.h"

using std::string;

PXR_NAMESPACE_OPEN_SCOPE

bool TfPyConvertTfErrorsToPythonException(TfErrorMark const &m) {
    // If there is a python exception somewhere in here, restore that, otherwise
    // raise a normal error exception.
    if (!m.IsClean()) {
        for (TfErrorMark::Iterator e = m.GetBegin(); e != m.GetEnd(); ++e) {
            if (e->GetErrorCode() == TF_PYTHON_EXCEPTION) {
                if (const TfPyExceptionState* info =
                        e->GetInfo<TfPyExceptionState>()) {
                    TfPyExceptionState(*info).Restore();
                    TfDiagnosticMgr::GetInstance().EraseError(e);

                    // XXX: We have a problem here: we've restored the
                    //      Python error exactly as it was but we may
                    //      have other errors still in the error mark. 
                    //      If we try to return to Python with errors
                    //      posted then we'll turn those errors into
                    //      a Python exception, interfering with what
                    //      we just did and possibly causing other
                    //      problems.  But if we clear the errors we
                    //      might lose something important.
                    //
                    //      For now we clear the errors.  This might
                    //      have to become something more complex,
                    //      like chained exceptions or a custom
                    //      exception holding a Python exception and
                    //      Tf errors.
                    m.Clear();
                    return true;
                } else {
                    // abort? should perhaps use polymorphic_downcast workalike
                    // instead? throw a python error...
                }
            }
        }
        // make and set a python exception
        PyObject *excObj = Tf_PyCreateErrorException(m);
        if (excObj) {
            PyErr_SetObject(Tf_PyGetErrorExceptionClass(), excObj);
            Py_DecRef(excObj);
        }
        m.Clear();
        return true;
    }
    return false;
}


void
TfPyConvertPythonExceptionToTfErrors()
{
    // Get the python exception info.
    TfPyExceptionState exc = TfPyExceptionState::Fetch();
 
    // Replace the errors in m with errors parsed out of the exception.
    if (exc.GetType()) {
        if (exc.GetType() == Tf_PyGetErrorExceptionClass() &&
            exc.GetValue()) {
            // Replace the errors in m with errors pulled out of exc.
            Tf_PyAppendErrorsFromException(exc.GetValue());
        } else {
            TF_ERROR(exc, TF_PYTHON_EXCEPTION, "Tf Python Exception");
        }
    }
    else if (exc.GetValue()) {
        Tf_PyRethrowSavedTfException(exc.GetValue());
    }                
}

PXR_NAMESPACE_CLOSE_SCOPE
