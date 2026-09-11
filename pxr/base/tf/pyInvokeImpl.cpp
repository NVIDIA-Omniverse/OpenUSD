//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//

#include "pxr/pxr.h"

#include "pxr/base/tf/pyInvokeImpl.h"

#include "pxr/base/tf/diagnostic.h"
#include "pxr/base/tf/errorMark.h"
#include "pxr/base/tf/pyError.h"
#include "pxr/base/tf/stringUtils.h"

PXR_NAMESPACE_OPEN_SCOPE

bool Tf_PyInvokeImpl(
    const std::string &moduleName,
    const std::string &callableExpr,
    PyObject *posArgs,
    PyObject *kwArgs,
    PyObject **resultObjOut)
{
    static const char* const listVarName = "_Tf_invokeList_";
    static const char* const dictVarName = "_Tf_invokeDict_";
    static const char* const resultVarName = "_Tf_invokeResult_";

    if (!TF_VERIFY(posArgs) || !TF_VERIFY(kwArgs) || !TF_VERIFY(resultObjOut)) {
        return false;
    }

    // Build globals dict, containing builtins and args.
    // No need for TfScriptModuleLoader; our python code performs import.
    PyObject *globals = PyDict_New();
    if (!globals) {
        TfPyConvertPythonExceptionToTfErrors();
        PyErr_Clear();
        return false;
    }

    PyObject *builtins = PyImport_ImportModule("builtins");
    if (!builtins) {
        Py_DECREF(globals);
        TfPyConvertPythonExceptionToTfErrors();
        PyErr_Clear();
        return false;
    }

    if (PyDict_SetItemString(globals, "__builtins__", builtins) != 0 ||
        PyDict_SetItemString(globals, listVarName, posArgs) != 0 ||
        PyDict_SetItemString(globals, dictVarName, kwArgs) != 0) {
        Py_DECREF(builtins);
        Py_DECREF(globals);
        TfPyConvertPythonExceptionToTfErrors();
        PyErr_Clear();
        return false;
    }
    Py_DECREF(builtins);

    // Build python code for interpreter.
    // Import, look up callable, perform call, store result.
    const std::string pyStr = TfStringPrintf(
        "import %s\n"
        "%s = %s.%s(*%s, **%s)\n",
        moduleName.c_str(),
        resultVarName,
        moduleName.c_str(),
        callableExpr.c_str(),
        listVarName,
        dictVarName);

    TfErrorMark errorMark;

    // Execute code.
    PyObject *runResult = PyRun_String(
        pyStr.c_str(), Py_file_input, globals, globals);
    if (!runResult) {
        Py_DECREF(globals);
        TfPyConvertPythonExceptionToTfErrors();
        PyErr_Clear();
        return false;
    }
    Py_DECREF(runResult);

    // Bail if python code raised any TfErrors.
    if (!errorMark.IsClean()) {
        Py_DECREF(globals);
        return false;
    }

    // Look up result.  If we got this far, it should be there.
    PyObject *result = PyDict_GetItemString(globals, resultVarName);
    if (!TF_VERIFY(result)) {
        Py_DECREF(globals);
        return false;
    }

    Py_INCREF(result);
    *resultObjOut = result;
    Py_DECREF(globals);
    return true;
}

PXR_NAMESPACE_CLOSE_SCOPE
