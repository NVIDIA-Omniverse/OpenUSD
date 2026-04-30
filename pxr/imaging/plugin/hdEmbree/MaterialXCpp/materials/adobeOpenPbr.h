//
// Adobe OpenPBR adapter for MaterialXCpp.
//
#ifndef MXCPP_MATERIALS_ADOBE_OPEN_PBR_H
#define MXCPP_MATERIALS_ADOBE_OPEN_PBR_H

#include "../paramMap.h"
#include "../surfaceClosure.h"
#include "bsdf.h"

#include <memory>

namespace mxcpp {

/// Evaluate the OpenPBR Surface material model using Adobe's OpenPBR BSDF as
/// a whole-model backend.  When Adobe support is not compiled in, this falls
/// back to the native hdEmbree OpenPBR evaluator.
SurfaceClosure EvalAdobeOpenPbr(const ParamMap& params);
SurfaceClosure EvalAdobeOpenPbrVisibility(const ParamMap& params);

Vec3f EvalAdobeOpenPbr(
    const Bsdf::AdobeOpenPbrData& data,
    const Vec3f& N,
    const Vec3f& wi,
    const Vec3f& wo);

struct AdobeOpenPbrEvalPdfResult
{
    Vec3f value = Vec3f(0.0f);
    float pdf = 0.0f;
    bool evaluated = false;
};

struct AdobeOpenPbrPreparedSurfaceState;

struct AdobeOpenPbrPreparedSurface
{
    std::shared_ptr<const AdobeOpenPbrPreparedSurfaceState> state;
    bool valid = false;
};

AdobeOpenPbrPreparedSurface PrepareAdobeOpenPbrSurface(
    const SurfaceClosure& closure,
    const Vec3f& N,
    const Vec3f& wo);

AdobeOpenPbrEvalPdfResult EvalPdfAdobeOpenPbr(
    const Bsdf::AdobeOpenPbrData& data,
    const Vec3f& N,
    const Vec3f& wi,
    const Vec3f& wo);

AdobeOpenPbrEvalPdfResult TryEvalPdfAdobeOpenPbrSurface(
    const SurfaceClosure& closure,
    const Vec3f& N,
    const Vec3f& wi,
    const Vec3f& wo);

AdobeOpenPbrEvalPdfResult EvalPdfPreparedAdobeOpenPbrSurface(
    const AdobeOpenPbrPreparedSurface& preparedSurface,
    const Vec3f& wi);

float PdfAdobeOpenPbr(
    const Bsdf::AdobeOpenPbrData& data,
    const Vec3f& N,
    const Vec3f& wi,
    const Vec3f& wo);

Bsdf::BsdfSample SampleAdobeOpenPbr(
    const Bsdf::AdobeOpenPbrData& data,
    const Vec3f& N,
    const Vec3f& wo,
    float u1,
    float u2,
    float uLobe);

Bsdf::BsdfSample SamplePreparedAdobeOpenPbrSurface(
    const AdobeOpenPbrPreparedSurface& preparedSurface,
    float u1,
    float u2,
    float uLobe);

MediumProperties MakeAdobeOpenPbrInteriorMedium(
    const Bsdf::AdobeOpenPbrData& data);

Vec3f AdobeOpenPbrEvalVolumeTransmittance(
    const MediumProperties& medium,
    float distance);

float AdobeOpenPbrSampleVolumeEventDistance(
    const MediumProperties& medium,
    const Vec3f& throughput,
    float u);

Vec3f AdobeOpenPbrCalculateVolumeEventWeight(
    const MediumProperties& medium,
    const Vec3f& throughput,
    float distance);

Vec3f AdobeOpenPbrCalculateVolumeSurfaceWeight(
    const MediumProperties& medium,
    const Vec3f& throughput,
    float distance);

Vec3f AdobeOpenPbrSampleVolumePhase(
    const MediumProperties& medium,
    const Vec3f& wo,
    float u1,
    float u2);

float AdobeOpenPbrEvalVolumePhasePdf(
    const MediumProperties& medium,
    const Vec3f& wi,
    const Vec3f& wo);

}  // namespace mxcpp

#endif  // MXCPP_MATERIALS_ADOBE_OPEN_PBR_H
