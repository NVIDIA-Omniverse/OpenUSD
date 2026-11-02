//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/base/vt/value.h"
#include "pxr/external/boost/python/object.hpp"
PXR_NAMESPACE_USING_DIRECTIVE
void MustNotStoreBindingObject(pxr_boost::python::object const &obj) {
    VtValue unsafe(obj);
}
