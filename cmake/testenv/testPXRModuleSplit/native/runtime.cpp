#include <Python.h>
#ifndef Py_LIMITED_API
#error Native Python source did not enforce the Limited API
#endif
extern "C"
#ifdef _WIN32
__declspec(dllexport)
#endif
int RuntimeAnswer()
{
    PyObject *value = PyLong_FromLong(42);
    if (!value) return -1;
    const long result = PyLong_AsLong(value);
    Py_DECREF(value);
    return static_cast<int>(result);
}
