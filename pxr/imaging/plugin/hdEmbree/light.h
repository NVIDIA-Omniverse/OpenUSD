//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_LIGHT_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_LIGHT_H

#include "pxr/base/gf/vec3f.h"
#include "pxr/base/gf/vec3i.h"
#include "pxr/base/gf/matrix3f.h"
#include "pxr/base/gf/matrix4f.h"
#include "pxr/base/tf/token.h"
#include "pxr/imaging/hd/light.h"
#include "pxr/imaging/plugin/hdEmbree/pxrIES/pxrIES.h"

#include <embree4/rtcore_common.h>
#include <embree4/rtcore_geometry.h>

#include <limits>
#include <variant>
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


struct HdEmbree_UnknownLight
{};
struct HdEmbree_Cylinder
{
    float radius;
    float length;
};

struct HdEmbree_Disk
{
    float radius;
};

struct HdEmbree_Distant
{
    float angle = 0.53f;
};

// Needed for HdEmbree_LightVariant
struct HdEmbree_Dome
{};

struct HdEmbree_Rect
{
    float width;
    float height;
};

struct HdEmbree_Sphere
{
    float radius;
};

using HdEmbree_LightVariant = std::variant<
    HdEmbree_UnknownLight,
    HdEmbree_Cylinder,
    HdEmbree_Disk,
    HdEmbree_Distant,
    HdEmbree_Dome,
    HdEmbree_Rect,
    HdEmbree_Sphere>;

struct HdEmbree_LightTexture
{
    std::vector<GfVec3f> pixels;
    int width = 0;
    int height = 0;
    TfToken colorSpaceName;
    std::vector<float> texelWeights;
    std::vector<float> conditionalCdf;
    std::vector<float> marginalCdf;
    float weightSum = 0.0f;
};

/// Builds the sampling distribution for a lat-long dome texture.
///
/// Texels are weighted by luminance times their lat-long solid-angle measure.
void HdEmbreeBuildDomeLightSamplingDistribution(
    HdEmbree_LightTexture* texture);

struct HdEmbree_IES
{
    PxrIESFile iesFile;
    bool normalize = false;
    float angleScale = 0.0f;
};

struct HdEmbree_DirectionalShapingDistribution
{
    static constexpr int NumPhi = 64;
    static constexpr int NumBaseTheta = 32;

    // Theta row boundaries stored as cos(theta), strictly descending from 1
    // to -1. Rows are the uniform base partition plus the exact angles where
    // shaping features have structure (cone edges, IES vertical knots), so
    // features narrower than a base row always span whole cells.
    std::vector<float> rowCosThetaBounds;
    std::vector<float> cdf;
    std::vector<float> cellPdfW;
    float weightSum = 0.0f;
    float averageWeight = 1.0f;
    float peakWeight = 1.0f;
    float totalSolidAngleWeightedIntensity = 0.0f;
    GfVec3f principalDirection = GfVec3f(0.0f, 0.0f, 1.0f);

    int NumRows() const
    {
        return rowCosThetaBounds.size() < 2
            ? 0
            : static_cast<int>(rowCosThetaBounds.size()) - 1;
    }

    bool IsValid() const
    {
        const size_t numCells =
            static_cast<size_t>(NumRows()) * static_cast<size_t>(NumPhi);
        return numCells > 0 &&
               cdf.size() == numCells + 1 &&
               cellPdfW.size() == numCells &&
               weightSum > 0.0f;
    }
};

struct HdEmbree_DirectionalShapingSample
{
    GfVec3f localDirection = GfVec3f(0.0f, 0.0f, 1.0f);
    float pdfW = 0.0f;
    float importance = 0.0f;
    bool valid = false;
};

struct HdEmbree_Shaping
{
    GfVec3f focusTint = GfVec3f(0.0f);
    float focus = 0.0f;
    float coneAngle = 180.0f;
    float coneSoftness = 0.0f;
    HdEmbree_IES ies;
    HdEmbree_DirectionalShapingDistribution directionalDistribution;
};

GfVec3f HdEmbreeEvaluateDirectionalShaping(
    HdEmbree_Shaping const& shaping,
    GfVec3f const& localDirection);

float HdEmbreeDirectionalShapingImportance(
    HdEmbree_Shaping const& shaping,
    GfVec3f const& localDirection);

void HdEmbreeBuildDirectionalShapingDistribution(HdEmbree_Shaping* shaping);

HdEmbree_DirectionalShapingSample HdEmbreeSampleDirectionalShaping(
    HdEmbree_Shaping const& shaping,
    float u1,
    float u2);

float HdEmbreeDirectionalShapingPdf(
    HdEmbree_Shaping const& shaping,
    GfVec3f const& localDirection);

struct HdEmbree_LightData
{
    GfMatrix4f xformLightToWorld;
    GfMatrix3f normalXformLightToWorld;
    GfMatrix4f xformWorldToLight;
    GfVec3f color;
    HdEmbree_LightTexture texture;
    float intensity = 1.0f;
    float diffuse = 1.0f;
    float exposure = 0.0f;
    float colorTemperature = 6500.0f;
    bool enableColorTemperature = false;
    HdEmbree_LightVariant lightVariant;
    bool normalize = false;
    bool visible = true;
    bool visibleInPrimaryRay = false;
    TfToken lightLink;
    TfToken shadowLink;
    HdEmbree_Shaping shaping;
};

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
