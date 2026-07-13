//
// Copyright 2016 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_IMAGING_HD_LIGHT_H
#define PXR_IMAGING_HD_LIGHT_H

#include "pxr/pxr.h"
#include "pxr/imaging/hd/api.h"
#include "pxr/imaging/hd/version.h"
#include "pxr/imaging/hd/sprim.h"

#include "pxr/base/tf/staticTokens.h"

#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

#define HD_LIGHT_TOKENS                                     \
    (angle)                                                 \
    (color)                                                 \
    (colorTemperature)                                      \
    (enableColorTemperature)                                \
    (domeOffset)                                            \
    (exposure)                                              \
    (height)                                                \
    (intensity)                                             \
    (radius)                                                \
    (length)                                                \
    ((textureFile, "texture:file"))                         \
    ((textureFormat, "texture:format"))                     \
    (width)                                                 \
    (ambient)                                               \
    (diffuse)                                               \
    (specular)                                              \
    (normalize)                                             \
    (metersPerUnit)                                         \
    (hasShadow)                                             \
    ((shapingFocus, "shaping:focus"))                       \
    ((shapingFocusTint, "shaping:focusTint"))               \
    ((shapingConeAngle, "shaping:cone:angle"))              \
    ((shapingConeSoftness, "shaping:cone:softness"))        \
    ((shapingIesFile, "shaping:ies:file"))                  \
    ((shapingIesAngleScale, "shaping:ies:angleScale"))      \
    ((shapingIesNormalize, "shaping:ies:normalize"))        \
    ((photometricPower, "photometric:power"))                \
    ((photometricIlluminance, "photometric:illuminance"))    \
    ((photometricIlluminanceDistance,                        \
        "photometric:illuminance:distance"))                 \
    ((physicalIlluminant, "physical:illuminant"))            \
    ((physicalCustomIlluminant, "physical:customIlluminant")) \
    ((shadowEnable, "shadow:enable"))                       \
    ((shadowColor, "shadow:color"))                         \
    ((shadowDistance, "shadow:distance"))                   \
    ((shadowFalloff, "shadow:falloff"))                     \
    ((shadowFalloffGamma, "shadow:falloffGamma"))           \
                                                            \
    (params)                                                \
    (shadowCollection)                                      \
    (shadowParams)

TF_DECLARE_PUBLIC_TOKENS(HdLightTokens, HD_API, HD_LIGHT_TOKENS);

class HdSceneDelegate;
using HdLightPtrConstVector = std::vector<class HdLight const *>;

/// \class HdLight
///
/// A light model, used in conjunction with HdRenderPass.
///
class HdLight : public HdSprim
{
public:
    HD_API
    HdLight(SdfPath const & id);
    HD_API
    ~HdLight() override;

    // Change tracking for HdLight
    enum DirtyBits : HdDirtyBits {
        Clean                 = 0,
        DirtyTransform        = 1 << 0,
        // Note: Because DirtyVisibility wasn't added, DirtyParams does double
        //       duty for params and visibility.
        DirtyParams           = 1 << 1,
        DirtyShadowParams     = 1 << 2,
        DirtyCollection       = 1 << 3,
        DirtyResource         = 1 << 4,

        // XXX: This flag is important for instanced lights, and must have
        // the same value as it does for Rprims
        DirtyInstancer        = 1 << 16,
        AllDirty              = (DirtyTransform
                                 |DirtyParams
                                 |DirtyShadowParams
                                 |DirtyCollection
                                 |DirtyResource
                                 |DirtyInstancer)
    };

    HD_API
    static std::string StringifyDirtyBits(HdDirtyBits dirtyBits);

    /// Computes a scalar that converts the delegate's unnormalized emitted
    /// radiance/luminance into the value required by the UsdLux physical
    /// lighting APIs. If the light has no photometric parameters, this returns
    /// 1.0.
    /// TODO: Store lightType on HdLight itself so callers do not need to pass
    /// it here. That requires a broader Hydra API change because light type is
    /// currently only passed to render delegates during Sprim creation.
    HD_API
    static float ComputePhysicalScalingFactor(
        HdSceneDelegate* sceneDelegate,
        SdfPath const& id,
        TfToken const& lightType);

    /// Returns the reciprocal of the evaluated emission luminance from the
    /// standard light color/intensity/exposure/diffuse/color-temperature
    /// inputs. If that luminance is zero, this returns 0.
    HD_API
    static float EmissionLuminanceFactor(
        HdSceneDelegate* sceneDelegate,
        SdfPath const& id,
        TfToken const& lightType);

    /// Returns the geometry factor used by photometric:power on area lights.
    HD_API
    static float AreaLightPowerFactor(
        HdSceneDelegate* sceneDelegate,
        SdfPath const& id,
        TfToken const& lightType);

    /// Returns the projected-solid-angle factor used by
    /// photometric:illuminance on area lights.
    HD_API
    static float AreaLightIlluminanceFactor(
        HdSceneDelegate* sceneDelegate,
        SdfPath const& id,
        TfToken const& lightType);

    /// Returns the cone-size factor used by photometric:illuminance on distant
    /// lights.
    HD_API
    static float DistantLightIlluminanceFactor(
        HdSceneDelegate* sceneDelegate,
        SdfPath const& id);

    /// Returns the reciprocal mean luminance factor for a rect texture.
    /// Textureless rect lights and unreadable textures return 1.0.
    HD_API
    static float RectTextureLuminanceFactor(
        HdSceneDelegate* sceneDelegate,
        SdfPath const& id);

    /// Returns the reciprocal upper-hemisphere illuminance factor for a dome
    /// texture. Textureless domes use the constant-color value 1 / pi.
    HD_API
    static float DomeTextureIlluminanceFactor(
        HdSceneDelegate* sceneDelegate,
        SdfPath const& id);

    /// Returns the IES profile power compensation factor. If there is no valid
    /// IES profile, or the profile is already normalized by the delegate's IES
    /// evaluator, this returns 1.0.
    HD_API
    static float IesPowerFactor(
        HdSceneDelegate* sceneDelegate,
        SdfPath const& id);

    /// Returns the identifier of the instancer (if any) for this Sprim. If this
    /// Sprim is not instanced, an empty SdfPath will be returned.
    const SdfPath& GetInstancerId() const { return _instancerId; }

    HD_API
    void _UpdateInstancer(
        HdSceneDelegate* sceneDelegate,
        HdDirtyBits* dirtyBits);

private:
    SdfPath _instancerId;
};

PXR_NAMESPACE_CLOSE_SCOPE

#endif  // PXR_IMAGING_HD_LIGHT_H
