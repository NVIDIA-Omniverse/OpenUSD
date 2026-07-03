//
// Copyright 2016 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/usd/usdLux/photometricDistantLightAPI.h"
#include "pxr/usd/usd/schemaRegistry.h"
#include "pxr/usd/usd/typed.h"

#include "pxr/usd/sdf/types.h"
#include "pxr/usd/sdf/assetPath.h"

PXR_NAMESPACE_OPEN_SCOPE

TF_REGISTRY_FUNCTION(TfType)
{
    TfType::Define<UsdLuxPhotometricDistantLightAPI,
        TfType::Bases< UsdAPISchemaBase > >();

}

/* virtual */
UsdLuxPhotometricDistantLightAPI::~UsdLuxPhotometricDistantLightAPI()
{
}

/* static */
UsdLuxPhotometricDistantLightAPI
UsdLuxPhotometricDistantLightAPI::Get(const UsdStagePtr &stage, const SdfPath &path)
{
    if (!stage) {
        TF_CODING_ERROR("Invalid stage");
        return UsdLuxPhotometricDistantLightAPI();
    }
    return UsdLuxPhotometricDistantLightAPI(stage->GetPrimAtPath(path));
}

/* virtual */
UsdSchemaKind UsdLuxPhotometricDistantLightAPI::_GetSchemaKind() const
{
    return UsdLuxPhotometricDistantLightAPI::schemaKind;
}

/* static */
bool
UsdLuxPhotometricDistantLightAPI::CanApply(
    const UsdPrim &prim, std::string *whyNot)
{
    return prim.CanApplyAPI<UsdLuxPhotometricDistantLightAPI>(whyNot);
}

/* static */
UsdLuxPhotometricDistantLightAPI
UsdLuxPhotometricDistantLightAPI::Apply(const UsdPrim &prim)
{
    if (prim.ApplyAPI<UsdLuxPhotometricDistantLightAPI>()) {
        return UsdLuxPhotometricDistantLightAPI(prim);
    }
    return UsdLuxPhotometricDistantLightAPI();
}

/* static */
const TfType &
UsdLuxPhotometricDistantLightAPI::_GetStaticTfType()
{
    static TfType tfType = TfType::Find<UsdLuxPhotometricDistantLightAPI>();
    return tfType;
}

/* static */
bool
UsdLuxPhotometricDistantLightAPI::_IsTypedSchema()
{
    static bool isTyped = _GetStaticTfType().IsA<UsdTyped>();
    return isTyped;
}

/* virtual */
const TfType &
UsdLuxPhotometricDistantLightAPI::_GetTfType() const
{
    return _GetStaticTfType();
}

UsdAttribute
UsdLuxPhotometricDistantLightAPI::GetPhotometricIlluminanceAttr() const
{
    return GetPrim().GetAttribute(UsdLuxTokens->photometricIlluminance);
}

UsdAttribute
UsdLuxPhotometricDistantLightAPI::CreatePhotometricIlluminanceAttr(VtValue const &defaultValue, bool writeSparsely) const
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
UsdLuxPhotometricDistantLightAPI::GetSchemaAttributeNames(bool includeInherited)
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
