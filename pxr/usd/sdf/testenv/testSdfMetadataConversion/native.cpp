// Copyright 2026 Pixar
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
#include "pxr/usd/sdf/types.h"
#include "pxr/base/tf/pyError.h"
#include "pxr/base/tf/pyObjWrapper.h"
#include "pxr/base/vt/dictionary.h"
#include "pxr/base/vt/array.h"
#include "pxr/base/vt/value.h"
#include "pxr/base/vt/valueToPython.h"
#include "pxr/base/arch/export.h"
#include <exception>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE
extern "C" ARCH_EXPORT int CheckNativeMetadata()
{
    VtDictionary dictionary;
    dictionary["value"] = VtValue(std::vector<VtValue>{VtValue(1), VtValue(2.5)});
    std::string message;
    return SdfConvertToValidMetadataDictionary(&dictionary, &message) &&
        dictionary["value"].Get<VtIntArray>()[1] == 2 && message.empty();
}

extern "C" ARCH_EXPORT PyObject* ConvertRawMetadata(PyObject* object)
{
    try {
        VtDictionary dictionary;
        dictionary["value"] = VtValue(TfPyObjWrapper(object, TfPyBorrowedReference));
        std::string message;
        const bool success = SdfConvertToValidMetadataDictionary(&dictionary, &message);
        TfPyObjWrapper converted = Vt_GetPythonObjectFromHeldValue(dictionary["value"]);
        if (PyErr_Occurred()) return nullptr;
        return Py_BuildValue("NOs", PyBool_FromLong(success), converted.ptr(), message.c_str());
    } catch (TfPyErrorAlreadySet const&) {
        return nullptr;
    } catch (std::exception const& error) {
        PyErr_SetString(PyExc_RuntimeError, error.what());
        return nullptr;
    } catch (...) {
        if (!PyErr_Occurred()) PyErr_SetString(PyExc_RuntimeError, "Unknown native exception");
        return nullptr;
    }
}
