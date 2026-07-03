//
// Copyright 2016 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/usd/usdLux/physicalLightIlluminantAPI.h"
#include "pxr/usd/usd/schemaRegistry.h"
#include "pxr/usd/usd/typed.h"

#include "pxr/usd/sdf/types.h"
#include "pxr/usd/sdf/assetPath.h"

PXR_NAMESPACE_OPEN_SCOPE

TF_REGISTRY_FUNCTION(TfType)
{
    TfType::Define<UsdLuxPhysicalLightIlluminantAPI,
        TfType::Bases< UsdAPISchemaBase > >();

}

/* virtual */
UsdLuxPhysicalLightIlluminantAPI::~UsdLuxPhysicalLightIlluminantAPI()
{
}

/* static */
UsdLuxPhysicalLightIlluminantAPI
UsdLuxPhysicalLightIlluminantAPI::Get(const UsdStagePtr &stage, const SdfPath &path)
{
    if (!stage) {
        TF_CODING_ERROR("Invalid stage");
        return UsdLuxPhysicalLightIlluminantAPI();
    }
    return UsdLuxPhysicalLightIlluminantAPI(stage->GetPrimAtPath(path));
}

/* virtual */
UsdSchemaKind UsdLuxPhysicalLightIlluminantAPI::_GetSchemaKind() const
{
    return UsdLuxPhysicalLightIlluminantAPI::schemaKind;
}

/* static */
bool
UsdLuxPhysicalLightIlluminantAPI::CanApply(
    const UsdPrim &prim, std::string *whyNot)
{
    return prim.CanApplyAPI<UsdLuxPhysicalLightIlluminantAPI>(whyNot);
}

/* static */
UsdLuxPhysicalLightIlluminantAPI
UsdLuxPhysicalLightIlluminantAPI::Apply(const UsdPrim &prim)
{
    if (prim.ApplyAPI<UsdLuxPhysicalLightIlluminantAPI>()) {
        return UsdLuxPhysicalLightIlluminantAPI(prim);
    }
    return UsdLuxPhysicalLightIlluminantAPI();
}

/* static */
const TfType &
UsdLuxPhysicalLightIlluminantAPI::_GetStaticTfType()
{
    static TfType tfType = TfType::Find<UsdLuxPhysicalLightIlluminantAPI>();
    return tfType;
}

/* static */
bool
UsdLuxPhysicalLightIlluminantAPI::_IsTypedSchema()
{
    static bool isTyped = _GetStaticTfType().IsA<UsdTyped>();
    return isTyped;
}

/* virtual */
const TfType &
UsdLuxPhysicalLightIlluminantAPI::_GetTfType() const
{
    return _GetStaticTfType();
}

UsdAttribute
UsdLuxPhysicalLightIlluminantAPI::GetPhysicalIlluminantAttr() const
{
    return GetPrim().GetAttribute(UsdLuxTokens->physicalIlluminant);
}

UsdAttribute
UsdLuxPhysicalLightIlluminantAPI::CreatePhysicalIlluminantAttr(VtValue const &defaultValue, bool writeSparsely) const
{
    return UsdSchemaBase::_CreateAttr(UsdLuxTokens->physicalIlluminant,
                       SdfValueTypeNames->Token,
                       /* custom = */ false,
                       SdfVariabilityVarying,
                       defaultValue,
                       writeSparsely);
}
UsdAttribute
UsdLuxPhysicalLightIlluminantAPI::GetPhysicalCustomIlluminantAttr() const
{
    return GetPrim().GetAttribute(UsdLuxTokens->physicalCustomIlluminant);
}

UsdAttribute
UsdLuxPhysicalLightIlluminantAPI::CreatePhysicalCustomIlluminantAttr(VtValue const &defaultValue, bool writeSparsely) const
{
    return UsdSchemaBase::_CreateAttr(UsdLuxTokens->physicalCustomIlluminant,
                       SdfValueTypeNames->Float2Array,
                       /* custom = */ false,
                       SdfVariabilityVarying,
                       defaultValue,
                       writeSparsely);
}

namespace {
static inline TfTokenVector
_ConcatenateAttributeNames(const TfTokenVector& left,const TfTokenVector& right)
{
    TfTokenVector result;
    result.reserve(left.size() + right.size());
    result.insert(result.end(), left.begin(), left.end());
    result.insert(result.end(), right.begin(), right.end());
    return result;
}
}

/*static*/
const TfTokenVector&
UsdLuxPhysicalLightIlluminantAPI::GetSchemaAttributeNames(bool includeInherited)
{
    static TfTokenVector localNames = {
        UsdLuxTokens->physicalIlluminant,
        UsdLuxTokens->physicalCustomIlluminant,
    };
    static TfTokenVector allNames =
        _ConcatenateAttributeNames(
            UsdAPISchemaBase::GetSchemaAttributeNames(true),
            localNames);

    if (includeInherited)
        return allNames;
    else
        return localNames;
}

PXR_NAMESPACE_CLOSE_SCOPE

// ===================================================================== //
// Feel free to add custom code below this line. It will be preserved by
// the code generator.
//
// Just remember to wrap code in the appropriate delimiters:
// 'PXR_NAMESPACE_OPEN_SCOPE', 'PXR_NAMESPACE_CLOSE_SCOPE'.
// ===================================================================== //
// --(BEGIN CUSTOM CODE)--
