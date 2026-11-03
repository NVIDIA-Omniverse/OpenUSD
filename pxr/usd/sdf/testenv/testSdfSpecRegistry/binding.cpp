// Copyright 2026 Pixar
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
#include "native.h"
#include "pxr/usd/sdf/primSpec.h"
#include "pxr/usd/sdf/boost/pySpec.h"
#include "pxr/external/boost/python.hpp"
PXR_NAMESPACE_USING_DIRECTIVE
static SdfSpecHandle Prim() { return SdfSpecHandle(NativePrim()); }
static SdfHandle<const SdfSpec> Attribute() { return SdfHandle<const SdfSpec>(NativeAttribute()); }
static SdfSpecHandle Variant() { return SdfSpecHandle(NativeVariant()); }
static SdfPrimSpecHandle VariantPrim()
{
    return SdfPrimSpecHandle(Sdf_CastAccess::CastSpec<SdfPrimSpec, SdfSpec>(NativeVariant()));
}
static SdfSpecHandle Echo(const SdfSpecHandle& value) { return value; }
static std::string Repr(const pxr_boost::python::object& self)
{
    return Sdf_PySpecDetail::_SpecRepr(self, &NativePrim());
}
PXR_BOOST_PYTHON_MODULE(sdfSpecBindingConsumer)
{
    using namespace pxr_boost::python;
    def("prim", Prim);
    def("attribute", Attribute);
    def("variant", Variant);
    def("variant_prim", VariantPrim);
    def("echo", Echo);
    def("repr", Repr);
    def("remove", RemoveNativePrim);
    def("clear", ClearNativeSpecs);
}
