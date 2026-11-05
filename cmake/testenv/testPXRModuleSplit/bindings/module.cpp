#include <Python.h>
#include "pxr/base/tf/boost/pyModuleBoost.h"
#include "pxr/external/boost/python/def.hpp"
#include "../native/probe.h"
PXR_NAMESPACE_USING_DIRECTIVE
extern "C"
#ifdef _WIN32
__declspec(dllimport)
#endif
int RuntimeAnswer();
#ifdef Py_LIMITED_API
#error Native Limited API restriction leaked into the module
#endif
#ifndef BINDING_ONLY_MARKER
#error Module lost its explicit binding dependency
#endif
TF_WRAP_MODULE
{
    pxr_boost::python::def("answer", NativeAnswer);
    pxr_boost::python::def("runtimeAnswer", RuntimeAnswer);
}
