//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_IMAGING_PLUGIN_HDEMBREE_MATERIALXCPP_BSDF_SHADINGFRAME_H
#define PXR_IMAGING_PLUGIN_HDEMBREE_MATERIALXCPP_BSDF_SHADINGFRAME_H

#include "mathPrimitives.h"

#include <renderer/materials/MaterialXCpp/materials/bsdf.h>
#include <renderer/materials/MaterialXCpp/nodes/helpers/colorHelpers.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace mxcpp {
namespace Bsdf {
namespace detail {

/// Reflects finite vector `v` across a finite unit `normal`.
/// Preserves vector length within floating-point error and cannot fail.
inline Vec3f
MirrorAcrossSurface(const Vec3f& v, const Vec3f& normal)
{
    return v - 2.0f * Dot(v, normal) * normal;
}

/// Right-handed orthonormal shading frame in world space.
struct Frame {
    Vec3f tangentWld;
    Vec3f bitangentWld;
    Vec3f normalShdWldOut;

    /// Projects finite `directionWld` into this orthonormal frame.
    /// Returns signed local coordinates and cannot fail.
    Vec3f
    ToLocal(const Vec3f& directionWld) const
    {
        return Vec3f(Dot(directionWld, tangentWld),
                     Dot(directionWld, bitangentWld),
                     Dot(directionWld, normalShdWldOut));
    }

    /// Reconstructs a world-space vector from finite local coordinates.
    /// Returns a vector with the same length when this frame is orthonormal;
    /// cannot fail.
    Vec3f
    ToWorld(const Vec3f& directionLocal) const
    {
        return tangentWld * directionLocal[0] +
               bitangentWld * directionLocal[1] +
               normalShdWldOut * directionLocal[2];
    }

    /// Builds a stable orthonormal frame around finite unit
    /// `normalShdWldOut`. Returns a right-handed frame and cannot fail.
    static Frame
    FromNormal(const Vec3f& normalShdWldOut)
    {
        Frame frame;
        frame.normalShdWldOut = normalShdWldOut;
        const Vec3f helper = (std::abs(normalShdWldOut[0]) < 0.9f)
                                 ? Vec3f(1.0f, 0.0f, 0.0f)
                                 : Vec3f(0.0f, 1.0f, 0.0f);
        frame.tangentWld = normalShdWldOut.cross(helper).normalized();
        frame.bitangentWld = normalShdWldOut.cross(frame.tangentWld);
        return frame;
    }

    /// Builds a frame around finite unit `normalShdWldOut`, using finite
    /// `tangentWld` after projection. A zero or parallel tangent falls back to
    /// `FromNormal`. Returns a right-handed orthonormal frame; cannot fail.
    static Frame
    FromNormalAndTangent(const Vec3f& normalShdWldOut, const Vec3f& tangentWld)
    {
        const Vec3f tangentProjectedWld =
            tangentWld - normalShdWldOut * Dot(tangentWld, normalShdWldOut);
        if (tangentProjectedWld.length() < kEpsilon) {
            return FromNormal(normalShdWldOut);
        }

        Frame frame;
        frame.normalShdWldOut = normalShdWldOut;
        frame.tangentWld = tangentProjectedWld.normalized();
        frame.bitangentWld = Cross(frame.normalShdWldOut, frame.tangentWld);
        if (frame.bitangentWld.length() < kEpsilon) {
            return FromNormal(normalShdWldOut);
        }
        frame.bitangentWld.normalize();
        frame.tangentWld = Cross(frame.bitangentWld, frame.normalShdWldOut);
        return frame;
    }
};

/// Maps independent finite samples `u1` and `u2` from [0,1) to a unit
/// direction on the positive-Z cosine hemisphere. Cannot fail.
inline Vec3f
SampleCosineHemisphere(float u1, float u2)
{
    float cosTheta = std::sqrt(u1);
    float sinTheta = std::sqrt(1.0f - u1);
    float phi = 2.0f * kPi * u2;
    return Vec3f(sinTheta * std::cos(phi),
                 sinTheta * std::sin(phi),
                 cosTheta);
}

/// Returns the cosine-hemisphere solid-angle density for finite `cosTheta`.
/// Negative cosines return zero. The result is finite and non-negative;
/// cannot fail.
inline float
CosineHemispherePdf(float cosTheta)
{
    return std::max(cosTheta, 0.0f) * kInvPi;
}

/// Returns the absolute Z cosine of finite local direction `w`.
/// `w` is normally unit length. Cannot fail.
inline float
AbsCosTheta(const Vec3f& w)
{
    return std::abs(w[2]);
}

/// Returns tan(theta)^2 for finite unit local direction `w`.
/// A grazing direction whose squared cosine is at most `kEpsilon` returns
/// positive infinity; otherwise the result is finite and non-negative.
inline float
Tan2Theta(const Vec3f& w)
{
    const float cosTheta2 = w[2] * w[2];
    if (cosTheta2 <= kEpsilon) {
        return std::numeric_limits<float>::infinity();
    }
    return std::max(0.0f, 1.0f - cosTheta2) / cosTheta2;
}

/// Orients finite unit `normalShdWldOut` toward finite unit `omegaOutWld`.
/// Returns either the original normal or its negation; cannot fail.
inline Vec3f
FaceForwardNormal(const Vec3f& normalShdWldOut, const Vec3f& omegaOutWld)
{
    return (Dot(normalShdWldOut, omegaOutWld) < 0.0f) ? -normalShdWldOut
                                                      : normalShdWldOut;
}

/// Normalizes finite `v`, or returns finite `fallback` when its length is below
/// `kEpsilon`. Callers requiring a unit result must provide a unit fallback.
/// Cannot fail.
inline Vec3f
NormalizeOrFallback(const Vec3f& v, const Vec3f& fallback)
{
    const float length = v.length();
    if (length < kEpsilon) {
        return fallback;
    }
    return v / length;
}

/// Resolves and face-forwards the reflection normal stored in `data`.
/// `DataT` must expose `hasShadingNormal` and finite `normal`; the supplied
/// normal and outgoing direction must be finite unit vectors. Missing or
/// degenerate authored normals fall back to `normalShdWldOut`. Cannot fail.
template<typename DataT>
inline Vec3f
ResolveReflectionNormal(const DataT& data, const Vec3f& normalShdWldOut,
                         const Vec3f& omegaOutWld)
{
    if (!data.hasShadingNormal) {
        return FaceForwardNormal(normalShdWldOut, omegaOutWld);
    }

    return FaceForwardNormal(
        NormalizeOrFallback(data.normal,
                             FaceForwardNormal(normalShdWldOut, omegaOutWld)),
        omegaOutWld);
}

/// Returns whether finite unit directions `omegaInWld` and `omegaOutWld` lie
/// on the same open side of finite unit `normalShdWldOut`. Tangent directions
/// return false. Cannot fail.
inline bool
IsSameSide(const Vec3f& normalShdWldOut, const Vec3f& omegaInWld,
            const Vec3f& omegaOutWld)
{
    return Dot(normalShdWldOut, omegaInWld) *
               Dot(normalShdWldOut, omegaOutWld) >
           0.0f;
}

/// Applies inverse lobe-selection probability to an already weighted discrete
/// specular sample. `selectionProb` must be finite in [0,1]. Non-specular,
/// invalid, or zero-probability samples are returned unchanged. Cannot fail.
inline Bsdf::BsdfSample
ScaleDiscreteSpecularSample(
    Bsdf::BsdfSample sample,
    float selectionProb)
{
    if (sample.isSpecular && sample.pdfSolidAngle > 0.0f &&
        selectionProb > 0.0f) {
        sample.bsdfValue /= selectionProb;
    }
    return sample;
}

/// Samples perfect reflection about finite unit `normalShdLobeWldOut`.
/// `omegaOutWld` must be a finite unit vector; `reflectance` and `weight` must
/// be finite and non-negative. Returns a unit direction, sanitized BSDF value,
/// `pdfSolidAngle == 1`, `isSpecular == true`, and `eta == 1`. Cannot fail.
inline Bsdf::BsdfSample
SampleDeltaReflection(const Vec3f& reflectance, float weight,
                       const Vec3f& normalShdLobeWldOut,
                       const Vec3f& omegaOutWld)
{
    Vec3f omegaInWld =
        2.0f * Dot(normalShdLobeWldOut, omegaOutWld) * normalShdLobeWldOut -
        omegaOutWld;
    omegaInWld.normalize();

    Bsdf::BsdfSample sample{omegaInWld, SafeVec(reflectance * weight), 1.0f,
                            true};
    sample.eta = 1.0f;
    return sample;
}

}  // namespace detail
}  // namespace Bsdf
}  // namespace mxcpp

#endif  // PXR_IMAGING_PLUGIN_HDEMBREE_MATERIALXCPP_BSDF_SHADINGFRAME_H
