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

MediumProperties MakeSubsurfaceMedium(
    float subsurfaceWeight,
    const Vec3f& subsurfaceColor,
    const Vec3f& subsurfaceRadius,
    const Vec3f& subsurfaceRadiusScale,
    float subsurfaceAnisotropy) noexcept;

}  // namespace mxcpp

#endif  // PXR_IMAGING_PLUGIN_HD_EMBREE_MEDIUM_H
