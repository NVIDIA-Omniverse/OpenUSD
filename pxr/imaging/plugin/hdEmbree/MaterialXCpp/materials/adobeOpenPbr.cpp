//
// Adobe OpenPBR adapter for MaterialXCpp.
//
#include "adobeOpenPbr.h"

#include "../../medium.h"
#include "../nodes/helpers/mathHelpers.h"
#include "../paramMap.h"
#include "openPbr.h"

#include <algorithm>
#include <cmath>
#include <utility>

#ifdef PXR_HDEMBREE_ENABLE_ADOBE_OPENPBR
#include <glm/glm.hpp>
#include <openpbr/openpbr.h>
#endif

namespace mxcpp {

#ifdef PXR_HDEMBREE_ENABLE_ADOBE_OPENPBR

static const SlotName _kBaseWeight("base_weight");
static const SlotName _kBaseColor("base_color");
static const SlotName _kBaseDiffuseRoughness("base_diffuse_roughness");
static const SlotName _kLegacyBaseRoughness("base_roughness");
static const SlotName _kBaseMetalness("base_metalness");
static const SlotName _kSpecularWeight("specular_weight");
static const SlotName _kSpecularColor("specular_color");
static const SlotName _kSpecularRoughness("specular_roughness");
static const SlotName _kSpecularIor("specular_ior");
static const SlotName _kSpecularRoughnessAnisotropy(
    "specular_roughness_anisotropy");
static const SlotName _kLegacySpecularAnisotropy("specular_anisotropy");
static const SlotName _kTransmissionWeight("transmission_weight");
static const SlotName _kTransmissionColor("transmission_color");
static const SlotName _kTransmissionDispersionScale(
    "transmission_dispersion_scale");
static const SlotName _kTransmissionDispersionAbbeNumber(
    "transmission_dispersion_abbe_number");
static const SlotName _kTransmissionDepth("transmission_depth");
static const SlotName _kTransmissionScatter("transmission_scatter");
static const SlotName _kTransmissionScatterAnisotropy(
    "transmission_scatter_anisotropy");
static const SlotName _kSubsurfaceWeight("subsurface_weight");
static const SlotName _kSubsurfaceColor("subsurface_color");
static const SlotName _kSubsurfaceRadius("subsurface_radius");
static const SlotName _kSubsurfaceRadiusScale("subsurface_radius_scale");
static const SlotName _kSubsurfaceScatterAnisotropy(
    "subsurface_scatter_anisotropy");
static const SlotName _kCoatWeight("coat_weight");
static const SlotName _kCoatColor("coat_color");
static const SlotName _kCoatRoughness("coat_roughness");
static const SlotName _kCoatRoughnessAnisotropy("coat_roughness_anisotropy");
static const SlotName _kCoatIor("coat_ior");
static const SlotName _kCoatDarkening("coat_darkening");
static const SlotName _kThinFilmWeight("thin_film_weight");
static const SlotName _kThinFilmThickness("thin_film_thickness");
static const SlotName _kThinFilmIor("thin_film_ior");
static const SlotName _kGeometryCoatNormal("geometry_coat_normal");
static const SlotName _kCoatNormal("coat_normal");
static const SlotName _kFuzzWeight("fuzz_weight");
static const SlotName _kFuzzColor("fuzz_color");
static const SlotName _kFuzzRoughness("fuzz_roughness");
static const SlotName _kEmissionLuminance("emission_luminance");
static const SlotName _kEmissionColor("emission_color");
static const SlotName _kGeometryOpacity("geometry_opacity");
static const SlotName _kGeometryThinWalled("geometry_thin_walled");
static const SlotName _kGeometryNormal("geometry_normal");
static const SlotName _kGeometryTangent("geometry_tangent");
static const SlotName _kGeometryCoatTangent("geometry_coat_tangent");
static const SlotName _kLegacyNormal("normal");
static const SlotName _kLegacyTangent("tangent");

namespace {

constexpr float _kEpsilon = 1.0e-7f;

template <typename T>
T
_GetWithFallback(const ParamMap& params,
                 const SlotName& primary,
                 const SlotName& fallback,
                 const T& defaultValue)
{
    if (params.Find(primary)) {
        return Get<T>(params, primary, defaultValue);
    }
    return Get<T>(params, fallback, defaultValue);
}

float
_AverageOpacity(const ParamMap& params)
{
    const Vec3f opacityVec = Get<Vec3f>(
        params, _kGeometryOpacity, Vec3f(1.0f));
    float opacity =
        (opacityVec[0] + opacityVec[1] + opacityVec[2]) / 3.0f;
    if (opacity == 1.0f) {
        opacity = Get<float>(params, _kGeometryOpacity, 1.0f);
    }
    return std::clamp(opacity, 0.0f, 1.0f);
}

Bsdf::AdobeOpenPbrData
_MakeAdobeOpenPbrData(const ParamMap& params)
{
    Bsdf::AdobeOpenPbrData data;

    data.baseWeight = Get<float>(params, _kBaseWeight, data.baseWeight);
    data.baseColor = Get<Vec3f>(params, _kBaseColor, data.baseColor);
    data.baseDiffuseRoughness = _GetWithFallback<float>(
        params,
        _kBaseDiffuseRoughness,
        _kLegacyBaseRoughness,
        data.baseDiffuseRoughness);
    data.baseMetalness =
        Get<float>(params, _kBaseMetalness, data.baseMetalness);

    data.subsurfaceWeight =
        Get<float>(params, _kSubsurfaceWeight, data.subsurfaceWeight);
    data.subsurfaceColor =
        Get<Vec3f>(params, _kSubsurfaceColor, data.subsurfaceColor);
    data.subsurfaceRadius =
        Get<float>(params, _kSubsurfaceRadius, data.subsurfaceRadius);
    data.subsurfaceRadiusScale = Get<Vec3f>(
        params, _kSubsurfaceRadiusScale, data.subsurfaceRadiusScale);
    data.subsurfaceScatterAnisotropy = Get<float>(
        params,
        _kSubsurfaceScatterAnisotropy,
        data.subsurfaceScatterAnisotropy);

    data.specularWeight =
        Get<float>(params, _kSpecularWeight, data.specularWeight);
    data.specularColor =
        Get<Vec3f>(params, _kSpecularColor, data.specularColor);
    data.specularRoughness =
        Get<float>(params, _kSpecularRoughness, data.specularRoughness);
    data.specularRoughnessAnisotropy = _GetWithFallback<float>(
        params,
        _kSpecularRoughnessAnisotropy,
        _kLegacySpecularAnisotropy,
        data.specularRoughnessAnisotropy);
    data.specularIor =
        Get<float>(params, _kSpecularIor, data.specularIor);

    data.coatWeight = Get<float>(params, _kCoatWeight, data.coatWeight);
    data.coatColor = Get<Vec3f>(params, _kCoatColor, data.coatColor);
    data.coatRoughness =
        Get<float>(params, _kCoatRoughness, data.coatRoughness);
    data.coatRoughnessAnisotropy = Get<float>(
        params, _kCoatRoughnessAnisotropy, data.coatRoughnessAnisotropy);
    data.coatIor = Get<float>(params, _kCoatIor, data.coatIor);
    data.coatDarkening =
        Get<float>(params, _kCoatDarkening, data.coatDarkening);

    data.fuzzWeight = Get<float>(params, _kFuzzWeight, data.fuzzWeight);
    data.fuzzColor = Get<Vec3f>(params, _kFuzzColor, data.fuzzColor);
    data.fuzzRoughness =
        Get<float>(params, _kFuzzRoughness, data.fuzzRoughness);

    data.transmissionWeight =
        Get<float>(params, _kTransmissionWeight, data.transmissionWeight);
    data.transmissionColor =
        Get<Vec3f>(params, _kTransmissionColor, data.transmissionColor);
    data.transmissionDepth =
        Get<float>(params, _kTransmissionDepth, data.transmissionDepth);
    data.transmissionScatter =
        Get<Vec3f>(params, _kTransmissionScatter, data.transmissionScatter);
    data.transmissionScatterAnisotropy = Get<float>(
        params,
        _kTransmissionScatterAnisotropy,
        data.transmissionScatterAnisotropy);
    data.transmissionDispersionScale = Get<float>(
        params,
        _kTransmissionDispersionScale,
        data.transmissionDispersionScale);
    data.transmissionDispersionAbbeNumber = Get<float>(
        params,
        _kTransmissionDispersionAbbeNumber,
        data.transmissionDispersionAbbeNumber);

    data.thinFilmWeight =
        Get<float>(params, _kThinFilmWeight, data.thinFilmWeight);
    data.thinFilmThickness =
        Get<float>(params, _kThinFilmThickness, data.thinFilmThickness);
    data.thinFilmIor = Get<float>(params, _kThinFilmIor, data.thinFilmIor);

    data.emissionLuminance =
        Get<float>(params, _kEmissionLuminance, data.emissionLuminance);
    data.emissionColor =
        Get<Vec3f>(params, _kEmissionColor, data.emissionColor);

    data.geometryOpacity = _AverageOpacity(params);
    data.geometryThinWalled =
        Get<bool>(params, _kGeometryThinWalled, data.geometryThinWalled);
    data.geometryNormal = _GetWithFallback<Vec3f>(
        params,
        _kGeometryNormal,
        _kLegacyNormal,
        data.geometryNormal);
    data.geometryTangent = _GetWithFallback<Vec3f>(
        params,
        _kGeometryTangent,
        _kLegacyTangent,
        data.geometryTangent);

    const bool hasCoatNormal =
        params.Find(_kGeometryCoatNormal) || params.Find(_kCoatNormal);
    data.geometryCoatNormal = hasCoatNormal
        ? _GetWithFallback<Vec3f>(
              params,
              _kGeometryCoatNormal,
              _kCoatNormal,
              data.geometryNormal)
        : data.geometryNormal;
    data.geometryCoatTangent = _GetWithFallback<Vec3f>(
        params,
        _kGeometryCoatTangent,
        _kGeometryTangent,
        data.geometryTangent);

    return data;
}

Vec3f
_CleanNonnegative(const Vec3f& value)
{
    Vec3f result = value;
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(result[i]) || result[i] < 0.0f) {
            result[i] = 0.0f;
        }
    }
    return result;
}

Vec3f
_NormalizeOrFallback(const Vec3f& value, const Vec3f& fallback)
{
    if (value.length2() > _kEpsilon * _kEpsilon) {
        return value.normalized();
    }
    return fallback.normalized();
}

vec3
_ToOpenPbr(const Vec3f& value)
{
    return vec3(value[0], value[1], value[2]);
}

Vec3f
_FromOpenPbr(const vec3& value)
{
    return Vec3f(value.x, value.y, value.z);
}

OpenPBR_Basis
_MakeBasis(const Vec3f& normal, const Vec3f& tangent)
{
    const Vec3f n = _NormalizeOrFallback(normal, Vec3f(0.0f, 0.0f, 1.0f));
    const Vec3f projectedTangent = tangent - n * Dot(tangent, n);
    if (projectedTangent.length2() <= _kEpsilon * _kEpsilon) {
        return openpbr_make_basis(_ToOpenPbr(n));
    }
    return openpbr_make_basis(
        _ToOpenPbr(n),
        _ToOpenPbr(projectedTangent.normalized()),
        1.0f);
}

OpenPBR_ResolvedInputs
_MakeResolvedInputs(const Bsdf::AdobeOpenPbrData& data, const Vec3f& N)
{
    OpenPBR_ResolvedInputs inputs = openpbr_make_default_resolved_inputs();

    inputs.base_weight = data.baseWeight;
    inputs.base_color = _ToOpenPbr(data.baseColor);
    inputs.base_diffuse_roughness = data.baseDiffuseRoughness;
    inputs.base_metalness = data.baseMetalness;

    inputs.subsurface_weight = data.subsurfaceWeight;
    inputs.subsurface_color = _ToOpenPbr(data.subsurfaceColor);
    inputs.subsurface_radius = data.subsurfaceRadius;
    inputs.subsurface_radius_scale = _ToOpenPbr(data.subsurfaceRadiusScale);
    inputs.subsurface_scatter_anisotropy =
        data.subsurfaceScatterAnisotropy;

    inputs.specular_weight = data.specularWeight;
    inputs.specular_color = _ToOpenPbr(data.specularColor);
    inputs.specular_roughness = data.specularRoughness;
    inputs.specular_roughness_anisotropy =
        data.specularRoughnessAnisotropy;
    inputs.specular_ior = data.specularIor;

    inputs.coat_weight = data.coatWeight;
    inputs.coat_color = _ToOpenPbr(data.coatColor);
    inputs.coat_roughness = data.coatRoughness;
    inputs.coat_roughness_anisotropy = data.coatRoughnessAnisotropy;
    inputs.coat_ior = data.coatIor;
    inputs.coat_darkening = data.coatDarkening;

    inputs.fuzz_weight = data.fuzzWeight;
    inputs.fuzz_color = _ToOpenPbr(data.fuzzColor);
    inputs.fuzz_roughness = data.fuzzRoughness;

    inputs.transmission_weight = data.transmissionWeight;
    inputs.transmission_color = _ToOpenPbr(data.transmissionColor);
    inputs.transmission_depth = data.transmissionDepth;
    inputs.transmission_scatter = _ToOpenPbr(data.transmissionScatter);
    inputs.transmission_scatter_anisotropy =
        data.transmissionScatterAnisotropy;
    inputs.transmission_dispersion_scale =
        data.transmissionDispersionScale;
    inputs.transmission_dispersion_abbe_number =
        data.transmissionDispersionAbbeNumber;

    inputs.thin_film_weight = data.thinFilmWeight;
    inputs.thin_film_thickness = data.thinFilmThickness;
    inputs.thin_film_ior = data.thinFilmIor;

    inputs.emission_luminance = data.emissionLuminance;
    inputs.emission_color = _ToOpenPbr(data.emissionColor);

    inputs.geometry_opacity = data.geometryOpacity;
    inputs.geometry_thin_walled = data.geometryThinWalled;
    inputs.geometry_basis = _MakeBasis(N, data.geometryTangent);
    inputs.geometry_coat_basis = _MakeBasis(N, data.geometryCoatTangent);

    return inputs;
}

OpenPBR_PreparedBsdf
_Prepare(const Bsdf::AdobeOpenPbrData& data,
         const Vec3f& N,
         const Vec3f& wo)
{
    const OpenPBR_ResolvedInputs inputs = _MakeResolvedInputs(data, N);
    const Vec3f normalizedWo = _NormalizeOrFallback(wo, N);
    return openpbr_prepare(
        inputs,
        vec3(1.0f),
        OpenPBR_BaseRgbWavelengths_nm,
        OpenPBR_VacuumIor,
        _ToOpenPbr(normalizedWo));
}

}  // namespace

#endif  // PXR_HDEMBREE_ENABLE_ADOBE_OPENPBR

SurfaceClosure
EvalAdobeOpenPbr(const ParamMap& params)
{
#ifndef PXR_HDEMBREE_ENABLE_ADOBE_OPENPBR
    return EvalOpenPbr(params);
#else
    const Bsdf::AdobeOpenPbrData data = _MakeAdobeOpenPbrData(params);

    SurfaceClosure closure;
    closure.baseColor = data.baseColor * data.baseWeight;
    closure.roughness = data.specularRoughness;
    closure.metallic = data.baseMetalness;
    closure.specular = data.specularWeight;
    closure.specularColor = data.specularColor;
    closure.specularIor = data.specularIor;
    closure.emissiveColor = data.emissionColor * data.emissionLuminance;
    closure.transmission = data.transmissionWeight;
    closure.transmissionColor = data.transmissionColor;
    closure.opacity = data.geometryOpacity;
    closure.presence = data.geometryOpacity;
    closure.coat = data.coatWeight;
    closure.coatRoughness = data.coatRoughness;
    closure.coatIor = data.coatIor;
    closure.sheen = data.fuzzWeight;
    closure.sheenColor = data.fuzzColor;
    closure.sheenRoughness = data.fuzzRoughness;
    closure.normal = data.geometryNormal;
    closure.thinWalled = data.geometryThinWalled;
    closure.subsurfaceWeight = data.subsurfaceWeight;
    closure.subsurfaceColor = data.subsurfaceColor;
    closure.subsurfaceRadius = Vec3f(data.subsurfaceRadius);
    closure.subsurfaceRadiusScale = data.subsurfaceRadiusScale;
    closure.subsurfaceAnisotropy = data.subsurfaceScatterAnisotropy;
    closure.interiorMedium = MakeAdobeOpenPbrInteriorMedium(data);
    closure.hasInteriorMedium =
        !data.geometryThinWalled && !closure.interiorMedium.IsVacuum();

    Bsdf::ClosureTree tree;
    tree.root = tree.Add(data);
    closure.bsdfTree = std::move(tree);
    return closure;
#endif
}

Vec3f
EvalAdobeOpenPbr(
    const Bsdf::AdobeOpenPbrData& data,
    const Vec3f& N,
    const Vec3f& wi,
    const Vec3f& wo)
{
#ifndef PXR_HDEMBREE_ENABLE_ADOBE_OPENPBR
    (void)data;
    (void)N;
    (void)wi;
    (void)wo;
    return Vec3f(0.0f);
#else
    const float cosThetaI = std::abs(Dot(N, wi));
    if (cosThetaI <= _kEpsilon) {
        return Vec3f(0.0f);
    }

    const OpenPBR_PreparedBsdf prepared = _Prepare(data, N, wo);
    const OpenPBR_DiffuseSpecular valueWithCos =
        openpbr_eval(prepared, _ToOpenPbr(_NormalizeOrFallback(wi, N)));
    const Vec3f value =
        _FromOpenPbr(openpbr_get_sum_of_diffuse_specular(valueWithCos));
    return _CleanNonnegative(value * (1.0f / cosThetaI));
#endif
}

float
PdfAdobeOpenPbr(
    const Bsdf::AdobeOpenPbrData& data,
    const Vec3f& N,
    const Vec3f& wi,
    const Vec3f& wo)
{
#ifndef PXR_HDEMBREE_ENABLE_ADOBE_OPENPBR
    (void)data;
    (void)N;
    (void)wi;
    (void)wo;
    return 0.0f;
#else
    const OpenPBR_PreparedBsdf prepared = _Prepare(data, N, wo);
    const float pdf =
        openpbr_pdf(prepared, _ToOpenPbr(_NormalizeOrFallback(wi, N)));
    return std::isfinite(pdf) ? std::max(pdf, 0.0f) : 0.0f;
#endif
}

Bsdf::BsdfSample
SampleAdobeOpenPbr(
    const Bsdf::AdobeOpenPbrData& data,
    const Vec3f& N,
    const Vec3f& wo,
    float u1,
    float u2,
    float uLobe)
{
#ifndef PXR_HDEMBREE_ENABLE_ADOBE_OPENPBR
    (void)data;
    (void)N;
    (void)wo;
    (void)u1;
    (void)u2;
    (void)uLobe;
    return Bsdf::BsdfSample{Vec3f(0.0f), Vec3f(0.0f), 0.0f, false};
#else
    const OpenPBR_PreparedBsdf prepared = _Prepare(data, N, wo);

    vec3 lightDirection(0.0f);
    OpenPBR_DiffuseSpecular weight;
    float pdf = 0.0f;
    OpenPBR_BsdfLobeType sampledType = OpenPBR_BsdfLobeTypeNone;
    openpbr_sample(
        prepared,
        vec3(u1, u2, uLobe),
        lightDirection,
        weight,
        pdf,
        sampledType);

    if (!std::isfinite(pdf) || pdf <= 0.0f) {
        return Bsdf::BsdfSample{Vec3f(0.0f), Vec3f(0.0f), 0.0f, false};
    }

    const Vec3f wi = _NormalizeOrFallback(_FromOpenPbr(lightDirection), N);
    const Vec3f weightSum =
        _CleanNonnegative(_FromOpenPbr(openpbr_get_sum_of_diffuse_specular(weight)));
    const bool isSpecular =
        (sampledType & OpenPBR_BsdfLobeTypeSpecular) != 0;

    Vec3f f(0.0f);
    if (isSpecular) {
        f = weightSum;
    } else {
        const float cosThetaI = std::abs(Dot(N, wi));
        if (cosThetaI <= _kEpsilon) {
            return Bsdf::BsdfSample{Vec3f(0.0f), Vec3f(0.0f), 0.0f, false};
        }
        f = weightSum * (pdf / cosThetaI);
    }

    Bsdf::BsdfSample sample{
        wi,
        _CleanNonnegative(f),
        pdf,
        isSpecular
    };
    const bool isSpecularTransmission =
        isSpecular &&
        ((sampledType & OpenPBR_BsdfLobeTypeTransmission) != 0);
    if (isSpecularTransmission) {
        const float ior = std::max(data.specularIor, 1.0f);
        sample.eta = Dot(N, wo) > 0.0f ? (1.0f / ior) : ior;
    }
    return sample;
#endif
}

MediumProperties
MakeAdobeOpenPbrInteriorMedium(const Bsdf::AdobeOpenPbrData& data)
{
#ifndef PXR_HDEMBREE_ENABLE_ADOBE_OPENPBR
    (void)data;
    return MediumProperties();
#else
    if (data.geometryThinWalled) {
        return MediumProperties();
    }

    const OpenPBR_PreparedBsdf prepared =
        _Prepare(data, Vec3f(0.0f, 0.0f, 1.0f), Vec3f(0.0f, 0.0f, 1.0f));
    if (!openpbr_is_homogeneous_volume(prepared.volume)) {
        return MediumProperties();
    }

    const Vec3f extinction =
        _CleanNonnegative(_FromOpenPbr(prepared.volume.extinction_coefficient));
    Vec3f albedo = _FromOpenPbr(prepared.volume.albedo);
    for (int i = 0; i < 3; ++i) {
        albedo[i] = std::clamp(albedo[i], 0.0f, 1.0f);
    }

    MediumProperties medium;
    medium.sigmaS = CompMul(extinction, albedo);
    medium.sigmaA = CompMul(extinction, Vec3f(1.0f) - albedo);
    medium.anisotropy = std::clamp(
        prepared.volume.anisotropy,
        -0.999f,
        0.999f);
    return medium;
#endif
}

}  // namespace mxcpp
