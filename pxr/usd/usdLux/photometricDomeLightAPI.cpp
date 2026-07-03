//
// Copyright 2016 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/usd/usdLux/photometricDomeLightAPI.h"
#include "pxr/usd/usd/schemaRegistry.h"
#include "pxr/usd/usd/typed.h"

#include "pxr/usd/sdf/types.h"
#include "pxr/usd/sdf/assetPath.h"

PXR_NAMESPACE_OPEN_SCOPE

TF_REGISTRY_FUNCTION(TfType)
{
    TfType::Define<UsdLuxPhotometricDomeLightAPI,
        TfType::Bases< UsdAPISchemaBase > >();

}

/* virtual */
UsdLuxPhotometricDomeLightAPI::~UsdLuxPhotometricDomeLightAPI()
{
}

/* static */
UsdLuxPhotometricDomeLightAPI
UsdLuxPhotometricDomeLightAPI::Get(const UsdStagePtr &stage, const SdfPath &path)
{
    if (!stage) {
        TF_CODING_ERROR("Invalid stage");
        return UsdLuxPhotometricDomeLightAPI();
    }
    return UsdLuxPhotometricDomeLightAPI(stage->GetPrimAtPath(path));
}

/* virtual */
UsdSchemaKind UsdLuxPhotometricDomeLightAPI::_GetSchemaKind() const
{
    return UsdLuxPhotometricDomeLightAPI::schemaKind;
}

/* static */
bool
UsdLuxPhotometricDomeLightAPI::CanApply(
    const UsdPrim &prim, std::string *whyNot)
{
    return prim.CanApplyAPI<UsdLuxPhotometricDomeLightAPI>(whyNot);
}

/* static */
UsdLuxPhotometricDomeLightAPI
UsdLuxPhotometricDomeLightAPI::Apply(const UsdPrim &prim)
{
    if (prim.ApplyAPI<UsdLuxPhotometricDomeLightAPI>()) {
        return UsdLuxPhotometricDomeLightAPI(prim);
    }
    return UsdLuxPhotometricDomeLightAPI();
}

/* static */
const TfType &
UsdLuxPhotometricDomeLightAPI::_GetStaticTfType()
{
    static TfType tfType = TfType::Find<UsdLuxPhotometricDomeLightAPI>();
    return tfType;
}

/* static */
bool
UsdLuxPhotometricDomeLightAPI::_IsTypedSchema()
{
    static bool isTyped = _GetStaticTfType().IsA<UsdTyped>();
    return isTyped;
}

/* virtual */
const TfType &
UsdLuxPhotometricDomeLightAPI::_GetTfType() const
{
    return _GetStaticTfType();
}

UsdAttribute
UsdLuxPhotometricDomeLightAPI::GetPhotometricIlluminanceAttr() const
{
    return GetPrim().GetAttribute(UsdLuxTokens->photometricIlluminance);
}

UsdAttribute
UsdLuxPhotometricDomeLightAPI::CreatePhotometricIlluminanceAttr(VtValue const &defaultValue, bool writeSparsely) const
{
    return UsdSchemaBase::_CreateAttr(UsdLuxTokens->photometricIlluminance,
                       SdfValueTypeNames->Float,
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
UsdLuxPhotometricDomeLightAPI::GetSchemaAttributeNames(bool includeInherited)
{
    static TfTokenVector localNames = {
        UsdLuxTokens->photometricIlluminance,
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
