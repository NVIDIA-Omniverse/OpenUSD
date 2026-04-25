//
// Adobe OpenPBR adapter for MaterialXCpp.
//
#ifndef MXCPP_MATERIALS_ADOBE_OPEN_PBR_H
#define MXCPP_MATERIALS_ADOBE_OPEN_PBR_H

#include "../paramMap.h"
#include "../surfaceClosure.h"
#include "bsdf.h"

namespace mxcpp {

/// Evaluate the OpenPBR Surface material model using Adobe's OpenPBR BSDF as
/// a whole-model backend.  When Adobe support is not compiled in, this falls
/// back to the native hdEmbree OpenPBR evaluator.
SurfaceClosure EvalAdobeOpenPbr(const ParamMap& params);

Vec3f EvalAdobeOpenPbr(
    const Bsdf::AdobeOpenPbrData& data,
    const Vec3f& N,
    const Vec3f& wi,
    const Vec3f& wo);

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

MediumProperties MakeAdobeOpenPbrInteriorMedium(
    const Bsdf::AdobeOpenPbrData& data);

}  // namespace mxcpp

#endif  // MXCPP_MATERIALS_ADOBE_OPEN_PBR_H
