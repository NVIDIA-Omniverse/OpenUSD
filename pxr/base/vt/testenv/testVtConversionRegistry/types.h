//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef VT_REGISTRY_TEST_TYPES_H
#define VT_REGISTRY_TEST_TYPES_H

#include "pxr/base/tf/pySafePython.h"
#include "pxr/external/boost/python/errors.hpp"

struct Convertible {
    explicit Convertible(int n) : value(n) {}
    int value;
};
struct Registered {
    explicit Registered(int n) : value(n) {}
    Registered(Convertible const &other) : value(other.value + 100) {
        if (other.value < 0) {
            PyErr_SetString(PyExc_ValueError, "negative conversion");
            PXR_BOOST_NAMESPACE::python::throw_error_already_set();
        }
    }
    int value;
};
inline bool operator==(Registered const &a, Registered const &b) {
    return a.value == b.value;
}
#endif
