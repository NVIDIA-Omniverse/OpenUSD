#include <Python.h>
extern "C"
#ifdef _WIN32
__declspec(dllexport)
#endif
int FullTypeSize() { return sizeof(PyTypeObject); }
