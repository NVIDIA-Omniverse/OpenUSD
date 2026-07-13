//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_RENDERER_LIGHT_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_RENDERER_LIGHT_H

#include "pxr/pxr.h"
#include "pxr/base/gf/matrix3f.h"
#include "pxr/base/gf/matrix4f.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/tf/token.h"
#include "pxr/imaging/plugin/hdEmbree/renderer/lights/pxrIES/pxrIES.h"

#include <limits>
#include <variant>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

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

    bool IsDome() const {
        return std::holds_alternative<HdEmbree_Dome>(lightVariant);
    }

    bool IsFiniteLight() const {
        return std::holds_alternative<HdEmbree_Cylinder>(lightVariant) ||
               std::holds_alternative<HdEmbree_Disk>(lightVariant) ||
               std::holds_alternative<HdEmbree_Rect>(lightVariant) ||
               std::holds_alternative<HdEmbree_Sphere>(lightVariant);
    }
};

PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_IMAGING_PLUGIN_HD_EMBREE_RENDERER_LIGHT_H
