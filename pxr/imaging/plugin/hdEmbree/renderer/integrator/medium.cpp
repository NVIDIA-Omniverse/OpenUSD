//
// hdEmbree medium helpers shared by renderer and MaterialXCpp.
//
#include "medium.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace mxcpp {

float PhaseHG(float cosTheta, float anisotropy) noexcept;

namespace {

constexpr float _kEpsilon = 1.0e-6f;
constexpr float _kPi = 3.14159265358979323846f;

Vec3f
_ClampNonNegative(const Vec3f& value) noexcept
{
    return Vec3f(
        std::max(value[0], 0.0f),
        std::max(value[1], 0.0f),
        std::max(value[2], 0.0f));
}

Vec3f
_SafeNormalized(const Vec3f& value, const Vec3f& fallback) noexcept
{
    if (value.length2() <= _kEpsilon * _kEpsilon) {
        return fallback;
    }
    Vec3f normalized = value;
    normalized.normalize();
    return normalized;
}

void
_CoordinateSystem(
    const Vec3f& zAxis,
    Vec3f* xAxis,
    Vec3f* yAxis) noexcept
{
    const Vec3f z = _SafeNormalized(zAxis, Vec3f(0.0f, 0.0f, 1.0f));
    if (std::abs(z[2]) < 0.999f) {
        *xAxis = Vec3f(-z[1], z[0], 0.0f);
    } else {
        *xAxis = Vec3f(0.0f, 1.0f, 0.0f).cross(z);
    }
    if (xAxis->length2() <= _kEpsilon * _kEpsilon) {
        *xAxis = Vec3f(1.0f, 0.0f, 0.0f);
    } else {
        xAxis->normalize();
    }
    *yAxis = z.cross(*xAxis);
    if (yAxis->length2() <= _kEpsilon * _kEpsilon) {
        *yAxis = Vec3f(0.0f, 1.0f, 0.0f);
    } else {
        yAxis->normalize();
    }
}

}  // namespace

Vec3f
EvalBeerTransmittance(const MediumProperties& medium, float distance) noexcept
{
    if (distance <= 0.0f || medium.IsVacuum()) {
        return Vec3f(1.0f);
    }

    const Vec3f sigmaT = medium.SigmaT();
    return Vec3f(
        std::exp(-sigmaT[0] * distance),
        std::exp(-sigmaT[1] * distance),
        std::exp(-sigmaT[2] * distance));
}

float
MajorantExtinction(const MediumProperties& medium) noexcept
{
    const Vec3f sigmaT = medium.SigmaT();
    return std::max({sigmaT[0], sigmaT[1], sigmaT[2], 0.0f});
}

Vec3f
EvalMajorantTransmittanceWeight(
    const MediumProperties& medium,
    float distance) noexcept
{
    if (distance <= 0.0f || medium.IsVacuum()) {
        return Vec3f(1.0f);
    }

    const float sigmaMaj = MajorantExtinction(medium);
    if (sigmaMaj <= _kEpsilon) {
        return Vec3f(1.0f);
    }

    const Vec3f sigmaT = medium.SigmaT();
    return Vec3f(
        std::exp((sigmaMaj - sigmaT[0]) * distance),
        std::exp((sigmaMaj - sigmaT[1]) * distance),
        std::exp((sigmaMaj - sigmaT[2]) * distance));
}

Vec3f
EvalFreeFlightScatterWeight(
    const MediumProperties& medium,
    float distance) noexcept
{
    const float sigmaMaj = MajorantExtinction(medium);
    if (sigmaMaj <= _kEpsilon) {
        return Vec3f(0.0f);
    }

    const Vec3f transmittanceWeight =
        EvalMajorantTransmittanceWeight(medium, distance);
    const Vec3f sigmaMajVec(sigmaMaj);
    return CompMul(
        transmittanceWeight,
        CompDiv(_ClampNonNegative(medium.sigmaS), sigmaMajVec));
}

float
SampleFreeFlight(const MediumProperties& medium, float u) noexcept
{
    const float sigmaTMajor = MajorantExtinction(medium);
    if (sigmaTMajor <= _kEpsilon) {
        return std::numeric_limits<float>::infinity();
    }

    const float clampedU = std::clamp(u, _kEpsilon, 1.0f - _kEpsilon);
    return -std::log(1.0f - clampedU) / sigmaTMajor;
}

float
PhaseHG(float cosTheta, float anisotropy) noexcept
{
    const float g = std::clamp(anisotropy, -0.999f, 0.999f);
    const float denom = 1.0f + g * g + 2.0f * g * cosTheta;
    return (1.0f - g * g) / (4.0f * _kPi * denom * std::sqrt(denom));
}

Vec3f
SampleHenyeyGreenstein(
    const Vec3f& wo,
    float anisotropy,
    float u1,
    float u2) noexcept
{
    const float g = std::clamp(anisotropy, -0.999f, 0.999f);
    const float sampleU1 = std::clamp(u1, _kEpsilon, 1.0f - _kEpsilon);
    const float sampleU2 = std::clamp(u2, 0.0f, 1.0f);

    float cosTheta = 0.0f;
    if (std::abs(g) < 1.0e-3f) {
        cosTheta = 1.0f - 2.0f * sampleU1;
    } else {
        const float sqrTerm =
            (1.0f - g * g) / (1.0f - g + 2.0f * g * sampleU1);
        cosTheta = (1.0f + g * g - sqrTerm * sqrTerm) / (2.0f * g);
        cosTheta = std::clamp(cosTheta, -1.0f, 1.0f);
    }

    const float sinTheta =
        std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
    const float phi = 2.0f * _kPi * sampleU2;
    const float sinPhi = std::sin(phi);
    const float cosPhi = std::cos(phi);

    const Vec3f zAxis = _SafeNormalized(-wo, Vec3f(0.0f, 0.0f, 1.0f));
    Vec3f xAxis(1.0f, 0.0f, 0.0f);
    Vec3f yAxis(0.0f, 1.0f, 0.0f);
    _CoordinateSystem(zAxis, &xAxis, &yAxis);

    Vec3f wi = xAxis * (sinTheta * cosPhi) +
               yAxis * (sinTheta * sinPhi) +
               zAxis * cosTheta;
    if (wi.length2() <= _kEpsilon * _kEpsilon) {
        return zAxis;
    }
    wi.normalize();
    return wi;
}

float
PdfHenyeyGreenstein(
    const Vec3f& wi,
    const Vec3f& wo,
    float anisotropy) noexcept
{
    const Vec3f safeWi = _SafeNormalized(wi, Vec3f(0.0f, 0.0f, 1.0f));
    const Vec3f safeWo = _SafeNormalized(wo, Vec3f(0.0f, 0.0f, 1.0f));
    return PhaseHG(safeWi.dot(safeWo), anisotropy);
}

MediumProperties
MakeTransmissionMedium(
    float transmissionWeight,
    const Vec3f& transmissionColor,
    float transmissionDepth,
    const Vec3f& transmissionScatter,
    float transmissionScatterAnisotropy) noexcept
{
    MediumProperties medium;
    if (transmissionWeight <= 0.0f || transmissionDepth <= _kEpsilon) {
        return medium;
    }

    const Vec3f safeColor(
        std::clamp(transmissionColor[0], _kEpsilon, 1.0f),
        std::clamp(transmissionColor[1], _kEpsilon, 1.0f),
        std::clamp(transmissionColor[2], _kEpsilon, 1.0f));
    const Vec3f scatter = _ClampNonNegative(transmissionScatter);

    const Vec3f extinction(
        -std::log(safeColor[0]) / transmissionDepth,
        -std::log(safeColor[1]) / transmissionDepth,
        -std::log(safeColor[2]) / transmissionDepth);
    medium.sigmaS = scatter / transmissionDepth;
    Vec3f absorption(
        extinction[0] - medium.sigmaS[0],
        extinction[1] - medium.sigmaS[1],
        extinction[2] - medium.sigmaS[2]);

    // Match the MaterialX volume graph: if scattering pushes any absorption
    // channel below zero, shift the full absorption vector so the minimum
    // lands on zero instead of clamping channels independently.
    const float minAbsorption = std::min(
        {absorption[0], absorption[1], absorption[2]});
    if (minAbsorption < 0.0f) {
        absorption -= Vec3f(minAbsorption);
    }

    medium.sigmaA = _ClampNonNegative(absorption);
    medium.anisotropy = std::clamp(transmissionScatterAnisotropy, -1.0f, 1.0f);
    return medium;
}

int
MinExtinctionChannel(const MediumProperties& medium) noexcept
{
    const Vec3f sigmaT = medium.SigmaT();
    int minCh = 0;
    float minVal = sigmaT[0];
    for (int i = 1; i < 3; ++i) {
        if (sigmaT[i] > _kEpsilon && sigmaT[i] < minVal) {
            minVal = sigmaT[i];
            minCh = i;
        }
    }
    return minCh;
}

float
SampleFreeFlightChannel(
    const MediumProperties& medium,
    int channel,
    float u) noexcept
{
    const float sigmaT = medium.SigmaT()[std::clamp(channel, 0, 2)];
    if (sigmaT <= _kEpsilon) {
        return std::numeric_limits<float>::infinity();
    }
    const float clampedU = std::clamp(u, _kEpsilon, 1.0f - _kEpsilon);
    return -std::log(1.0f - clampedU) / sigmaT;
}

Vec3f
EvalChannelScatterWeight(
    const MediumProperties& medium,
    int channel,
    float distance) noexcept
{
    const Vec3f sigmaT = medium.SigmaT();
    const float trackSigmaT = sigmaT[std::clamp(channel, 0, 2)];
    if (trackSigmaT <= _kEpsilon) {
        return Vec3f(0.0f);
    }
    const Vec3f sigmaS = _ClampNonNegative(medium.sigmaS);
    return Vec3f(
        sigmaS[0] / trackSigmaT *
            std::exp((trackSigmaT - sigmaT[0]) * distance),
        sigmaS[1] / trackSigmaT *
            std::exp((trackSigmaT - sigmaT[1]) * distance),
        sigmaS[2] / trackSigmaT *
            std::exp((trackSigmaT - sigmaT[2]) * distance));
}

Vec3f
EvalChannelTransmittanceWeight(
    const MediumProperties& medium,
    int channel,
    float distance) noexcept
{
    if (distance <= 0.0f) {
        return Vec3f(1.0f);
    }
    const Vec3f sigmaT = medium.SigmaT();
    const float trackSigmaT = sigmaT[std::clamp(channel, 0, 2)];
    return Vec3f(
        std::exp((trackSigmaT - sigmaT[0]) * distance),
        std::exp((trackSigmaT - sigmaT[1]) * distance),
        std::exp((trackSigmaT - sigmaT[2]) * distance));
}

int
ChannelMIS(
    const Vec3f& throughput,
    const Vec3f& weights,
    float u,
    Vec3f* channelPdf)
{
    Vec3f raw(
        std::max(throughput[0] * weights[0], 0.0f),
        std::max(throughput[1] * weights[1], 0.0f),
        std::max(throughput[2] * weights[2], 0.0f));
    const float sum = raw[0] + raw[1] + raw[2];

    if (sum <= 1.0e-12f) {
        // Uniform fallback.
        *channelPdf = Vec3f(1.0f / 3.0f);
        const float scaled = std::clamp(u * 3.0f, 0.0f, 3.0f - 1.0e-6f);
        return static_cast<int>(scaled);
    }

    *channelPdf = raw * (1.0f / sum);
    const float u01 = std::clamp(u, 0.0f, 1.0f - 1.0e-6f);
    if (u01 < (*channelPdf)[0]) return 0;
    if (u01 < (*channelPdf)[0] + (*channelPdf)[1]) return 1;
    return 2;
}

}  // namespace mxcpp
