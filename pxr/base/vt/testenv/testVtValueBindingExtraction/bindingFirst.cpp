//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/base/vt/boost/valueBoost.h"
#include "nativeTypes.h"
#include <type_traits>
PXR_NAMESPACE_USING_DIRECTIVE
static_assert(std::is_same_v<
    Vt_ValueGetStored<pxr_boost::python::object>::Type, TfPyObjWrapper>);
void CheckBindingFirst(pxr_boost::python::object const &obj) {
    VtValue value(obj);
    value = obj;
}
