//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_RENDERER_LIGHT_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_RENDERER_LIGHT_H

#include <renderer/lights/pxrIES/pxrIES.h>

#include "pxr/base/gf/matrix3f.h"
#include "pxr/base/gf/matrix4f.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/tf/token.h"
#include "pxr/pxr.h"

#include <limits>
#include <variant>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE
namespace ty {

struct UnknownLight
{};
struct CylinderLight
{
    float radius;
    float length;
};

struct DiskLight
{
    float radius;
};

struct DistantLight
{
    float angle = 0.53f;
};

// Needed for LightVariant
struct DomeLight
{};

struct RectLight
{
    float width;
    float height;
};

struct SphereLight
{
    float radius;
};

using LightVariant = std::variant<
    UnknownLight,
    CylinderLight,
    DiskLight,
    DistantLight,
    DomeLight,
    RectLight,
    SphereLight>;

struct LightTexture
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
void BuildDomeLightSamplingDistribution(
    LightTexture* texture);

struct IesShaping
{
    PxrIESFile iesFile;
    bool normalize = false;
    float angleScale = 0.0f;
};

struct DirectionalShapingDistribution
{
    static constexpr int NumPhi = 64;
    static constexpr int NumBaseTheta = 32;

    // Theta row boundaries stored as cos(theta), strictly descending from 1
    // to -1. Rows are the uniform base partition plus the exact angles where
    // shaping features have structure (cone edges, IES vertical knots), so
    // features narrower than a base row always span whole cells.
    std::vector<float> rowCosThetaBounds;
    std::vector<float> cdf;
    std::vector<float> cellPdfSolidAngle;
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
        return numCells > 0 && cdf.size() == numCells + 1 &&
               cellPdfSolidAngle.size() == numCells && weightSum > 0.0f;
    }
};

struct DirectionalShapingSample
{
    GfVec3f localDirection = GfVec3f(0.0f, 0.0f, 1.0f);
    float pdfSolidAngle = 0.0f;
    float importance = 0.0f;
    bool valid = false;
};

struct Shaping
{
    GfVec3f focusTint = GfVec3f(0.0f);
    float focus = 0.0f;
    float coneAngle = 180.0f;
    float coneSoftness = 0.0f;
    IesShaping ies;
    DirectionalShapingDistribution directionalDistribution;
};

GfVec3f EvaluateDirectionalShaping(
    Shaping const& shaping,
    GfVec3f const& localDirection);

float DirectionalShapingImportance(
    Shaping const& shaping,
    GfVec3f const& localDirection);

void BuildDirectionalShapingDistribution(Shaping* shaping);

DirectionalShapingSample SampleDirectionalShaping(
    Shaping const& shaping,
    float u1,
    float u2);

float DirectionalShapingPdf(
    Shaping const& shaping,
    GfVec3f const& localDirection);

struct LightData
{
    GfMatrix4f xformLightToWorld;
    GfMatrix3f normalXformLightToWorld;
    GfMatrix4f xformWorldToLight;
    GfVec3f color;
    LightTexture texture;
    float intensity = 1.0f;
    float diffuse = 1.0f;
    float exposure = 0.0f;
    float colorTemperature = 6500.0f;
    bool enableColorTemperature = false;
    LightVariant lightVariant;
    bool normalize = false;
    bool visible = true;
    bool visibleInPrimaryRay = false;
    TfToken lightLink;
    TfToken shadowLink;
    Shaping shaping;

    bool IsDome() const {
        return std::holds_alternative<DomeLight>(lightVariant);
    }

    bool IsFiniteLight() const {
        return std::holds_alternative<CylinderLight>(lightVariant) ||
               std::holds_alternative<DiskLight>(lightVariant) ||
               std::holds_alternative<RectLight>(lightVariant) ||
               std::holds_alternative<SphereLight>(lightVariant);
    }
};

} // namespace ty
PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_IMAGING_PLUGIN_HD_EMBREE_RENDERER_LIGHT_H
