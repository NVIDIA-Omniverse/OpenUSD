//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//

#include "pxr/pxr.h"

#include "pxr/base/tf/pyErrorInternal.h"

#if defined(PXR_PYTHON_SUPPORT_ENABLED) && !defined(Py_LIMITED_API)

#include "pxr/base/tf/diagnosticMgr.h"
#include "pxr/base/tf/error.h"
#include "pxr/base/tf/iterator.h"

#include "pxr/external/boost/python/borrowed.hpp"
#include "pxr/external/boost/python/extract.hpp"
#include "pxr/external/boost/python/handle.hpp"
#include "pxr/external/boost/python/list.hpp"
#include "pxr/external/boost/python/object.hpp"
#include "pxr/external/boost/python/tuple.hpp"

#include <cstdint>
#include <cstring>
#include <exception>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

using namespace pxr_boost::python;

PyObject *
Tf_PyCreateErrorException(TfErrorMark const &m)
{
    list args;
    for (TfErrorMark::Iterator e = m.GetBegin(); e != m.GetEnd(); ++e) {
        if (e->GetErrorCode() != TF_PYTHON_EXCEPTION) {
            args.append(*e);
        }
    }

    return PyObject_CallObject(Tf_PyGetErrorExceptionClass(),
                               tuple(args).ptr());
}

bool
Tf_PyAppendErrorsFromException(PyObject *exception)
{
    object exceptionObj(handle<>(borrowed(exception)));
    object args = exceptionObj.attr("args");
    extract<std::vector<TfError>> extractor(args);
    if (!extractor.check()) {
        return false;
    }

    std::vector<TfError> errs = extractor();
    TF_FOR_ALL(e, errs) {
        TfDiagnosticMgr::GetInstance().AppendError(*e);
    }
    return true;
}

void
Tf_PyRethrowSavedTfException(PyObject *exception)
{
    object exceptionObj(handle<>(borrowed(exception)));
    if (!PyObject_HasAttrString(exceptionObj.ptr(), "_pxr_SavedTfException")) {
        return;
    }

    extract<uintptr_t> extractor(exceptionObj.attr("_pxr_SavedTfException"));
    if (!extractor.check()) {
        return;
    }

    uintptr_t addr = extractor();
    std::exception_ptr *excPtrPtr;
    memcpy(&excPtrPtr, &addr, sizeof(addr));
    std::exception_ptr eptr = *excPtrPtr;
    delete excPtrPtr;
    std::rethrow_exception(eptr);
}

PXR_NAMESPACE_CLOSE_SCOPE

#endif
