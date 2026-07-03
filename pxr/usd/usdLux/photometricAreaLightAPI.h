//
// Copyright 2016 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef USDLUX_GENERATED_PHOTOMETRICAREALIGHTAPI_H
#define USDLUX_GENERATED_PHOTOMETRICAREALIGHTAPI_H

/// \file usdLux/photometricAreaLightAPI.h

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
// PHOTOMETRICAREALIGHTAPI                                                            //
// -------------------------------------------------------------------------- //

/// \class UsdLuxPhotometricAreaLightAPI
///
/// Photometric controls for area lights.
///
class UsdLuxPhotometricAreaLightAPI : public UsdAPISchemaBase
{
public:
    static const UsdSchemaKind schemaKind = UsdSchemaKind::SingleApplyAPI;

    explicit UsdLuxPhotometricAreaLightAPI(const UsdPrim& prim=UsdPrim())
        : UsdAPISchemaBase(prim)
    {
    }

    explicit UsdLuxPhotometricAreaLightAPI(const UsdSchemaBase& schemaObj)
        : UsdAPISchemaBase(schemaObj)
    {
    }

    USDLUX_API
    virtual ~UsdLuxPhotometricAreaLightAPI();

    USDLUX_API
    static const TfTokenVector &
    GetSchemaAttributeNames(bool includeInherited=true);

    USDLUX_API
    static UsdLuxPhotometricAreaLightAPI
    Get(const UsdStagePtr &stage, const SdfPath &path);

    USDLUX_API
    static bool
    CanApply(const UsdPrim &prim, std::string *whyNot=nullptr);

    USDLUX_API
    static UsdLuxPhotometricAreaLightAPI
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
    // PHOTOMETRIC_POWER
    // --------------------------------------------------------------------- //
    USDLUX_API
    UsdAttribute GetPhotometricPowerAttr() const;

    USDLUX_API
    UsdAttribute CreatePhotometricPowerAttr(VtValue const &defaultValue = VtValue(), bool writeSparsely=false) const;
public:
    // --------------------------------------------------------------------- //
    // PHOTOMETRIC_ILLUMINANCE
    // --------------------------------------------------------------------- //
    USDLUX_API
    UsdAttribute GetPhotometricIlluminanceAttr() const;

    USDLUX_API
    UsdAttribute CreatePhotometricIlluminanceAttr(VtValue const &defaultValue = VtValue(), bool writeSparsely=false) const;
public:
    // --------------------------------------------------------------------- //
    // PHOTOMETRIC_ILLUMINANCE_DISTANCE
    // --------------------------------------------------------------------- //
    USDLUX_API
    UsdAttribute GetPhotometricIlluminanceDistanceAttr() const;

    USDLUX_API
    UsdAttribute CreatePhotometricIlluminanceDistanceAttr(VtValue const &defaultValue = VtValue(), bool writeSparsely=false) const;

};

PXR_NAMESPACE_CLOSE_SCOPE

#endif
