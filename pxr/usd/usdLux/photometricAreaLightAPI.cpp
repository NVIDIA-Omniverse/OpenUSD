//
// Copyright 2016 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/usd/usdLux/photometricAreaLightAPI.h"
#include "pxr/usd/usd/schemaRegistry.h"
#include "pxr/usd/usd/typed.h"

#include "pxr/usd/sdf/types.h"
#include "pxr/usd/sdf/assetPath.h"

PXR_NAMESPACE_OPEN_SCOPE

TF_REGISTRY_FUNCTION(TfType)
{
    TfType::Define<UsdLuxPhotometricAreaLightAPI,
        TfType::Bases< UsdAPISchemaBase > >();

}

/* virtual */
UsdLuxPhotometricAreaLightAPI::~UsdLuxPhotometricAreaLightAPI()
{
}

/* static */
UsdLuxPhotometricAreaLightAPI
UsdLuxPhotometricAreaLightAPI::Get(const UsdStagePtr &stage, const SdfPath &path)
{
    if (!stage) {
        TF_CODING_ERROR("Invalid stage");
        return UsdLuxPhotometricAreaLightAPI();
    }
    return UsdLuxPhotometricAreaLightAPI(stage->GetPrimAtPath(path));
}

/* virtual */
UsdSchemaKind UsdLuxPhotometricAreaLightAPI::_GetSchemaKind() const
{
    return UsdLuxPhotometricAreaLightAPI::schemaKind;
}

/* static */
bool
UsdLuxPhotometricAreaLightAPI::CanApply(
    const UsdPrim &prim, std::string *whyNot)
{
    return prim.CanApplyAPI<UsdLuxPhotometricAreaLightAPI>(whyNot);
}

/* static */
UsdLuxPhotometricAreaLightAPI
UsdLuxPhotometricAreaLightAPI::Apply(const UsdPrim &prim)
{
    if (prim.ApplyAPI<UsdLuxPhotometricAreaLightAPI>()) {
        return UsdLuxPhotometricAreaLightAPI(prim);
    }
    return UsdLuxPhotometricAreaLightAPI();
}

/* static */
const TfType &
UsdLuxPhotometricAreaLightAPI::_GetStaticTfType()
{
    static TfType tfType = TfType::Find<UsdLuxPhotometricAreaLightAPI>();
    return tfType;
}

/* static */
bool
UsdLuxPhotometricAreaLightAPI::_IsTypedSchema()
{
    static bool isTyped = _GetStaticTfType().IsA<UsdTyped>();
    return isTyped;
}

/* virtual */
const TfType &
UsdLuxPhotometricAreaLightAPI::_GetTfType() const
{
    return _GetStaticTfType();
}

UsdAttribute
UsdLuxPhotometricAreaLightAPI::GetPhotometricPowerAttr() const
{
    return GetPrim().GetAttribute(UsdLuxTokens->photometricPower);
}

UsdAttribute
UsdLuxPhotometricAreaLightAPI::CreatePhotometricPowerAttr(VtValue const &defaultValue, bool writeSparsely) const
{
    return UsdSchemaBase::_CreateAttr(UsdLuxTokens->photometricPower,
                       SdfValueTypeNames->Float,
                       /* custom = */ false,
                       SdfVariabilityVarying,
                       defaultValue,
                       writeSparsely);
}
UsdAttribute
UsdLuxPhotometricAreaLightAPI::GetPhotometricIlluminanceAttr() const
{
    return GetPrim().GetAttribute(UsdLuxTokens->photometricIlluminance);
}

UsdAttribute
UsdLuxPhotometricAreaLightAPI::CreatePhotometricIlluminanceAttr(VtValue const &defaultValue, bool writeSparsely) const
{
    return UsdSchemaBase::_CreateAttr(UsdLuxTokens->photometricIlluminance,
                       SdfValueTypeNames->Float,
                       /* custom = */ false,
                       SdfVariabilityVarying,
                       defaultValue,
                       writeSparsely);
}
UsdAttribute
UsdLuxPhotometricAreaLightAPI::GetPhotometricIlluminanceDistanceAttr() const
{
    return GetPrim().GetAttribute(UsdLuxTokens->photometricIlluminanceDistance);
}

UsdAttribute
UsdLuxPhotometricAreaLightAPI::CreatePhotometricIlluminanceDistanceAttr(VtValue const &defaultValue, bool writeSparsely) const
{
    return UsdSchemaBase::_CreateAttr(UsdLuxTokens->photometricIlluminanceDistance,
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
UsdLuxPhotometricAreaLightAPI::GetSchemaAttributeNames(bool includeInherited)
{
    static TfTokenVector localNames = {
        UsdLuxTokens->photometricPower,
        UsdLuxTokens->photometricIlluminance,
        UsdLuxTokens->photometricIlluminanceDistance,
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
