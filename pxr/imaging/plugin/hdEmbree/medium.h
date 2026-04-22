//
// hdEmbree medium helpers shared by renderer and MaterialXCpp.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_MEDIUM_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_MEDIUM_H

#include "MaterialXCpp/mathTypes.h"

namespace mxcpp {

struct MediumProperties
{
    Vec3f sigmaA = Vec3f(0.0f);
    Vec3f sigmaS = Vec3f(0.0f);
    float anisotropy = 0.0f;

    Vec3f SigmaT() const noexcept {
        return sigmaA + sigmaS;
    }

    bool IsVacuum() const noexcept {
        const Vec3f sigmaT = SigmaT();
        return sigmaT[0] <= 0.0f && sigmaT[1] <= 0.0f && sigmaT[2] <= 0.0f;
    }

    bool IsAbsorbingOnly() const noexcept {
        return sigmaS[0] <= 0.0f && sigmaS[1] <= 0.0f && sigmaS[2] <= 0.0f &&
               !IsVacuum();
    }
};

Vec3f EvalBeerTransmittance(
    const MediumProperties& medium,
    float distance) noexcept;

float MajorantExtinction(
    const MediumProperties& medium) noexcept;

Vec3f EvalMajorantTransmittanceWeight(
    const MediumProperties& medium,
    float distance) noexcept;

Vec3f EvalFreeFlightScatterWeight(
    const MediumProperties& medium,
    float distance) noexcept;

float SampleFreeFlight(
    const MediumProperties& medium,
    float u) noexcept;

float PhaseHG(
    float cosTheta,
    float anisotropy) noexcept;

Vec3f SampleHenyeyGreenstein(
    const Vec3f& wo,
    float anisotropy,
    float u1,
    float u2) noexcept;

float PdfHenyeyGreenstein(
    const Vec3f& wi,
    const Vec3f& wo,
    float anisotropy) noexcept;

MediumProperties MakeTransmissionMedium(
    float transmissionWeight,
    const Vec3f& transmissionColor,
    float transmissionDepth,
    const Vec3f& transmissionScatter,
    float transmissionScatterAnisotropy) noexcept;

// --- Per-channel spectral tracking for SSS random walks ---
// Using the majorant (max sigmaT) for free-flight sampling creates extreme
// variance when channels have very different extinction coefficients (e.g.
// skin).  These functions track by a single channel's sigmaT instead,
// bounding per-channel weights and eliminating the exponential blow-up.

/// Return the channel index with the minimum non-zero sigmaT.
int MinExtinctionChannel(
    const MediumProperties& medium) noexcept;

/// Sample a free-flight distance using a specific channel's sigmaT.
float SampleFreeFlightChannel(
    const MediumProperties& medium,
    int channel,
    float u) noexcept;

/// Per-channel scatter weight when tracking with a specific channel's
/// sigmaT (ratio tracking).
Vec3f EvalChannelScatterWeight(
    const MediumProperties& medium,
    int channel,
    float distance) noexcept;

/// Per-channel transmittance weight when tracking with a specific
/// channel's sigmaT.
Vec3f EvalChannelTransmittanceWeight(
    const MediumProperties& medium,
    int channel,
    float distance) noexcept;

/// Chiang balance heuristic channel selection MIS.
///
/// Selects one of 3 RGB channels with probability proportional to
/// `throughput[i] * weights[i]`.  Returns the selected channel index
/// and writes all channel probabilities to `channelPdf`.
///
/// If all weights sum to zero, falls back to uniform (1/3 each).
///
/// Used in random walk SSS (Phase 2) to importance-sample color channels.
/// Designed to be reusable by transmission scattering (Phase 4, ticket #404).
int ChannelMIS(
    const Vec3f& throughput,
    const Vec3f& weights,
    float u,
    Vec3f* channelPdf);

}  // namespace mxcpp

#endif  // PXR_IMAGING_PLUGIN_HD_EMBREE_MEDIUM_H
