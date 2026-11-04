// Copyright 2026 Pixar
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
#include "pxr/usd/usd/boost/pyConversions.h"
#include "pxr/usd/usd/boost/pyEditContext.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usd/variantSets.h"
#include "pxr/usd/sdf/schema.h"
#include "pxr/base/tf/boost/pyPtrHelpersBoost.h"
#include "pxr/external/boost/python.hpp"

PXR_NAMESPACE_USING_DIRECTIVE
namespace bp = pxr_boost::python;

static bp::object Convert(const bp::object& value, const std::string& type)
{
    return TfPyObjWrapperToBoostObject(UsdVtValueToPython(
        UsdPythonToSdfType(value, SdfSchema::GetInstance().FindType(type))));
}
static bp::object Metadata(const bp::object& value, const std::string& key)
{
    VtValue result;
    if (!UsdPythonToMetadataValue(TfToken(key), TfToken(), value, &result))
        return bp::object();
    return TfPyObjWrapperToBoostObject(UsdVtValueToPython(result));
}
static UsdPyEditContext Context(const UsdStagePtr& stage, const SdfLayerHandle& layer)
{
    return UsdPyEditContext(stage, UsdEditTarget(layer));
}
static UsdPyEditContext DefaultContext(const UsdStagePtr& stage)
{
    return UsdPyEditContext(stage);
}
static UsdPyEditContext VariantContext(const UsdVariantSet& variant, const SdfLayerHandle& layer)
{
    return UsdPyEditContext(variant.GetVariantEditContext(layer));
}
PXR_BOOST_PYTHON_MODULE(usdBindingHelpers)
{
    bp::def("convert", Convert);
    bp::def("metadata", Metadata);
    bp::def("context", Context);
    bp::def("default_context", DefaultContext);
    bp::def("variant_context", VariantContext);
}
