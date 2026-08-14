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

/// Local-space cylinder dimensions in scene units. Radius and length are
/// authored inputs expected to be positive and finite; behavior otherwise is
/// unspecified. The cylinder axis is local X.
struct CylinderLight
{
    float radius;
    float length;
};

/// Local-space disk radius in scene units. Radius is an authored input expected
/// to be positive and finite; behavior otherwise is unspecified. The emitting
/// face has local normal -Z.
struct DiskLight
{
    float radius;
};

/// Angular diameter of a distant emitter in degrees. Local +Z transformed to
/// world space points from the shading point toward the emitter, opposite its
/// local -Z emission axis.
struct DistantLight
{
    float angle = 0.53f;
};

// Needed for LightVariant
struct DomeLight
{};

/// Local-space rectangle dimensions in scene units. Width and height are
/// authored inputs expected to be positive and finite; behavior otherwise is
/// unspecified. The emitting face has local normal -Z.
struct RectLight
{
    float width;
    float height;
};

/// Local-space sphere radius in scene units. Radius is an authored input
/// expected to be positive and finite; behavior otherwise is unspecified.
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
    /// Row-major RGB texels in colorSpaceName; pixels has at least width *
    /// height entries when a texture is loaded.
    std::vector<GfVec3f> pixels;
    int width = 0;
    int height = 0;
    TfToken colorSpaceName;
    /// Luminance-solid-angle data built by the shared texture loader and
    /// consumed only by dome sampling. texelWeights has width * height entries,
    /// conditionalCdf has height * (width + 1), and marginalCdf has height + 1.
    /// Empty vectors and zero weightSum select uniform dome sampling.
    std::vector<float> texelWeights;
    std::vector<float> conditionalCdf;
    std::vector<float> marginalCdf;
    float weightSum = 0.0f;
};

/// Builds the sampling distribution for a lat-long dome texture.
///
/// Texels are weighted by luminance times their lat-long solid-angle measure.
/// A null texture is ignored. Invalid dimensions, pixel count, or total weight
/// clear all sampling arrays and leave weightSum zero.
void BuildDomeLightSamplingDistribution(
    LightTexture* texture);

struct IesShaping
{
    /// Parsed local-space IES profile. normalize divides evaluated intensity by
    /// iesFile.power(); angleScale is passed to PxrIESFile::eval unchanged.
    PxrIESFile iesFile;
    bool normalize = false;
    float angleScale = 0.0f;

    /// Photometric power treats the IES profile as luminous intensity. Finite
    /// emitters convert candela to luminance using their projected physical
    /// area; sceneUnitArea compensates UsdLux normalize handling.
    bool convertCandelaToLuminance = false;
    float metersPerUnit = 1.0f;
    float sceneUnitArea = 1.0f;
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
    // Normalized prefix CDF in row-major (theta, phi) cell order, with
    // NumRows() * NumPhi + 1 entries when valid.
    std::vector<float> cdf;
    // Piecewise-constant proposal density per cell in local sr^-1.
    std::vector<float> cellPdfSolidAngle;
    // Unnormalized solid-angle integral of the scalar proposal importance.
    float weightSum = 0.0f;
    // Mean and maximum scalar proposal importance over the sphere.
    float averageWeight = 1.0f;
    float peakWeight = 1.0f;
    // Solid-angle integral retained for emitted-power normalization.
    float totalSolidAngleWeightedIntensity = 0.0f;
    // Normalized first-moment direction in light-local space.
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
    /// Normalized direction in light-local space.
    GfVec3f dirLight = GfVec3f(0.0f, 0.0f, 1.0f);
    /// Probability density with respect to local solid angle, in sr^-1.
    float pdfSolidAngle = 0.0f;
    /// Non-negative scalar proposal importance at dirLight.
    float importance = 0.0f;
    /// True only when the direction, PDF, and importance form a usable sample.
    /// Callers may read no other field when false.
    bool valid = false;
};

struct Shaping
{
    /// UsdLux shaping inputs evaluated about local +Z. Angles are degrees;
    /// focusTint is authored RGB and the remaining scalar values are unitless.
    GfVec3f focusTint = GfVec3f(0.0f);
    float focus = 0.0f;
    float coneAngle = 180.0f;
    float coneSoftness = 0.0f;
    IesShaping ies;
    DirectionalShapingDistribution directionalDistribution;
};

/// Evaluate the RGB shaping multiplier for a light-local direction.
///
/// The direction need not be normalized. A zero or non-finite direction
/// returns zero.
GfVec3f EvaluateDirectionalShaping(
    Shaping const& shaping,
    GfVec3f const& dirLight);

/// Return the non-negative max-component proposal importance for the
/// light-local direction. Invalid directions return zero.
float DirectionalShapingImportance(
    Shaping const& shaping,
    GfVec3f const& dirLight);

/// Rebuild shaping.directionalDistribution from the authored shaping inputs.
///
/// A null pointer is ignored. No authored shaping, zero integrated importance,
/// or non-finite integrated importance leaves an invalid empty distribution.
void BuildDirectionalShapingDistribution(Shaping* shaping);

/// Sample the shaping proposal in light-local solid-angle measure.
///
/// u1 and u2 are expected in [0,1) and are clamped defensively. An invalid
/// distribution returns an invalid result whose other fields must not be read.
DirectionalShapingSample SampleDirectionalShaping(
    Shaping const& shaping,
    float u1,
    float u2);

/// Return the shaping proposal density in local sr^-1.
///
/// The direction need not be normalized. An invalid distribution or a zero or
/// non-finite direction returns zero.
float DirectionalShapingPdf(
    Shaping const& shaping,
    GfVec3f const& dirLight);

/// Renderer-facing light record owned at a stable address by HdEmbree_Light.
/// The delegate mutates it only after AcquireSceneForEdit() stops rendering;
/// light registries and render workers borrow it as immutable state.
struct LightData
{
    /// Finite, invertible affine transforms between light-local and world
    /// space. normalXformLightToWorld is the inverse-transpose direction
    /// transform.
    GfMatrix4f xformLightToWorld;
    GfMatrix3f normalXformLightToWorld;
    GfMatrix4f xformWorldToLight;
    /// Finite authored light color interpreted in the render color space.
    GfVec3f color;
    LightTexture texture;
    /// UsdLux radiometric controls. Intensity and diffuse are finite,
    /// non-negative linear multipliers, exposure is finite stops, and
    /// colorTemperature is positive finite kelvin; behavior outside these
    /// authored invariants is unspecified.
    float intensity = 1.0f;
    float diffuse = 1.0f;
    float exposure = 0.0f;
    float colorTemperature = 6500.0f;
    bool enableColorTemperature = false;
    /// Hydra-computed physical-light scale applied to emitted radiance.
    float physicalScale = 1.0f;
    /// Concrete light shape and its local-space authored parameters.
    LightVariant lightVariant;
    /// Apply UsdLux emitted-power normalization for the selected shape.
    bool normalize = false;
    /// Renderer and primary-camera visibility policy.
    bool visible = true;
    bool visibleInPrimaryRay = false;
    /// Authored Hydra collection categories used for light and shadow linking.
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
