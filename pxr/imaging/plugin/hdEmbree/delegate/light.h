//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_LIGHT_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_LIGHT_H

#include <renderer/lights/light.h>

#include "pxr/imaging/hd/light.h"

#include <embree4/rtcore_common.h>
#include <embree4/rtcore_geometry.h>

#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

class HdEmbreeRenderer;

/// Reference implementation of USD Lux support, for the hdEmbree renderer.
///
/// This is a reference implementation of USD Lux support, for the hdEmbree
/// renderer. It is not intented for use in production, but instead as useful
/// reference for understanding how to implement USD Lux support for other
/// renderers.
///
/// Supported Features:
/// - LightAPI:
///   - inputs:intensity
///   - inputs:exposure
///   - inputs:diffuse
///   - inputs:normalize
///   - inputs:color
///   - inputs:enableColorTemperature
///   - inputs:colorTemperature
/// - DiskLight
///   - inputs:radius
/// - RectLight
///   - inputs:width
///   - inputs:height
///   - inputs:texture:file
/// - SphereLight
///   - inputs:radius
/// - CylinderLight
///   - inputs:radius
///   - inputs:length
/// - DomeLight
///   - inputs:texture:file
/// - DistantLight
///   - inputs:angle
/// - ShapingAPI
///   - inputs:shaping:focus
///   - inputs:shaping:focusTint
///   - inputs:shaping:cone:angle
///   - inputs:shaping:cone:softness
///   - inputs:shaping:ies:file
///   - inputs:shaping:ies:angleScale
///   - inputs:shaping:ies:normalize
/// - hdEmbree renderer-specific attributes:
///   - visibleInPrimaryRay
/// - Respects double-sidedness of meshes
///
/// Currently Unsupported Features / Limitations:
/// - Surface shaders (all surfaces are assumed to be 100% reflective diffuse
///   BRDFs)
/// - Light shaders
/// - Unsupported light types:
///   - MeshLightAPI
///   - VolumeLightAPI
///   - GeometryLight
///   - PortalLight
///   - PluginLight
/// - Unsupported UsdLux APIS:
///   - LightListAPI
///   - ListAPI
///   - ShadowAPI (shadows are rendered, but are unaffected by this API)
///   - LightFilter
/// - Unsupported attributes on supported light types / APIs:
///   - LightAPI:
///     - light:shaderId
///     - light:materialSyncMode
///     - inputs:specular
///     - light:filters
///   - SphereLight:
///     - treatAsPoint
///   - CylinderLight:
///     - inputs:treatAsLine
///   - DomeLight:
///     - inputs:texture:format (always assumed to be "latlong")
/// - No support for motion blur (currently, if motion blur is enabled, all
///   samples taken at the first time sample, ie, when the shutter opens).
/// - No support for instanced lights

/// Known Bugs / Issues:
/// - if animating the ies:file property, and an ies file is removed (ie,
///   set to blank), no update is registered, and the ies file will continue to
///   be used


class HdEmbree_Light final : public HdLight
{
public:
    HdEmbree_Light(SdfPath const& id, TfToken const& lightType);
    ~HdEmbree_Light();

    /// Synchronizes state from the delegate to this object.
    void Sync(HdSceneDelegate* sceneDelegate,
              HdRenderParam* renderParam,
              HdDirtyBits* dirtyBits) override;

    /// Returns the minimal set of dirty bits to place in the
    /// change tracker for use in the first sync of this prim.
    /// Typically this would be all dirty bits.
    HdDirtyBits GetInitialDirtyBitsMask() const override;

    void Finalize(HdRenderParam *renderParam) override;

    HdEmbree_LightData const& LightData() const {
        return _lightData;
    }

    bool IsDome() const {
        return std::holds_alternative<HdEmbree_Dome>(_lightData.lightVariant);
    }

    bool IsFiniteLight() const {
        return std::holds_alternative<HdEmbree_Cylinder>(
                   _lightData.lightVariant) ||
               std::holds_alternative<HdEmbree_Disk>(
                   _lightData.lightVariant) ||
               std::holds_alternative<HdEmbree_Rect>(
                   _lightData.lightVariant) ||
               std::holds_alternative<HdEmbree_Sphere>(
                   _lightData.lightVariant);
    }

private:
    void _UpdateVisibleGeometry(
        RTCScene scene, RTCDevice device, HdEmbreeRenderer* renderer);
    void _ReleaseVisibleGeometry(
        RTCScene scene, HdEmbreeRenderer* renderer);

    HdEmbree_LightData _lightData;
    RTCGeometry _rtcVisibleGeometry = nullptr;
    unsigned int _rtcVisibleGeometryId = RTC_INVALID_GEOMETRY_ID;
    std::vector<GfVec3f> _rtcVisiblePoints;
    std::vector<GfVec3i> _rtcVisibleTriangles;
};


PXR_NAMESPACE_CLOSE_SCOPE

#endif
