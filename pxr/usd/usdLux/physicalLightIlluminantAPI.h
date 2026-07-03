//
// Copyright 2016 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef USDLUX_GENERATED_PHYSICALLIGHTILLUMINANTAPI_H
#define USDLUX_GENERATED_PHYSICALLIGHTILLUMINANTAPI_H

/// \file usdLux/physicalLightIlluminantAPI.h

#include "pxr/pxr.h"
#include "pxr/usd/usdLux/api.h"
#include "pxr/usd/usd/apiSchemaBase.h"
#include "pxr/usd/usd/prim.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usdLux/tokens.h"

#include "pxr/base/vt/value.h"

#include "pxr/base/tf/token.h"
#include "pxr/base/tf/type.h"

PXR_NAMESPACE_OPEN_SCOPE

class SdfAssetPath;

// -------------------------------------------------------------------------- //
// PHYSICALLIGHTILLUMINANTAPI                                                            //
// -------------------------------------------------------------------------- //

/// \class UsdLuxPhysicalLightIlluminantAPI
///
/// Controls the illuminant spectrum used by physical light APIs.
///
class UsdLuxPhysicalLightIlluminantAPI : public UsdAPISchemaBase
{
public:
    static const UsdSchemaKind schemaKind = UsdSchemaKind::SingleApplyAPI;

    explicit UsdLuxPhysicalLightIlluminantAPI(const UsdPrim& prim=UsdPrim())
        : UsdAPISchemaBase(prim)
    {
    }

    explicit UsdLuxPhysicalLightIlluminantAPI(const UsdSchemaBase& schemaObj)
        : UsdAPISchemaBase(schemaObj)
    {
    }

    USDLUX_API
    virtual ~UsdLuxPhysicalLightIlluminantAPI();

    USDLUX_API
    static const TfTokenVector &
    GetSchemaAttributeNames(bool includeInherited=true);

    USDLUX_API
    static UsdLuxPhysicalLightIlluminantAPI
    Get(const UsdStagePtr &stage, const SdfPath &path);

    USDLUX_API
    static bool
    CanApply(const UsdPrim &prim, std::string *whyNot=nullptr);

    USDLUX_API
    static UsdLuxPhysicalLightIlluminantAPI
    Apply(const UsdPrim &prim);

protected:
    USDLUX_API
    UsdSchemaKind _GetSchemaKind() const override;

private:
    friend class UsdSchemaRegistry;
    USDLUX_API
    static const TfType &_GetStaticTfType();

    static bool _IsTypedSchema();

    USDLUX_API
    const TfType &_GetTfType() const override;

public:
    // --------------------------------------------------------------------- //
    // PHYSICAL_ILLUMINANT
    // --------------------------------------------------------------------- //
    USDLUX_API
    UsdAttribute GetPhysicalIlluminantAttr() const;

    USDLUX_API
    UsdAttribute CreatePhysicalIlluminantAttr(VtValue const &defaultValue = VtValue(), bool writeSparsely=false) const;
public:
    // --------------------------------------------------------------------- //
    // PHYSICAL_CUSTOM_ILLUMINANT
    // --------------------------------------------------------------------- //
    USDLUX_API
    UsdAttribute GetPhysicalCustomIlluminantAttr() const;

    USDLUX_API
    UsdAttribute CreatePhysicalCustomIlluminantAttr(VtValue const &defaultValue = VtValue(), bool writeSparsely=false) const;

};

PXR_NAMESPACE_CLOSE_SCOPE

#endif
