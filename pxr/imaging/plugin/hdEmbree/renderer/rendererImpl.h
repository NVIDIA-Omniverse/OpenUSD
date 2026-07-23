//
// Internal implementation shared by HdEmbreeRenderer translation units.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_RENDERER_IMPL_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_RENDERER_IMPL_H

#include "pxr/imaging/plugin/hdEmbree/renderer/renderer.h"
#include "pxr/imaging/plugin/hdEmbree/renderer/config.h"
#include "pxr/imaging/plugin/hdEmbree/renderer/materials/oiioTextureSystem.h"
#include "pxr/imaging/plugin/hdEmbree/renderer/integrator/sss.h"
#include "pxr/imaging/plugin/hdEmbree/renderer/materials/MaterialXCpp/graph.h"
#include "pxr/imaging/plugin/hdEmbree/renderer/materials/MaterialXCpp/materials/adobeOpenPbr.h"
#include "pxr/imaging/plugin/hdEmbree/renderer/materials/MaterialXCpp/materials/bsdf.h"
#include "pxr/imaging/plugin/hdEmbree/renderer/materials/MaterialXCpp/shadingContext.h"
#include "pxr/imaging/plugin/hdEmbree/renderer/materials/MaterialXCpp/spectral.h"

#include "pxr/imaging/hd/meshUtil.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/plugin/hdEmbree/renderer/geometry/displacementEvaluation.h"
#include "pxr/imaging/plugin/hdEmbree/renderer/geometry/meshSamplers.h"
#include "pxr/imaging/plugin/hdEmbree/renderer/geometry/primvarSampling.h"

#include "pxr/base/gf/matrix3f.h"
#include "pxr/base/tf/hash.h"

#include <embree4/rtcore_common.h>
#include <embree4/rtcore_geometry.h>
#include <embree4/rtcore_scene.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <variant>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

PXR_NAMESPACE_USING_DIRECTIVE

// -------------------------------------------------------------------------
// Constants
// -------------------------------------------------------------------------

template <typename T>
constexpr T _pi = static_cast<T>(M_PI);

constexpr float _rayHitContinueBias = 0.001f;
constexpr float _minLuminanceCutoff = 1e-9f;
constexpr float _volumePdfEps = 1.0e-20f;
static const TfToken _tokensTangent("tangent");
static const TfToken _tokensBitangent("bitangent");
static const TfToken _tokensComputedTangent("hdEmbreeComputedTangent");
static const TfToken _tokensComputedBitangent("hdEmbreeComputedBitangent");
static const TfToken _tokensSt("st");
static const TfToken _tokensDielectricLayerThroughputModeBsdl(
    "bsdl", TfToken::Immortal);
static const TfToken _tokensDielectricLayerThroughputModeMaterialXGlsl(
    "materialxGlsl", TfToken::Immortal);

inline bool
_IsFinite(GfVec3f const& value)
{
    return std::isfinite(value[0]) &&
           std::isfinite(value[1]) &&
           std::isfinite(value[2]);
}

inline bool
_TryNormalizeDirection(
    GfVec3f const& value,
    GfVec3f* normalized,
    double* length = nullptr)
{
    if (!normalized || !_IsFinite(value)) {
        return false;
    }

    const double maximumComponent = std::max({
        std::abs(static_cast<double>(value[0])),
        std::abs(static_cast<double>(value[1])),
        std::abs(static_cast<double>(value[2]))});
    if (!std::isfinite(maximumComponent) || maximumComponent == 0.0) {
        return false;
    }

    const double x = static_cast<double>(value[0]) / maximumComponent;
    const double y = static_cast<double>(value[1]) / maximumComponent;
    const double z = static_cast<double>(value[2]) / maximumComponent;
    const double scaledLength = std::sqrt(x * x + y * y + z * z);
    if (!std::isfinite(scaledLength) || scaledLength == 0.0) {
        return false;
    }

    *normalized = GfVec3f(
        static_cast<float>(x / scaledLength),
        static_cast<float>(y / scaledLength),
        static_cast<float>(z / scaledLength));
    if (length) {
        *length = maximumComponent * scaledLength;
    }
    return _IsFinite(*normalized);
}

inline bool
_TryBuildSurfaceNormal(
    GfVec3f const& dPdu,
    GfVec3f const& dPdv,
    GfVec3f* normal)
{
    GfVec3f normalizedU;
    GfVec3f normalizedV;
    if (!normal ||
        !_TryNormalizeDirection(dPdu, &normalizedU) ||
        !_TryNormalizeDirection(dPdv, &normalizedV)) {
        return false;
    }

    const GfVec3f relativeArea = GfCross(normalizedU, normalizedV);
    const float relativeAreaSquared = relativeArea.GetLengthSq();
    return std::isfinite(relativeAreaSquared) &&
        relativeAreaSquared > 1.0e-18f &&
        _TryNormalizeDirection(relativeArea, normal);
}

inline bool
_IsCameraDepthOfFieldEnabled(HdEmbreeCameraDepthOfField const& dof,
                             bool isOrthographic)
{
    return !isOrthographic &&
           std::isfinite(dof.fStop) &&
           std::isfinite(dof.focusDistance) &&
           std::isfinite(dof.focalLength) &&
           dof.fStop > 0.0f &&
           dof.focusDistance > 0.0f &&
           dof.focalLength > 0.0f;
}

inline float
_GetLensRadius(HdEmbreeCameraDepthOfField const& dof)
{
    return dof.focalLength / (2.0f * dof.fStop);
}

inline GfVec2f
_SampleUniformDiskConcentric(GfVec2f const& sample)
{
    const float x = 2.0f * sample[0] - 1.0f;
    const float y = 2.0f * sample[1] - 1.0f;

    if (x == 0.0f && y == 0.0f) {
        return GfVec2f(0.0f);
    }

    float r;
    float theta;
    if (std::abs(x) > std::abs(y)) {
        r = x;
        theta = (_pi<float> / 4.0f) * (y / x);
    } else {
        r = y;
        theta = (_pi<float> / 2.0f) -
                (_pi<float> / 4.0f) * (x / y);
    }

    return GfVec2f(r * std::cos(theta), r * std::sin(theta));
}

inline bool
_ApplyCameraDepthOfField(HdEmbreeCameraDepthOfField const& dof,
                         GfVec2f const& lensPoint,
                         GfVec3f *origin,
                         GfVec3f *dir)
{
    constexpr float eps = 1.0e-7f;

    if (!origin || !dir || !_IsFinite(*origin) || !_IsFinite(*dir)) {
        return false;
    }

    const float dz = (*dir)[2];
    if (!std::isfinite(dz) || std::abs(dz) < eps) {
        return false;
    }

    const float focusT = -dof.focusDistance / dz;
    if (!std::isfinite(focusT) || focusT <= 0.0f) {
        return false;
    }

    const GfVec3f focusPoint = *origin + (*dir) * focusT;
    const GfVec3f lensOrigin(lensPoint[0], lensPoint[1], 0.0f);
    const GfVec3f dofDir = focusPoint - lensOrigin;
    if (!_IsFinite(dofDir) || dofDir.GetLengthSq() <= eps * eps) {
        return false;
    }

    *origin = lensOrigin;
    *dir = dofDir;
    return true;
}

// -------------------------------------------------------------------------
// General Ray Utilities
// -------------------------------------------------------------------------

inline GfVec3f
_CalculateHitPosition(RTCRayHit const& rayHit)
{
    return GfVec3f(rayHit.ray.org_x + rayHit.ray.tfar * rayHit.ray.dir_x,
                   rayHit.ray.org_y + rayHit.ray.tfar * rayHit.ray.dir_y,
                   rayHit.ray.org_z + rayHit.ray.tfar * rayHit.ray.dir_z);
}

/// Transform a semantic object-space normal to world space.
///
/// Normals are covectors, so non-uniform transforms require the inverse
/// transpose rather than the object-to-world linear transform. Reflections do
/// not negate a semantic USD normal; Hydra accounts for winding reversal
/// separately when it classifies faces.
inline GfVec3f
_TransformNormalToWorld(
    GfMatrix4f const& worldToObjectMatrix,
    GfVec3f const& objectNormal)
{
    GfVec3f worldNormal =
        worldToObjectMatrix.GetTranspose().TransformDir(objectNormal);

    GfVec3f normalizedWorldNormal;
    if (!_TryNormalizeDirection(worldNormal, &normalizedWorldNormal)) {
        // A singular transform has no mathematically unique world normal.
        // Preserve a valid authored/geometric direction rather than replacing
        // it with an unrelated axis.
        if (_TryNormalizeDirection(objectNormal, &normalizedWorldNormal)) {
            return normalizedWorldNormal;
        }
        return GfVec3f(0.0f, 0.0f, 1.0f);
    }
    return normalizedWorldNormal;
}

inline GfVec3f
_TransformNormalToWorld(
    HdEmbreeInstanceContext const* instanceContext,
    GfVec3f const& objectNormal)
{
    if (!instanceContext) {
        return objectNormal;
    }
    return _TransformNormalToWorld(
        instanceContext->worldToObjectMatrix,
        objectNormal);
}

/// Recover the oriented object-space normal corresponding to a world-space
/// normal. This is the inverse direction of _TransformNormalToWorld and keeps
/// a face-forward sign applied by the caller.
inline GfVec3f
_TransformNormalToObject(
    GfMatrix4f const& objectToWorldMatrix,
    GfVec3f const& worldNormal)
{
    GfVec3f objectNormal =
        objectToWorldMatrix.GetTranspose().TransformDir(worldNormal);

    GfVec3f normalizedObjectNormal;
    if (!_TryNormalizeDirection(objectNormal, &normalizedObjectNormal)) {
        if (_TryNormalizeDirection(worldNormal, &normalizedObjectNormal)) {
            return normalizedObjectNormal;
        }
        return GfVec3f(0.0f, 0.0f, 1.0f);
    }
    return normalizedObjectNormal;
}

inline GfVec3f
_TransformNormalToObject(
    HdEmbreeInstanceContext const* instanceContext,
    GfVec3f const& worldNormal)
{
    if (!instanceContext) {
        return worldNormal;
    }
    return _TransformNormalToObject(
        instanceContext->objectToWorldMatrix, worldNormal);
}

/// Transform the derivative of a normalized normal through an instance.
/// Applying the inverse transpose to dN alone is insufficient under
/// non-uniform scale: the result must also include the derivative of the
/// normalization operation.
inline GfVec3f
_TransformNormalDerivativeToWorld(
    HdEmbreeInstanceContext const* instanceContext,
    GfVec3f const& objectNormal,
    GfVec3f const& objectNormalDerivative)
{
    if (!instanceContext) {
        return objectNormalDerivative;
    }

    const GfMatrix4f normalTransform =
        instanceContext->worldToObjectMatrix.GetTranspose();
    GfVec3f transformedNormal =
        normalTransform.TransformDir(objectNormal);
    GfVec3f transformedDerivative =
        normalTransform.TransformDir(objectNormalDerivative);

    GfVec3f worldNormal;
    double normalLength = 0.0;
    if (!_IsFinite(transformedDerivative) ||
        !_TryNormalizeDirection(
            transformedNormal, &worldNormal, &normalLength) ||
        !std::isfinite(normalLength) || normalLength == 0.0) {
        return GfVec3f(0.0f);
    }

    const double projection =
        static_cast<double>(worldNormal[0]) * transformedDerivative[0] +
        static_cast<double>(worldNormal[1]) * transformedDerivative[1] +
        static_cast<double>(worldNormal[2]) * transformedDerivative[2];
    const GfVec3f worldDerivative(
        static_cast<float>(
            (transformedDerivative[0] - worldNormal[0] * projection) /
            normalLength),
        static_cast<float>(
            (transformedDerivative[1] - worldNormal[1] * projection) /
            normalLength),
        static_cast<float>(
            (transformedDerivative[2] - worldNormal[2] * projection) /
            normalLength));
    return _IsFinite(worldDerivative) ? worldDerivative : GfVec3f(0.0f);
}

// Dot product, but set to 0 if less than 0 - ie, 0 for backward-facing rays
inline float
_DotZeroClip(GfVec3f const& a, GfVec3f const& b)
{
    return std::max(0.0f, GfDot(a, b));
}

}  // anonymous namespace

// Conversion helpers between GfVec3f and mxcpp::Vec3f (Imath::V3f).
// GfVec3f in this build does not have implicit Imath conversion.
namespace {

/// Numerically stable computation of a*b - c*d using Kahan compensation.
inline float
_DifferenceOfProducts(float a, float b, float c, float d)
{
    float cd = c * d;
    float err = std::fma(-c, d, cd);
    float dop = std::fma(a, b, -cd);
    return dop + err;
}

inline GfVec3f
_ToGf(const mxcpp::Vec3f& v)
{
    return GfVec3f(v[0], v[1], v[2]);
}

inline mxcpp::Vec3f
_ToMx(const GfVec3f& v)
{
    return mxcpp::Vec3f(v[0], v[1], v[2]);
}

inline mxcpp::Vec2f
_ToMx(const GfVec2f& v)
{
    return mxcpp::Vec2f(v[0], v[1]);
}

inline mxcpp::Mat4f
_ToMx(const GfMatrix4f& m)
{
    mxcpp::Mat4f result;
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            result[row][col] = m[row][col];
        }
    }
    return result;
}

inline bool
_IsNearlyBlack(const GfVec3f& value, float threshold = 1.0e-4f)
{
    return value[0] <= threshold &&
           value[1] <= threshold &&
           value[2] <= threshold;
}

inline GfVec3f
_ClampFireflyContribution(GfVec3f contribution, float threshold)
{
    if (threshold <= 0.0f) {
        return contribution;
    }

    const float luminance = 0.2126f * contribution[0]
                          + 0.7152f * contribution[1]
                          + 0.0722f * contribution[2];
    if (luminance > threshold) {
        contribution *= threshold / luminance;
    }
    return contribution;
}

inline float
_GetMultiSampleMisLightPdf(float lightPdf, int sampleCount)
{
    if (lightPdf <= 0.0f) {
        return 0.0f;
    }
    return lightPdf * static_cast<float>(std::max(1, sampleCount));
}

constexpr float _reflectionOnlyEps = 1.0e-6f;

inline bool
_IsEffectivelyZero(float value)
{
    return value <= _reflectionOnlyEps;
}

inline bool
_IsEffectivelyOpaque(float value)
{
    return value >= 1.0f - _reflectionOnlyEps;
}

inline float
_Clamp01(float value)
{
    return std::clamp(value, 0.0f, 1.0f);
}

inline GfVec3f
_Clamp01(GfVec3f const& value)
{
    return GfVec3f(
        _Clamp01(value[0]),
        _Clamp01(value[1]),
        _Clamp01(value[2]));
}

inline GfVec3f
_TransparentShadowTransmission(
    mxcpp::SurfaceClosure const& closure,
    GfVec3f const& direction,
    GfVec3f const& hitNormal,
    bool includeSurfaceTint)
{
    const float transmission = _Clamp01(closure.transmission);
    if (transmission <= 0.0f) {
        return GfVec3f(0.0f);
    }

    const float interfaceTransmission =
        mxcpp::Bsdf::StraightShadowDielectricTransmission(
            closure, GfDot(direction, hitNormal));
    GfVec3f attenuation(transmission * interfaceTransmission);
    if (includeSurfaceTint) {
        attenuation = GfCompMult(
            attenuation,
            _Clamp01(_ToGf(closure.transmissionColor)));
    }
    return _Clamp01(attenuation);
}

inline GfVec3f
_CombinePresenceAndTransmissionVisibility(
    mxcpp::SurfaceClosure const& closure,
    GfVec3f const& transmissionVisibility)
{
    // `presence` is geometric coverage. `opacity` can be an alpha/transmission
    // control (e.g. UsdPreviewSurface transparent mode), so using it here would
    // bypass the transmissive shadow response for fully transparent glass.
    const float presence = _Clamp01(closure.presence);
    const GfVec3f passthroughVisibility(1.0f - presence);
    return _Clamp01(passthroughVisibility + transmissionVisibility * presence);
}

inline bool
_IsReflectionOnlyNode(
    mxcpp::Bsdf::ClosureTree const& tree,
    mxcpp::Bsdf::NodeId nodeId)
{
    const mxcpp::Bsdf::Node* const node = tree.Get(nodeId);
    if (!node) {
        return false;
    }

    return std::visit(
        [&](auto const& data) -> bool {
            using T = std::decay_t<decltype(data)>;
            if constexpr (
                std::is_same_v<T, mxcpp::Bsdf::OrenNayarDiffuseData> ||
                std::is_same_v<T, mxcpp::Bsdf::BurleyDiffuseData> ||
                std::is_same_v<T, mxcpp::Bsdf::ConductorData> ||
                std::is_same_v<T, mxcpp::Bsdf::SheenData>) {
                return true;
            } else if constexpr (
                std::is_same_v<T, mxcpp::Bsdf::DielectricData>) {
                return _IsEffectivelyZero(data.weight) ||
                       data.scatterMode == mxcpp::Bsdf::ScatterMode::Reflection;
            } else if constexpr (
                std::is_same_v<T, mxcpp::Bsdf::DielectricInterfaceData>) {
                return _IsEffectivelyZero(data.transmissionWeight);
            } else if constexpr (
                std::is_same_v<T, mxcpp::Bsdf::GeneralizedSchlickData>) {
                return _IsEffectivelyZero(data.weight) ||
                       data.scatterMode == mxcpp::Bsdf::ScatterMode::Reflection;
            } else if constexpr (
                std::is_same_v<T, mxcpp::Bsdf::AdobeOpenPbrData>) {
                return _IsEffectivelyOpaque(data.geometryOpacity) &&
                       _IsEffectivelyZero(data.transmissionWeight) &&
                       _IsEffectivelyZero(data.subsurfaceWeight);
            } else if constexpr (
                std::is_same_v<T, mxcpp::Bsdf::TranslucentData> ||
                std::is_same_v<T, mxcpp::Bsdf::SubsurfaceData>) {
                return _IsEffectivelyZero(data.weight);
            } else if constexpr (
                std::is_same_v<T, mxcpp::Bsdf::MixData>) {
                if (_IsEffectivelyZero(data.mix)) {
                    return _IsReflectionOnlyNode(tree, data.bg);
                }
                if (_IsEffectivelyOpaque(data.mix)) {
                    return _IsReflectionOnlyNode(tree, data.fg);
                }
                return _IsReflectionOnlyNode(tree, data.fg) &&
                       _IsReflectionOnlyNode(tree, data.bg);
            } else if constexpr (
                std::is_same_v<T, mxcpp::Bsdf::LayerData>) {
                return _IsReflectionOnlyNode(tree, data.top) &&
                       _IsReflectionOnlyNode(tree, data.base);
            } else if constexpr (
                std::is_same_v<T, mxcpp::Bsdf::AddData>) {
                return _IsReflectionOnlyNode(tree, data.in1) &&
                       _IsReflectionOnlyNode(tree, data.in2);
            } else if constexpr (
                std::is_same_v<T, mxcpp::Bsdf::MultiplyData>) {
                return _IsReflectionOnlyNode(tree, data.input);
            } else {
                return false;
            }
        },
        node->data);
}

inline bool
_IsReflectionOnlyClosure(mxcpp::SurfaceClosure const& closure)
{
    if (!_IsEffectivelyOpaque(closure.presence) ||
        !_IsEffectivelyOpaque(closure.opacity) ||
        closure.HasSubsurfaceScattering()) {
        return false;
    }

    if (closure.HasBsdfTree()) {
        return _IsReflectionOnlyNode(closure.bsdfTree, closure.bsdfTree.root);
    }

    return _IsEffectivelyZero(closure.transmission) &&
           _IsEffectivelyZero(closure.subsurfaceWeight);
}

inline bool
_HasTransmissionNode(
    mxcpp::Bsdf::ClosureTree const& tree,
    mxcpp::Bsdf::NodeId nodeId)
{
    const mxcpp::Bsdf::Node* const node = tree.Get(nodeId);
    if (!node) {
        return false;
    }

    return std::visit(
        [&](auto const& data) -> bool {
            using T = std::decay_t<decltype(data)>;
            if constexpr (
                std::is_same_v<T, mxcpp::Bsdf::DielectricData>) {
                return !_IsEffectivelyZero(data.weight) &&
                       data.scatterMode !=
                           mxcpp::Bsdf::ScatterMode::Reflection;
            } else if constexpr (
                std::is_same_v<T, mxcpp::Bsdf::DielectricInterfaceData>) {
                return !_IsEffectivelyZero(data.transmissionWeight);
            } else if constexpr (
                std::is_same_v<T, mxcpp::Bsdf::GeneralizedSchlickData>) {
                return !_IsEffectivelyZero(data.weight) &&
                       data.scatterMode !=
                           mxcpp::Bsdf::ScatterMode::Reflection;
            } else if constexpr (
                std::is_same_v<T, mxcpp::Bsdf::AdobeOpenPbrData>) {
                return !_IsEffectivelyZero(data.transmissionWeight);
            } else if constexpr (
                std::is_same_v<T, mxcpp::Bsdf::MixData>) {
                if (_IsEffectivelyZero(data.mix)) {
                    return _HasTransmissionNode(tree, data.bg);
                }
                if (_IsEffectivelyOpaque(data.mix)) {
                    return _HasTransmissionNode(tree, data.fg);
                }
                return _HasTransmissionNode(tree, data.fg) ||
                       _HasTransmissionNode(tree, data.bg);
            } else if constexpr (
                std::is_same_v<T, mxcpp::Bsdf::LayerData>) {
                return _HasTransmissionNode(tree, data.top) ||
                       _HasTransmissionNode(tree, data.base);
            } else if constexpr (
                std::is_same_v<T, mxcpp::Bsdf::AddData>) {
                return _HasTransmissionNode(tree, data.in1) ||
                       _HasTransmissionNode(tree, data.in2);
            } else if constexpr (
                std::is_same_v<T, mxcpp::Bsdf::MultiplyData>) {
                return _HasTransmissionNode(tree, data.input);
            } else {
                return false;
            }
        },
        node->data);
}

inline bool
_HasTransmissionClosure(mxcpp::SurfaceClosure const& closure)
{
    if (closure.HasBsdfTree()) {
        return _HasTransmissionNode(closure.bsdfTree, closure.bsdfTree.root);
    }

    return !_IsEffectivelyZero(closure.transmission);
}

inline bool
_IsVolumeOnlyBoundary(mxcpp::SurfaceClosure const& closure)
{
    return closure.hasInteriorMedium &&
           !closure.HasBsdfTree() &&
           _IsEffectivelyZero(closure.opacity);
}

inline GfVec3f
_GetBsdfNormal(
    mxcpp::SurfaceClosure const& closure,
    GfVec3f const& faceForwardedNormal,
    GfVec3f const& geometricNormal,
    GfVec3f const& wo)
{
    if (!_HasTransmissionClosure(closure)) {
        return faceForwardedNormal;
    }

    // Reflection and diffuse lobes want the shading normal face-forwarded to
    // wo, but thick transmission needs the interface side. Re-flip only for
    // transmissive closures when the ray is on the geometric back side, so
    // exit hits refract from the interior medium into the exterior medium.
    return GfDot(geometricNormal, wo) < 0.0f
        ? -faceForwardedNormal
        : faceForwardedNormal;
}

inline mxcpp::Mat4f
_ToMx(const GfMatrix4d& m)
{
    mxcpp::Mat4f result;
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            result[row][col] = static_cast<float>(m[row][col]);
        }
    }
    return result;
}

inline float
_RgbToSpectralValue(const GfVec3f& rgb, const _HeroWavelengthState& hero)
{
    return mxcpp::Spectral::RgbToSpectralValue(_ToMx(rgb), hero.wavelengthNm);
}

inline GfVec3f
_SpectralValueToRgb(float value, const _HeroWavelengthState& hero)
{
    return _ToGf(mxcpp::Spectral::SpectralValueToRgb(
        value,
        hero.wavelengthNm,
        hero.pdf));
}

inline GfVec3f
_SpectralScalarToRgb(float value, const _HeroWavelengthState& hero)
{
    return hero.active ? _SpectralValueToRgb(value, hero)
                       : GfVec3f(value);
}

/// Returns true if the "points" primvar uses subdivision sampling.
inline bool
_IsSubdivMesh(HdEmbreePrototypeContext const* prototypeContext)
{
    return prototypeContext->refined;
}

inline void
_InterpolateSubdivPosition(
    RTCGeometry geometry,
    unsigned int primID,
    float u,
    float v,
    GfVec3f* position,
    GfVec3f* dPdu,
    GfVec3f* dPdv)
{
    // rtcInterpolate1 writes through SIMD-width arrays. Compact GfVec3f
    // objects are only 12 bytes, so write to padded temporaries first.
    alignas(16) float sampled[4] = {};
    alignas(16) float sampledDu[4] = {};
    alignas(16) float sampledDv[4] = {};
    rtcInterpolate1(
        geometry, primID, u, v, RTC_BUFFER_TYPE_VERTEX, 0,
        sampled, dPdu ? sampledDu : nullptr, dPdv ? sampledDv : nullptr, 3);
    if (position) {
        *position = GfVec3f(sampled[0], sampled[1], sampled[2]);
    }
    if (dPdu) {
        *dPdu = GfVec3f(sampledDu[0], sampledDu[1], sampledDu[2]);
    }
    if (dPdv) {
        *dPdv = GfVec3f(sampledDv[0], sampledDv[1], sampledDv[2]);
    }
}

/// Try to compute a smooth limit-surface normal for a subdivision hit.
inline bool
_TryComputeSubdivLimitNormal(
    RTCScene rootScene,
    unsigned int geomID,
    unsigned int primID,
    float u,
    float v,
    GfVec3f* outNormal)
{
    GfVec3f posValue;
    GfVec3f dPdu(0.0f);
    GfVec3f dPdv(0.0f);
    _InterpolateSubdivPosition(
        rtcGetGeometry(rootScene, geomID),
        primID, u, v, &posValue, &dPdu, &dPdv);
    (void)posValue;

    GfVec3f limitNormal;
    if (!_TryBuildSurfaceNormal(dPdu, dPdv, &limitNormal)) {
        return false;
    }

    *outNormal = limitNormal;
    return true;
}

/// Resolve the object-space shading normal for a hit.
inline GfVec3f
_ResolveObjectSpaceNormal(
    HdEmbreePrototypeContext const* prototypeContext,
    RTCScene rootScene,
    unsigned int geomID,
    RTCRayHit const& rayHit,
    HdEmbreeDisplacedSubdivFrame* outDisplacedFrame = nullptr)
{
    if (outDisplacedFrame) {
        *outDisplacedFrame = HdEmbreeDisplacedSubdivFrame{};
    }
    GfVec3f normal = prototypeContext->orientationSign * GfVec3f(
        rayHit.hit.Ng_x, rayHit.hit.Ng_y, rayHit.hit.Ng_z);

    // rtcInterpolate ignores displacement, so reconstruct the differential
    // frame of P + D*N explicitly. Embree's hit Ng remains the true facet
    // orientation and is the fallback for invalid graph/patch evaluations.
    if (prototypeContext->displaced) {
        HdEmbreeDisplacedSubdivFrame displacedFrame;
        if (HdEmbreeComputeDisplacedSubdivFrame(
                rtcGetGeometry(rootScene, geomID),
                prototypeContext,
                rayHit.hit.primID,
                rayHit.hit.u,
                rayHit.hit.v,
                &displacedFrame)) {
            if (GfDot(displacedFrame.normal, normal) < 0.0f) {
                displacedFrame.normal = -displacedFrame.normal;
            }
            if (outDisplacedFrame) {
                *outDisplacedFrame = displacedFrame;
            }
            return displacedFrame.normal;
        }
        return normal;
    }

    auto it = prototypeContext->primvarMap.find(HdTokens->normals);
    if (it != prototypeContext->primvarMap.end()) {
        GfVec3f authoredNormal;
        if (it->second->Sample(
                rayHit.hit.primID, rayHit.hit.u, rayHit.hit.v,
                &authoredNormal) &&
            _TryNormalizeDirection(authoredNormal, &normal)) {
            return normal;
        }
    }

    if (_IsSubdivMesh(prototypeContext)) {
        GfVec3f limitNormal;
        if (_TryComputeSubdivLimitNormal(
                rootScene,
                geomID,
                rayHit.hit.primID,
                rayHit.hit.u,
                rayHit.hit.v,
                &limitNormal)) {
            return prototypeContext->orientationSign * limitNormal;
        }
    }

    return normal;
}

/// Compute dPdu/dPdv for a triangle mesh hit.
/// If st primvar is available, computes dP/ds, dP/dt via Jacobian inverse.
/// If not, uses barycentric edge vectors (P1-P0, P2-P0).
/// Falls back to GfBuildOrthonormalFrame on degenerate cases.
inline void
_ComputeTriangleSurfaceDerivatives(
    HdEmbreePrototypeContext const* prototypeContext,
    unsigned int primID,
    float u,
    float v,
    GfVec3f const& normal,
    GfVec3f* outDPdu, GfVec3f* outDPdv,
    GfVec3f* outDndu, GfVec3f* outDndv)
{
    bool haveCachedDerivatives = false;
    {
        if (prototypeContext->triangleDPdu && prototypeContext->triangleDPdv) {
            const VtVec3fArray& cachedDPdu = *prototypeContext->triangleDPdu;
            const VtVec3fArray& cachedDPdv = *prototypeContext->triangleDPdv;
            if (primID < cachedDPdu.size() && primID < cachedDPdv.size()) {
                *outDPdu = cachedDPdu[primID];
                *outDPdv = cachedDPdv[primID];
                haveCachedDerivatives = true;
            }
        }
    }

    if (!haveCachedDerivatives) {
        GfBuildOrthonormalFrame(normal, outDPdu, outDPdv);
    }

    // Try to get st vertices for normal derivatives.
    GfVec2f st[3];
    bool haveSt = false;
    {
        auto it = prototypeContext->primvarMap.find(_tokensSt);
        if (it != prototypeContext->primvarMap.end()) {
            auto* vtxSampler =
                dynamic_cast<HdEmbreeTriangleVertexSampler*>(it->second);
            if (vtxSampler) {
                haveSt = vtxSampler->SampleVertices(
                    primID, &st[0], &st[1], &st[2]);
            }
            if (!haveSt) {
                auto* fvSampler =
                    dynamic_cast<HdEmbreeTriangleFaceVaryingSampler*>(
                        it->second);
                if (fvSampler) {
                    haveSt = fvSampler->SampleVertices(
                        primID, &st[0], &st[1], &st[2]);
                }
            }
        }
    }

    // 3. Normal derivatives (dndu, dndv)
    GfVec3f N[3];
    bool haveNormals = false;
    {
        auto it = prototypeContext->primvarMap.find(HdTokens->normals);
        if (it != prototypeContext->primvarMap.end()) {
            auto* vtxSampler =
                dynamic_cast<HdEmbreeTriangleVertexSampler*>(it->second);
            if (vtxSampler) {
                haveNormals = vtxSampler->SampleVertices(
                    primID, &N[0], &N[1], &N[2]);
            }
            if (!haveNormals) {
                auto* fvSampler =
                    dynamic_cast<HdEmbreeTriangleFaceVaryingSampler*>(
                        it->second);
                if (fvSampler) {
                    haveNormals = fvSampler->SampleVertices(
                        primID, &N[0], &N[1], &N[2]);
                }
            }
        }
    }

    if (haveNormals) {
        const GfVec3f dN1 = N[1] - N[0];
        const GfVec3f dN2 = N[2] - N[0];
        const GfVec3f sampledNormal = N[0] + u * dN1 + v * dN2;
        if (haveSt) {
            GfVec2f dst1 = st[1] - st[0];
            GfVec2f dst2 = st[2] - st[0];
            float det = _DifferenceOfProducts(
                dst1[0], dst2[1], dst1[1], dst2[0]);
            if (std::abs(det) > 1e-9f) {
                float invDet = 1.0f / det;
                *outDndu = ( dst2[1] * dN1 - dst1[1] * dN2) * invDet;
                *outDndv = (-dst2[0] * dN1 + dst1[0] * dN2) * invDet;
            } else {
                *outDndu = dN1;
                *outDndv = dN2;
            }
        } else {
            *outDndu = dN1;
            *outDndv = dN2;
        }

        // Primvar interpolation produces an unnormalized vector. Differentiate
        // its normalization and preserve any face-forward orientation applied
        // to the shading normal by the caller.
        const float sampledLengthSquared = sampledNormal.GetLengthSq();
        if (_IsFinite(sampledNormal) &&
            std::isfinite(sampledLengthSquared) &&
            sampledLengthSquared > 1.0e-18f) {
            const float sampledLength = std::sqrt(sampledLengthSquared);
            const GfVec3f sampledUnitNormal =
                sampledNormal / sampledLength;
            *outDndu =
                (*outDndu - sampledUnitNormal *
                    GfDot(sampledUnitNormal, *outDndu)) /
                sampledLength;
            *outDndv =
                (*outDndv - sampledUnitNormal *
                    GfDot(sampledUnitNormal, *outDndv)) /
                sampledLength;
            if (GfDot(sampledUnitNormal, normal) < 0.0f) {
                *outDndu = -*outDndu;
                *outDndv = -*outDndv;
            }
        } else {
            *outDndu = GfVec3f(0.0f);
            *outDndv = GfVec3f(0.0f);
        }
    } else {
        *outDndu = GfVec3f(0.0f);
        *outDndv = GfVec3f(0.0f);
    }
}

/// Compute dPdu/dPdv for a subdivision surface hit.
inline void
_ComputeSubdivSurfaceDerivatives(
    HdEmbreePrototypeContext const* prototypeContext,
    RTCScene rootScene,
    unsigned int geomID,
    unsigned int primID, float u, float v,
    GfVec3f const& normal,
    GfVec3f* outDPdu, GfVec3f* outDPdv,
    GfVec3f* outDndu, GfVec3f* outDndv,
    HdEmbreeDisplacedSubdivFrame const* displacedFrame = nullptr)
{
    RTCGeometry const geometry = rtcGetGeometry(rootScene, geomID);

    // 1. Position derivatives. Displaced subdivision needs the derivatives
    //    of P + D*N; rtcInterpolate itself intentionally returns only the
    //    undisplaced limit surface.
    bool havePositionDerivs = false;
    if (prototypeContext->displaced) {
        if (displacedFrame && displacedFrame->valid) {
            *outDPdu = displacedFrame->dPdu;
            *outDPdv = displacedFrame->dPdv;
            havePositionDerivs = true;
        } else {
            GfVec3f displacedNormal;
            havePositionDerivs = HdEmbreeComputeDisplacedSubdivFrame(
                geometry,
                prototypeContext,
                primID,
                u,
                v,
                &displacedNormal,
                outDPdu,
                outDPdv);
        }
    }
    if (!havePositionDerivs) {
        GfVec3f posVal;
        _InterpolateSubdivPosition(
            geometry,
            primID, u, v, &posVal, outDPdu, outDPdv);
        havePositionDerivs = true;
    }

    if (!havePositionDerivs) {
        GfBuildOrthonormalFrame(normal, outDPdu, outDPdv);
        *outDndu = GfVec3f(0.0f);
        *outDndv = GfVec3f(0.0f);
        return;
    }

    // 2. If st available, transform from parametric to st space.
    //    Handle both GfVec2f and GfVec3f st buffers.
    //    Support both vertex and face-varying interpolation modes.
    HdEmbreeSubdivTexcoordJacobian stJacobian;
    auto const stIt = prototypeContext->primvarMap.find(_tokensSt);
    if (stIt != prototypeContext->primvarMap.end()) {
        stJacobian = HdEmbreeComputeSubdivTexcoordJacobian(
            stIt->second, primID, u, v);
    }
    if (stJacobian.valid) {
        const GfVec3f dPduParam = *outDPdu;
        const GfVec3f dPdvParam = *outDPdv;
        // Chain rule through the inverse st Jacobian.
        *outDPdu =
            stJacobian.duDs * dPduParam +
            stJacobian.dvDs * dPdvParam;
        *outDPdv =
            stJacobian.duDt * dPduParam +
            stJacobian.dvDt * dPdvParam;
    }

    // Degenerate check
    GfVec3f derivativeNormal;
    if (!_TryBuildSurfaceNormal(
            *outDPdu, *outDPdv, &derivativeNormal)) {
        GfBuildOrthonormalFrame(normal, outDPdu, outDPdv);
    }

    // 3. Normal derivatives
    //    Support both vertex and face-varying interpolation modes.
    *outDndu = GfVec3f(0.0f);
    *outDndv = GfVec3f(0.0f);
    // Computing derivatives of the displaced normal itself requires another
    // finite-difference ring around the three displacement probes. Zero is a
    // conservative value for ray differentials and avoids reusing unrelated
    // authored base-normal derivatives on displaced geometry.
    if (prototypeContext->displaced) {
        return;
    }
    {
        auto it = prototypeContext->primvarMap.find(HdTokens->normals);
        if (it != prototypeContext->primvarMap.end()) {
            GfVec3f sampledNormal(0.0f);
            bool haveNormalDerivatives = false;
            auto* vertexSampler =
                dynamic_cast<HdEmbreeSubdivVertexSampler*>(it->second);
            auto* varyingSampler =
                dynamic_cast<HdEmbreeSubdivVaryingSampler*>(it->second);
            auto* fvarSampler =
                dynamic_cast<HdEmbreeSubdivFaceVaryingSampler*>(it->second);
            if (vertexSampler) {
                haveNormalDerivatives =
                    vertexSampler->SampleWithDerivatives(
                        primID, u, v, &sampledNormal,
                        outDndu, outDndv);
            } else if (varyingSampler) {
                haveNormalDerivatives =
                    varyingSampler->SampleWithDerivatives(
                        primID, u, v, &sampledNormal,
                        outDndu, outDndv);
            } else if (fvarSampler) {
                haveNormalDerivatives =
                    fvarSampler->SampleWithDerivatives(
                        primID, u, v, &sampledNormal,
                        outDndu, outDndv);
            }

            if (!haveNormalDerivatives) {
                *outDndu = GfVec3f(0.0f);
                *outDndv = GfVec3f(0.0f);
                return;
            }

            if (stJacobian.valid) {
                const GfVec3f dNduParam = *outDndu;
                const GfVec3f dNdvParam = *outDndv;
                *outDndu =
                    stJacobian.duDs * dNduParam +
                    stJacobian.dvDs * dNdvParam;
                *outDndv =
                    stJacobian.duDt * dNduParam +
                    stJacobian.dvDt * dNdvParam;
            }

            const float sampledLengthSquared = sampledNormal.GetLengthSq();
            if (_IsFinite(sampledNormal) &&
                std::isfinite(sampledLengthSquared) &&
                sampledLengthSquared > 1.0e-18f) {
                const float sampledLength = std::sqrt(sampledLengthSquared);
                const GfVec3f sampledUnitNormal =
                    sampledNormal / sampledLength;
                *outDndu =
                    (*outDndu - sampledUnitNormal *
                        GfDot(sampledUnitNormal, *outDndu)) /
                    sampledLength;
                *outDndv =
                    (*outDndv - sampledUnitNormal *
                        GfDot(sampledUnitNormal, *outDndv)) /
                    sampledLength;
                if (GfDot(sampledUnitNormal, normal) < 0.0f) {
                    *outDndu = -*outDndu;
                    *outDndv = -*outDndv;
                }
            } else {
                *outDndu = GfVec3f(0.0f);
                *outDndv = GfVec3f(0.0f);
            }
        }
    }
}

/// Complete the lazily sampled displaced-normal derivatives, remap them from
/// Embree patch coordinates to authored st, and transform the normalized
/// normal derivative through the instance transform.
inline bool
_TryComputeDisplacedSubdivNormalDerivativesToWorld(
    HdEmbreePrototypeContext const* prototypeContext,
    HdEmbreeInstanceContext const* instanceContext,
    RTCScene rootScene,
    unsigned int geomID,
    HdEmbreeDisplacedSubdivFrame const& frame,
    GfVec3f const& faceForwardedWorldNormal,
    GfVec3f* outDndu,
    GfVec3f* outDndv)
{
    if (!prototypeContext || !rootScene || !frame.valid ||
        !outDndu || !outDndv) {
        return false;
    }

    GfVec3f objectDndu;
    GfVec3f objectDndv;
    if (!HdEmbreeComputeDisplacedSubdivNormalDerivatives(
            rtcGetGeometry(rootScene, geomID),
            prototypeContext,
            frame,
            &objectDndu,
            &objectDndv)) {
        return false;
    }

    auto const stIt = prototypeContext->primvarMap.find(_tokensSt);
    if (stIt != prototypeContext->primvarMap.end()) {
        const HdEmbreeSubdivTexcoordJacobian stJacobian =
            HdEmbreeComputeSubdivTexcoordJacobian(
                stIt->second, frame.primID, frame.u, frame.v);
        if (stJacobian.valid) {
            const GfVec3f dNduParam = objectDndu;
            const GfVec3f dNdvParam = objectDndv;
            objectDndu =
                stJacobian.duDs * dNduParam +
                stJacobian.dvDs * dNdvParam;
            objectDndv =
                stJacobian.duDt * dNduParam +
                stJacobian.dvDt * dNdvParam;
        }
    }

    GfVec3f objectNormal = frame.normal;
    const GfVec3f frameWorldNormal =
        _TransformNormalToWorld(instanceContext, objectNormal);
    if (GfDot(frameWorldNormal, faceForwardedWorldNormal) < 0.0f) {
        objectNormal = -objectNormal;
        objectDndu = -objectDndu;
        objectDndv = -objectDndv;
    }

    const GfVec3f worldDndu = _TransformNormalDerivativeToWorld(
        instanceContext, objectNormal, objectDndu);
    const GfVec3f worldDndv = _TransformNormalDerivativeToWorld(
        instanceContext, objectNormal, objectDndv);
    if (!_IsFinite(worldDndu) || !_IsFinite(worldDndv)) {
        return false;
    }

    *outDndu = worldDndu;
    *outDndv = worldDndv;
    return true;
}

/// Compute screen-space derivatives from ray differentials and surface
/// derivatives.  When ray differentials are unavailable (non-specular
/// bounces), uses a camera-projection-based fallback approximation.
/// Populates dPdx, dPdy, dudx, dvdx, dudy, dvdy on the ShadingContext.
///
/// Note: viewMatrix, inverseProjMatrix, imageWidth, imageHeight, and
/// samplesPerPixel are only used for the fallback path. They come from
/// HdEmbreeRenderer member variables, accessed by the caller.
inline void
_ComputeScreenSpaceDerivatives(
    HdEmbreeRayDifferential const& rayDiff,
    GfVec3f const& hitPos,
    GfVec3f const& normal,
    GfVec3f const& dPdu,
    GfVec3f const& dPdv,
    GfMatrix4d const& viewMatrix,
    GfMatrix4d const& inverseProjMatrix,
    float imageWidth,
    float imageHeight,
    int samplesPerPixel,
    mxcpp::ShadingContext& ctx)
{
    GfVec3f dPdx(0.0f);
    GfVec3f dPdy(0.0f);

    if (rayDiff.hasDifferentials) {
        // Intersect differential rays with the tangent plane at hitPos.
        float d = -GfDot(normal, hitPos);
        float rxDotN = GfDot(normal, rayDiff.rxDirection);
        if (std::abs(rxDotN) > 1e-10f) {
            float tx = -(GfDot(normal, rayDiff.rxOrigin) + d) / rxDotN;
            GfVec3f px = rayDiff.rxOrigin + tx * rayDiff.rxDirection;
            dPdx = px - hitPos;
        }
        float ryDotN = GfDot(normal, rayDiff.ryDirection);
        if (std::abs(ryDotN) > 1e-10f) {
            float ty = -(GfDot(normal, rayDiff.ryOrigin) + d) / ryDotN;
            GfVec3f py = rayDiff.ryOrigin + ty * rayDiff.ryDirection;
            dPdy = py - hitPos;
        }
    } else {
        // Fallback: approximate dPdx/dPdy from camera projection.
        GfVec3f hitCamera = GfVec3f(viewMatrix.Transform(hitPos));
        float dist = hitCamera.GetLength();
        if (dist > 1e-6f) {
            GfVec3f ndcCenter(0.0f, 0.0f, -1.0f);
            GfVec3f ndcDx(2.0f / imageWidth, 0.0f, -1.0f);
            GfVec3f ndcDy(0.0f, 2.0f / imageHeight, -1.0f);
            GfVec3f camCenter = GfVec3f(inverseProjMatrix.Transform(ndcCenter));
            GfVec3f camDx = GfVec3f(inverseProjMatrix.Transform(ndcDx));
            GfVec3f camDy = GfVec3f(inverseProjMatrix.Transform(ndcDy));
            float pixelScaleX = (camDx - camCenter).GetLength() * dist;
            float pixelScaleY = (camDy - camCenter).GetLength() * dist;

            float sppScale = (samplesPerPixel > 1)
                ? 1.0f / std::sqrt(static_cast<float>(samplesPerPixel))
                : 1.0f;
            pixelScaleX *= sppScale;
            pixelScaleY *= sppScale;

            GfVec3f t, b;
            GfBuildOrthonormalFrame(normal, &t, &b);
            dPdx = t * pixelScaleX;
            dPdy = b * pixelScaleY;
        }
    }

    ctx.dPdx = _ToMx(dPdx);
    ctx.dPdy = _ToMx(dPdy);

    // Solve for UV derivatives: A^T A x = A^T b (least squares)
    float ata00 = GfDot(dPdu, dPdu);
    float ata01 = GfDot(dPdu, dPdv);
    float ata11 = GfDot(dPdv, dPdv);
    float detATA = _DifferenceOfProducts(ata00, ata11, ata01, ata01);

    if (std::abs(detATA) > 1e-18f) {
        float invDet = 1.0f / detATA;
        float atb0x = GfDot(dPdu, dPdx);
        float atb1x = GfDot(dPdv, dPdx);
        float atb0y = GfDot(dPdu, dPdy);
        float atb1y = GfDot(dPdv, dPdy);

        ctx.dudx = std::clamp(
            _DifferenceOfProducts(ata11, atb0x, ata01, atb1x) * invDet,
            -1e8f, 1e8f);
        ctx.dvdx = std::clamp(
            _DifferenceOfProducts(ata00, atb1x, ata01, atb0x) * invDet,
            -1e8f, 1e8f);
        ctx.dudy = std::clamp(
            _DifferenceOfProducts(ata11, atb0y, ata01, atb1y) * invDet,
            -1e8f, 1e8f);
        ctx.dvdy = std::clamp(
            _DifferenceOfProducts(ata00, atb1y, ata01, atb0y) * invDet,
            -1e8f, 1e8f);
    }
}

}  // anonymous namespace

namespace {

/// Fill in an RTCRay structure from the given parameters.
inline void
_PopulateRay(
    RTCRay *ray,
    GfVec3f const& origin,
    GfVec3f const& dir,
    float nearest,
    float furthest = std::numeric_limits<float>::infinity(),
    HdEmbree_RayMask mask = HdEmbree_RayMask::All)
{
    ray->org_x = origin[0];
    ray->org_y = origin[1];
    ray->org_z = origin[2];
    ray->tnear = nearest;

    ray->dir_x = dir[0];
    ray->dir_y = dir[1];
    ray->dir_z = dir[2];
    ray->time = 0.0f;

    ray->tfar = furthest;
    ray->mask = static_cast<uint32_t>(mask);
    ray->id = 0;
    ray->flags = 0;
}

inline GfVec3f
_OffsetRayOrigin(
    GfVec3f const& position,
    GfVec3f const& normal,
    GfVec3f const& direction,
    float bias = 1.0e-4f)
{
    if (normal.GetLengthSq() < 1e-18f) {
        return position + direction * bias;
    }

    GfVec3f offsetNormal = normal.GetNormalized();
    if (GfDot(offsetNormal, direction) < 0.0f) {
        offsetNormal = -offsetNormal;
    }
    return position + offsetNormal * bias;
}

/// Fill in an RTCRayHit structure from the given parameters.
// note this containts a Ray and a RayHit
inline void
_PopulateRayHit(
    RTCRayHit* rayHit,
    GfVec3f const& origin,
    GfVec3f const& dir,
    float nearest,
    float furthest = std::numeric_limits<float>::infinity(),
    HdEmbree_RayMask mask = HdEmbree_RayMask::All)
{
    // Fill in defaults for the ray
    _PopulateRay(&rayHit->ray, origin, dir, nearest, furthest, mask);

    // Fill in defaults for the hit
    rayHit->hit.primID = RTC_INVALID_GEOMETRY_ID;
    rayHit->hit.geomID = RTC_INVALID_GEOMETRY_ID;
    for (unsigned int i = 0; i < RTC_MAX_INSTANCE_LEVEL_COUNT; ++i) {
        rayHit->hit.instID[i] = RTC_INVALID_GEOMETRY_ID;
    }
}

inline bool
_PopulateSssExitRayHit(
    RTCRayHit* rayHit,
    HdEmbreeSssOutput const& sssOut)
{
    if (sssOut.exitInstanceId == RTC_INVALID_GEOMETRY_ID ||
        sssOut.exitGeomId == RTC_INVALID_GEOMETRY_ID ||
        sssOut.exitPrimId == RTC_INVALID_GEOMETRY_ID) {
        return false;
    }

    GfVec3f rayDir = -sssOut.exitDir;
    if (rayDir.GetLengthSq() <= 1.0e-20f) {
        return false;
    }
    rayDir.Normalize();

    _PopulateRayHit(
        rayHit,
        sssOut.exitPos,
        rayDir,
        0.0f,
        0.0f,
        HdEmbree_RayMask::Camera);

    rayHit->hit.primID = sssOut.exitPrimId;
    rayHit->hit.geomID = sssOut.exitGeomId;
    rayHit->hit.instID[0] = sssOut.exitInstanceId;
    rayHit->hit.u = sssOut.exitU;
    rayHit->hit.v = sssOut.exitV;
    rayHit->hit.Ng_x = sssOut.exitObjectGeomNormal[0];
    rayHit->hit.Ng_y = sssOut.exitObjectGeomNormal[1];
    rayHit->hit.Ng_z = sssOut.exitObjectGeomNormal[2];
    return true;
}

/// Generate a random cosine-weighted direction ray (in the hemisphere
/// around <0,0,1>).  The input is a pair of uniformly distributed random
/// numbers in the range [0,1].
///
/// The algorithm here is to generate a random point on the disk, and project
/// that point to the unit hemisphere.
inline GfVec3f
_CosineWeightedDirection(GfVec2f const& uniform_float)
{
    GfVec3f dir;
    float theta = 2.0f * _pi<float> * uniform_float[0];
    float eta = uniform_float[1];
    float sqrteta = sqrtf(eta);
    dir[0] = cosf(theta) * sqrteta;
    dir[1] = sinf(theta) * sqrteta;
    dir[2] = sqrtf(1.0f-eta);
    return dir;
}

}  // anonymous namespace

inline void
HdEmbreeRenderer::_ApplyPathWeight(
    GfVec3f const& weight, _PathState* state) const
{
    if (!state) {
        return;
    }
    if (state->hero.active) {
        const _HeroWavelengthState hero{
            true, state->hero.wavelengthNm, state->hero.pdf};
        state->spectralThroughput *= _RgbToSpectralValue(weight, hero);
    } else {
        state->throughput = GfCompMult(state->throughput, weight);
    }
}

inline GfVec3f
HdEmbreeRenderer::_GetPathThroughputRgb(_PathState const& state) const
{
    const _HeroWavelengthState hero{
        state.hero.active,
        state.hero.wavelengthNm,
        state.hero.pdf};
    return state.hero.active
        ? _SpectralScalarToRgb(state.spectralThroughput, hero)
        : state.throughput;
}

PXR_NAMESPACE_CLOSE_SCOPE

#endif  // PXR_IMAGING_PLUGIN_HD_EMBREE_RENDERER_IMPL_H
