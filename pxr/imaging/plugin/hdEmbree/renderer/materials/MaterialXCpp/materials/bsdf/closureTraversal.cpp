//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "closureTraversal.h"
#include "dielectric.h"
#include "diffuse.h"
#include "energyCompensation.h"
#include "fresnel.h"
#include "legacySurface.h"
#include "mathPrimitives.h"
#include "microfacet.h"
#include "reflectionOnlyInterfaces.h"
#include "shadingFrame.h"
#include "sheen.h"
#include "thinFilm.h"

#include <renderer/materials/MaterialXCpp/materials/adobeOpenPbr.h>

#include <algorithm>
#include <cmath>
#include <type_traits>
#include <variant>
namespace mxcpp {
namespace Bsdf {
namespace detail {

Vec3f EvalNode(const Bsdf::ClosureTree& tree, Bsdf::NodeId nodeId,
                const Vec3f& normalShdWldOut, const Vec3f& omegaInWld,
                const Vec3f& omegaOutWld, float heroWavelengthNm);

static Vec3f _EvalThroughput(
    const Bsdf::ClosureTree& tree, Bsdf::NodeId nodeId,
    const Vec3f& normalShdWldOut, const Vec3f& omegaOutWld,
    float heroWavelengthNm);

static float _ApproxWeight(
    const Bsdf::ClosureTree& tree, Bsdf::NodeId nodeId,
    const Vec3f& normalShdWldOut, const Vec3f& omegaOutWld,
    float heroWavelengthNm);

float PdfNode(const Bsdf::ClosureTree& tree, Bsdf::NodeId nodeId,
               const Vec3f& normalShdWldOut, const Vec3f& omegaInWld,
               const Vec3f& omegaOutWld, float heroWavelengthNm);

Bsdf::BsdfSample SampleNode(const Bsdf::ClosureTree& tree, Bsdf::NodeId nodeId,
                             const Vec3f& normalShdWldOut,
                             const Vec3f& omegaOutWld, float u1, float u2,
                             float uChoice, float heroWavelengthNm);

class CausticClassPruner
{
public:
    explicit CausticClassPruner(const Bsdf::ClosureTree& source)
        : _source(source)
    {
        _result.luminanceCoefficients = source.luminanceCoefficients;
    }

    Bsdf::ClosureTree Run()
    {
        _result.root = PruneNode(_source.root);
        if (!_result.IsValid(_result.root)) {
            _result.Clear();
        }
        return std::move(_result);
    }

private:
    Bsdf::NodeId PruneNode(Bsdf::NodeId nodeId)
    {
        const auto luminance = [&](const Vec3f& value) {
            return Bsdf::detail::Luminance(
                value, _source.luminanceCoefficients);
        };
        const Bsdf::Node* node = _source.Get(nodeId);
        if (!node) {
            return Bsdf::InvalidNodeId;
        }

        return std::visit([&](const auto& data) -> Bsdf::NodeId {
            using T = std::decay_t<decltype(data)>;
            if constexpr (std::is_same_v<T, Bsdf::OrenNayarDiffuseData> ||
                          std::is_same_v<T, Bsdf::BurleyDiffuseData> ||
                          std::is_same_v<T, Bsdf::TranslucentData> ||
                          std::is_same_v<T, Bsdf::SubsurfaceData> ||
                          std::is_same_v<T, Bsdf::SheenData> ||
                          std::is_same_v<T, Bsdf::UnsupportedData>) {
                return _result.Add(data);
            } else if constexpr (std::is_same_v<T, Bsdf::DielectricData>) {
                if (IsEffectivelyDeltaAlpha(data.roughness) ||
                    data.scatterMode == Bsdf::ScatterMode::Transmission) {
                    return Bsdf::InvalidNodeId;
                }
                Bsdf::DielectricData pruned = data;
                if (pruned.scatterMode ==
                    Bsdf::ScatterMode::ReflectionTransmission) {
                    pruned.scatterMode = Bsdf::ScatterMode::Reflection;
                }
                return _result.Add(pruned);
            } else if constexpr (
                std::is_same_v<T, Bsdf::DielectricInterfaceData>) {
                if (IsEffectivelyDeltaAlpha(data.roughness)) {
                    return Bsdf::InvalidNodeId;
                }
                Bsdf::DielectricInterfaceData pruned = data;
                pruned.transmissionWeight = 0.0f;
                if (pruned.reflectionWeight <= kEpsilon) {
                    return Bsdf::InvalidNodeId;
                }
                return _result.Add(pruned);
            } else if constexpr (std::is_same_v<T, Bsdf::ConductorData>) {
                if (IsEffectivelyDeltaAlpha(data.roughness)) {
                    return Bsdf::InvalidNodeId;
                }
                return _result.Add(data);
            } else if constexpr (
                std::is_same_v<T, Bsdf::GeneralizedSchlickData>) {
                if (IsEffectivelyDeltaAlpha(data.roughness) ||
                    data.scatterMode == Bsdf::ScatterMode::Transmission) {
                    return Bsdf::InvalidNodeId;
                }
                Bsdf::GeneralizedSchlickData pruned = data;
                if (pruned.scatterMode ==
                    Bsdf::ScatterMode::ReflectionTransmission) {
                    pruned.scatterMode = Bsdf::ScatterMode::Reflection;
                }
                return _result.Add(pruned);
            } else if constexpr (std::is_same_v<T, Bsdf::AdobeOpenPbrData>) {
                Bsdf::AdobeOpenPbrData pruned = data;
                pruned.transmissionWeight = 0.0f;
                if (IsEffectivelySmoothPerceptualRoughness(
                        pruned.specularRoughness)) {
                    pruned.specularWeight = 0.0f;
                }
                if (IsEffectivelySmoothPerceptualRoughness(
                        pruned.coatRoughness)) {
                    pruned.coatWeight = 0.0f;
                }
                return _result.Add(pruned);
            } else if constexpr (std::is_same_v<T, Bsdf::MixData>) {
                const float mix = Clamp01(data.mix);
                const Bsdf::NodeId bg = PruneNode(data.bg);
                const Bsdf::NodeId fg = PruneNode(data.fg);
                if (IsValid(bg) && IsValid(fg)) {
                    Bsdf::MixData pruned = data;
                    pruned.bg = bg;
                    pruned.fg = fg;
                    pruned.mix = mix;
                    return _result.Add(pruned);
                }
                if (IsValid(bg)) {
                    return ScaleNode(bg, 1.0f - mix);
                }
                if (IsValid(fg)) {
                    return ScaleNode(fg, mix);
                }
                return Bsdf::InvalidNodeId;
            } else if constexpr (std::is_same_v<T, Bsdf::LayerData>) {
                const Bsdf::NodeId top = PruneNode(data.top);
                const Bsdf::NodeId base = PruneNode(data.base);
                if (IsValid(top) && IsValid(base)) {
                    Bsdf::LayerData pruned;
                    pruned.top = top;
                    pruned.base = base;
                    return _result.Add(pruned);
                }
                return IsValid(top) ? top : base;
            } else if constexpr (std::is_same_v<T, Bsdf::AddData>) {
                const Bsdf::NodeId in1 = PruneNode(data.in1);
                const Bsdf::NodeId in2 = PruneNode(data.in2);
                if (IsValid(in1) && IsValid(in2)) {
                    Bsdf::AddData pruned;
                    pruned.in1 = in1;
                    pruned.in2 = in2;
                    return _result.Add(pruned);
                }
                return IsValid(in1) ? in1 : in2;
            } else if constexpr (std::is_same_v<T, Bsdf::MultiplyData>) {
                const Bsdf::NodeId input = PruneNode(data.input);
                if (!IsValid(input) || luminance(data.weight) <= kEpsilon) {
                    return Bsdf::InvalidNodeId;
                }
                Bsdf::MultiplyData pruned = data;
                pruned.input = input;
                return _result.Add(pruned);
            } else {
                return Bsdf::InvalidNodeId;
            }
        }, node->data);
    }

    bool IsValid(Bsdf::NodeId nodeId) const
    {
        return _result.IsValid(nodeId);
    }

    Bsdf::NodeId ScaleNode(Bsdf::NodeId nodeId, float weight)
    {
        if (!IsValid(nodeId) || weight <= kEpsilon) {
            return Bsdf::InvalidNodeId;
        }
        if (weight >= 1.0f - kEpsilon) {
            return nodeId;
        }
        Bsdf::MultiplyData multiply;
        multiply.input = nodeId;
        multiply.weight = Vec3f(weight);
        return _result.Add(multiply);
    }

    const Bsdf::ClosureTree& _source;
    Bsdf::ClosureTree _result;
};

static Vec3f
_EvalLayerBaseThroughput(const Bsdf::ClosureTree& tree, Bsdf::NodeId topNodeId,
                         const Vec3f& normalShdWldOut, const Vec3f& omegaOutWld,
                         float heroWavelengthNm)
{
    return _EvalThroughput(tree, topNodeId, normalShdWldOut, omegaOutWld,
                           heroWavelengthNm);
}

Vec3f
EvalNode(const Bsdf::ClosureTree& tree, Bsdf::NodeId nodeId,
          const Vec3f& normalShdWldOut, const Vec3f& omegaInWld,
          const Vec3f& omegaOutWld, float heroWavelengthNm)
{
    const auto luminance = [&](const Vec3f& value) {
        return Bsdf::detail::Luminance(value, tree.luminanceCoefficients);
    };
    const Bsdf::Node* node = tree.Get(nodeId);
    if (!node) {
        return Vec3f(0.0f);
    }

    return std::visit([&](const auto& data) -> Vec3f {
        using T = std::decay_t<decltype(data)>;
        if constexpr (std::is_same_v<T, Bsdf::OrenNayarDiffuseData>) {
            const Vec3f normalShdLobeWldOut =
                ResolveReflectionNormal(data, normalShdWldOut, omegaOutWld);
            if (Dot(normalShdLobeWldOut, omegaInWld) <= 0.0f ||
                data.weight <= 0.0f) {
                return Vec3f(0.0f);
            }
            float NdotL = std::max(Dot(normalShdLobeWldOut, omegaInWld), 0.0f);
            float NdotV =
                std::max(Dot(normalShdLobeWldOut, omegaOutWld), kEpsilon);
            float LdotV = Dot(omegaInWld, omegaOutWld);
            if (data.energyCompensation) {
                return SafeVec(
                    EvalEonDiffuse(
                        data.color, data.roughness, NdotV, NdotL, LdotV) *
                    data.weight);
            }
            float factor = OrenNayarFactor(
                NdotV, NdotL, LdotV, data.roughness);
            return SafeVec(data.color * (data.weight * factor * kInvPi));
        } else if constexpr (std::is_same_v<T, Bsdf::BurleyDiffuseData>) {
            const Vec3f normalShdLobeWldOut =
                ResolveReflectionNormal(data, normalShdWldOut, omegaOutWld);
            if (Dot(normalShdLobeWldOut, omegaInWld) <= 0.0f ||
                data.weight <= 0.0f) {
                return Vec3f(0.0f);
            }
            float NdotL = std::max(Dot(normalShdLobeWldOut, omegaInWld), 0.0f);
            float NdotV =
                std::max(Dot(normalShdLobeWldOut, omegaOutWld), kEpsilon);
            Vec3f H = (omegaInWld + omegaOutWld).normalized();
            float LdotH = std::max(Dot(omegaInWld, H), 0.0f);
            float factor = BurleyFactor(NdotV, NdotL, LdotH, data.roughness);
            return SafeVec(data.color * (data.weight * factor * kInvPi));
        } else if constexpr (std::is_same_v<T, Bsdf::TranslucentData>) {
            const Vec3f normalShdLobeWldOut =
                ResolveReflectionNormal(data, normalShdWldOut, omegaOutWld);
            return SafeVec(EvalTranslucent(data.color, data.weight,
                                             normalShdLobeWldOut, omegaInWld));
        } else if constexpr (std::is_same_v<T, Bsdf::SubsurfaceData>) {
            return Vec3f(0.0f);
        } else if constexpr (std::is_same_v<T, Bsdf::DielectricData>) {
            Vec3f result(0.0f);
            bool sameSide =
                IsSameSide(normalShdWldOut, omegaInWld, omegaOutWld);
            const Vec3f normalShdLobeWldOut =
                ResolveReflectionNormal(data, normalShdWldOut, omegaOutWld);
            const float effectiveIor =
                ResolveDielectricIor(data, heroWavelengthNm);
            if (IsEffectivelyDeltaAlpha(data.roughness)) {
                return Vec3f(0.0f);
            }
            if (sameSide &&
                data.scatterMode != Bsdf::ScatterMode::Transmission) {
                const Vec3f fresnel = DielectricReflectionFresnel(
                    data, ReflectionFresnelCosTheta(omegaInWld, omegaOutWld),
                    effectiveIor);
                if (IsEffectivelyIsotropic(data.roughness)) {
                    result += EvalMicrofacetReflectionIsotropic(
                        std::clamp(data.roughness[0], kMinMicrofacetAlpha,
                                   1.0f),
                        fresnel, data.weight, normalShdLobeWldOut, omegaInWld,
                        omegaOutWld);
                } else {
                    result += EvalMicrofacetReflectionAnisotropic(
                        data.roughness, data.tangent, fresnel, data.weight,
                        normalShdLobeWldOut, omegaInWld, omegaOutWld);
                }
            }
            if (!sameSide &&
                data.scatterMode != Bsdf::ScatterMode::Reflection) {
                const float fresnelCos = TransmissionFresnelCosTheta(
                    effectiveIor, normalShdWldOut, omegaInWld, omegaOutWld);
                const float baseReflectance =
                    SchlickFresnelScalar(effectiveIor, fresnelCos);
                const Vec3f transmissionScale = TransmissionScale(
                    baseReflectance,
                    DielectricReflectionFresnelUntinted(
                        data, fresnelCos, effectiveIor));
                result += CompMul(Bsdf::EvalGGXTransmission(
                                      AverageAlphaAsRoughness(data.roughness),
                                      effectiveIor, data.tint, normalShdWldOut,
                                      omegaInWld, omegaOutWld) *
                                      data.weight,
                                  transmissionScale);
            }
            return SafeVec(result);
        } else if constexpr (
            std::is_same_v<T, Bsdf::DielectricInterfaceData>) {
            Vec3f result(0.0f);
            const bool sameSide =
                IsSameSide(normalShdWldOut, omegaInWld, omegaOutWld);
            const Vec3f normalShdLobeWldOut =
                ResolveReflectionNormal(data, normalShdWldOut, omegaOutWld);
            const float effectiveIor =
                ResolveDielectricIor(data, heroWavelengthNm);
            if (IsEffectivelyDeltaAlpha(data.roughness)) {
                return Vec3f(0.0f);
            }
            const bool compensateCoupledDielectric =
                UsesCoupledRoughDielectricSampling(data);
            if (sameSide && data.reflectionWeight > 0.0f) {
                const Vec3f fresnel = DielectricInterfaceReflectionCoefficient(
                    data, ReflectionFresnelCosTheta(omegaInWld, omegaOutWld),
                    effectiveIor, Dot(normalShdWldOut, omegaOutWld) < 0.0f);
                if (IsEffectivelyIsotropic(data.roughness)) {
                    result += EvalMicrofacetReflectionIsotropic(
                        std::clamp(data.roughness[0], kMinMicrofacetAlpha,
                                   1.0f),
                        fresnel, 1.0f, normalShdLobeWldOut, omegaInWld,
                        omegaOutWld, !compensateCoupledDielectric);
                } else {
                    result += EvalMicrofacetReflectionAnisotropic(
                        data.roughness, data.tangent, fresnel, 1.0f,
                        normalShdLobeWldOut, omegaInWld, omegaOutWld,
                        !compensateCoupledDielectric);
                }
            }
            if (!sameSide && data.transmissionWeight > 0.0f) {
                if (data.thinWalled) {
                    const float NdotV = std::max(
                        std::abs(Dot(normalShdLobeWldOut, omegaOutWld)),
                        kEpsilon);
                    const Vec3f transmission =
                        DielectricInterfaceTransmissionCoefficient(
                            data, NdotV, effectiveIor,
                            /* backside = */ false);
                    const Vec3f transmissionN = -normalShdLobeWldOut;
                    Vec3f omegaOutMirroredWld =
                        MirrorAcrossSurface(omegaOutWld, normalShdLobeWldOut);
                    omegaOutMirroredWld.normalize();
                    if (IsEffectivelyIsotropic(data.roughness)) {
                        result += EvalMicrofacetReflectionIsotropic(
                            std::clamp(data.roughness[0], kMinMicrofacetAlpha,
                                       1.0f),
                            transmission, 1.0f, transmissionN, omegaInWld,
                            omegaOutMirroredWld);
                    } else {
                        result += EvalMicrofacetReflectionAnisotropic(
                            data.roughness, data.tangent, transmission, 1.0f,
                            transmissionN, omegaInWld, omegaOutMirroredWld);
                    }
                    return SafeVec(result);
                }

                if (compensateCoupledDielectric) {
                    result += EvalCoupledRoughDielectricTransmission(
                        data, effectiveIor, normalShdWldOut,
                        normalShdLobeWldOut, omegaInWld, omegaOutWld);
                } else {
                    const float fresnelCos = TransmissionFresnelCosTheta(
                        effectiveIor, normalShdWldOut, omegaInWld, omegaOutWld);
                    const float baseReflectance =
                        SchlickFresnelScalar(effectiveIor, fresnelCos);
                    const Vec3f transmissionScale = TransmissionScale(
                        baseReflectance,
                        DielectricInterfaceReflectanceUntinted(
                            data, fresnelCos, effectiveIor,
                            Dot(normalShdWldOut, omegaOutWld) < 0.0f));
                    result +=
                        CompMul(Bsdf::EvalGGXTransmission(
                                    AverageAlphaAsRoughness(data.roughness),
                                    effectiveIor, data.transmissionTint,
                                    normalShdWldOut, omegaInWld, omegaOutWld) *
                                    Clamp01(data.transmissionWeight),
                                transmissionScale);
                }
            }
            if (compensateCoupledDielectric) {
                const CoupledDielectricCompensation compensation =
                    GetCoupledDielectricCompensation(
                        data, effectiveIor, normalShdWldOut,
                        normalShdLobeWldOut, omegaOutWld);
                if (compensation.missingEnergy > 0.0f) {
                    const float sideRatio = sameSide
                        ? compensation.reflectionRatio
                        : 1.0f - compensation.reflectionRatio;
                    const Vec3f tint = sameSide
                        ? SafeVec(data.reflectionTint) *
                            Clamp01(data.reflectionWeight)
                        : SafeVec(data.transmissionTint) *
                            Clamp01(data.transmissionWeight);
                    result += tint *
                        (compensation.missingEnergy * sideRatio * kInvPi);
                }
            }
            return SafeVec(result);
        } else if constexpr (std::is_same_v<T, Bsdf::ConductorData>) {
            const Vec3f normalShdLobeWldOut =
                ResolveReflectionNormal(data, normalShdWldOut, omegaOutWld);
            if (Dot(normalShdLobeWldOut, omegaInWld) <= 0.0f ||
                data.weight <= 0.0f) {
                return Vec3f(0.0f);
            }
            if (IsEffectivelyDeltaAlpha(data.roughness)) {
                return Vec3f(0.0f);
            }
            const Vec3f fresnel = ConductorReflectionFresnel(
                data, ReflectionFresnelCosTheta(omegaInWld, omegaOutWld));
            if (IsEffectivelyIsotropic(data.roughness)) {
                return EvalMicrofacetReflectionIsotropic(
                    std::clamp(data.roughness[0], kMinMicrofacetAlpha, 1.0f),
                    fresnel, data.weight, normalShdLobeWldOut, omegaInWld,
                    omegaOutWld);
            }
            return EvalMicrofacetReflectionAnisotropic(
                data.roughness, data.tangent, fresnel, data.weight,
                normalShdLobeWldOut, omegaInWld, omegaOutWld);
        } else if constexpr (std::is_same_v<T, Bsdf::GeneralizedSchlickData>) {
            Vec3f result(0.0f);
            bool sameSide =
                IsSameSide(normalShdWldOut, omegaInWld, omegaOutWld);
            const Vec3f normalShdLobeWldOut =
                ResolveReflectionNormal(data, normalShdWldOut, omegaOutWld);
            if (IsEffectivelyDeltaAlpha(data.roughness)) {
                return Vec3f(0.0f);
            }
            if (sameSide &&
                data.scatterMode != Bsdf::ScatterMode::Transmission) {
                const Vec3f fresnel = GeneralizedSchlickReflectionFresnel(
                    data, ReflectionFresnelCosTheta(omegaInWld, omegaOutWld));
                if (IsEffectivelyIsotropic(data.roughness)) {
                    result += EvalMicrofacetReflectionIsotropic(
                        std::clamp(data.roughness[0], kMinMicrofacetAlpha,
                                   1.0f),
                        fresnel, data.weight, normalShdLobeWldOut, omegaInWld,
                        omegaOutWld);
                } else {
                    result += EvalMicrofacetReflectionAnisotropic(
                        data.roughness, data.tangent, fresnel, data.weight,
                        normalShdLobeWldOut, omegaInWld, omegaOutWld);
                }
            }
            if (!sameSide &&
                data.scatterMode != Bsdf::ScatterMode::Reflection) {
                float avgF0 = Clamp01(luminance(Clamp01(data.color0)));
                float ior = (1.0f + std::sqrt(std::max(avgF0, 0.01f))) /
                            (1.0f - std::sqrt(std::max(avgF0, 0.01f)));
                const float fresnelCos = TransmissionFresnelCosTheta(
                    ior, normalShdWldOut, omegaInWld, omegaOutWld);
                const float baseReflectance =
                    SchlickFresnelScalar(ior, fresnelCos);
                const Vec3f transmissionScale = TransmissionScale(
                    baseReflectance,
                    GeneralizedSchlickReflectionFresnel(data, fresnelCos));
                result += CompMul(Bsdf::EvalGGXTransmission(
                                      AverageAlphaAsRoughness(data.roughness),
                                      ior, Vec3f(1.0f), normalShdWldOut,
                                      omegaInWld, omegaOutWld) *
                                      data.weight,
                                  transmissionScale);
            }
            return SafeVec(result);
        } else if constexpr (std::is_same_v<T, Bsdf::SheenData>) {
            const Vec3f normalShdLobeWldOut =
                ResolveReflectionNormal(data, normalShdWldOut, omegaOutWld);
            if (Dot(normalShdLobeWldOut, omegaInWld) <= 0.0f ||
                data.weight <= 0.0f) {
                return Vec3f(0.0f);
            }
            return SafeVec(Bsdf::EvalSheen(data.color, data.roughness,
                                            normalShdLobeWldOut, omegaInWld,
                                            omegaOutWld) *
                            data.weight);
        } else if constexpr (std::is_same_v<T, Bsdf::AdobeOpenPbrData>) {
            return SafeVec(EvalAdobeOpenPbr(data, normalShdWldOut, omegaInWld,
                                             omegaOutWld));
        } else if constexpr (std::is_same_v<T, Bsdf::UnsupportedData>) {
            return Vec3f(0.0f);
        } else if constexpr (std::is_same_v<T, Bsdf::MixData>) {
            return LerpVec(
                EvalNode(tree, data.bg, normalShdWldOut, omegaInWld,
                          omegaOutWld, heroWavelengthNm),
                EvalNode(tree, data.fg, normalShdWldOut, omegaInWld,
                          omegaOutWld, heroWavelengthNm),
                Clamp01(data.mix));
        } else if constexpr (std::is_same_v<T, Bsdf::LayerData>) {
            Vec3f topEval =
                EvalNode(tree, data.top, normalShdWldOut, omegaInWld,
                          omegaOutWld, heroWavelengthNm);
            Vec3f baseEval =
                EvalNode(tree, data.base, normalShdWldOut, omegaInWld,
                          omegaOutWld, heroWavelengthNm);
            return topEval +
                   CompMul(baseEval, _EvalLayerBaseThroughput(
                                         tree, data.top, normalShdWldOut,
                                         omegaOutWld, heroWavelengthNm));
        } else if constexpr (std::is_same_v<T, Bsdf::AddData>) {
            return EvalNode(tree, data.in1, normalShdWldOut, omegaInWld,
                             omegaOutWld, heroWavelengthNm) +
                   EvalNode(tree, data.in2, normalShdWldOut, omegaInWld,
                             omegaOutWld, heroWavelengthNm);
        } else if constexpr (std::is_same_v<T, Bsdf::MultiplyData>) {
            return CompMul(data.weight,
                           EvalNode(tree, data.input, normalShdWldOut,
                                     omegaInWld, omegaOutWld,
                                     heroWavelengthNm));
        } else {
            return Vec3f(0.0f);
        }
    }, node->data);
}

static Vec3f
_EvalThroughput(const Bsdf::ClosureTree& tree, Bsdf::NodeId nodeId,
                const Vec3f& normalShdWldOut, const Vec3f& omegaOutWld,
                float heroWavelengthNm)
{
    const Bsdf::Node* node = tree.Get(nodeId);
    if (!node) {
        return Vec3f(0.0f);
    }

    return std::visit([&](const auto& data) -> Vec3f {
        using T = std::decay_t<decltype(data)>;
        if constexpr (std::is_same_v<T, Bsdf::OrenNayarDiffuseData> ||
                      std::is_same_v<T, Bsdf::BurleyDiffuseData> ||
                      std::is_same_v<T, Bsdf::TranslucentData> ||
                      std::is_same_v<T, Bsdf::SubsurfaceData> ||
                      std::is_same_v<T, Bsdf::UnsupportedData>) {
            return Vec3f(0.0f);
        } else if constexpr (std::is_same_v<T, Bsdf::DielectricData>) {
            const Vec3f normalShdLobeWldOut =
                ResolveReflectionNormal(data, normalShdWldOut, omegaOutWld);
            const float NdotV = std::max(
                std::abs(Dot(normalShdLobeWldOut, omegaOutWld)), kEpsilon);
            const float effectiveIor =
                ResolveDielectricIor(data, heroWavelengthNm);
            Vec3f throughputRgb(1.0f);
            if (data.scatterMode != Bsdf::ScatterMode::Transmission) {
                const bool useDirectionalLayerThroughput =
                    IsGgxMultipleScatteringStateEnabled() &&
                    !HasThinFilm(
                        data.thinFilmWeight,
                        data.thinFilmThickness,
                        data.thinFilmIor);
                if (useDirectionalLayerThroughput) {
                    if (GetDielectricThroughputModeState() ==
                        Bsdf::DielectricLayerThroughputMode::MaterialXGlsl) {
                        const Vec3f reflectance =
                            MaterialXGlslDielectricLayerReflectance(
                                AverageAlphaForEnergy(data.roughness),
                                NdotV,
                                effectiveIor);
                        throughputRgb -= reflectance * data.weight;
                    } else {
                        const float filter =
                            LookupBsdlDielectricReflFrontFilter(
                                NdotV,
                                BsdlLayerRoughnessFromAlpha(data.roughness),
                                effectiveIor);
                        throughputRgb = LerpVec(Vec3f(1.0f), Vec3f(filter),
                                                 Clamp01(data.weight));
                    }
                } else {
                    const Vec3f reflectance = LayerThroughputReflectance(
                        AverageAlphaForEnergy(data.roughness),
                        NdotV,
                        DielectricReflectionFresnelUntinted(
                            data, NdotV, effectiveIor));
                    throughputRgb -= reflectance * data.weight;
                }
            }
            return Clamp01(throughputRgb);
        } else if constexpr (
            std::is_same_v<T, Bsdf::DielectricInterfaceData>) {
            const Vec3f normalShdLobeWldOut =
                ResolveReflectionNormal(data, normalShdWldOut, omegaOutWld);
            const float NdotV = std::max(
                std::abs(Dot(normalShdLobeWldOut, omegaOutWld)), kEpsilon);
            const float effectiveIor =
                ResolveDielectricIor(data, heroWavelengthNm);
            Vec3f throughputRgb(1.0f);
            if (data.reflectionWeight > 0.0f) {
                const bool useDirectionalLayerThroughput =
                    IsGgxMultipleScatteringStateEnabled() &&
                    !HasThinFilm(
                        data.thinFilmWeight,
                        data.thinFilmThickness,
                        data.thinFilmIor);
                if (useDirectionalLayerThroughput) {
                    if (GetDielectricThroughputModeState() ==
                        Bsdf::DielectricLayerThroughputMode::MaterialXGlsl) {
                        const Vec3f reflectance =
                            MaterialXGlslDielectricLayerReflectance(
                                AverageAlphaForEnergy(data.roughness),
                                NdotV,
                                effectiveIor);
                        throughputRgb -=
                            reflectance * Clamp01(data.reflectionWeight);
                    } else {
                        const float filter =
                            LookupBsdlDielectricReflFrontFilter(
                                NdotV,
                                BsdlLayerRoughnessFromAlpha(data.roughness),
                                effectiveIor);
                        throughputRgb =
                            LerpVec(Vec3f(1.0f), Vec3f(filter),
                                     Clamp01(data.reflectionWeight));
                    }
                } else {
                    const Vec3f reflectance = LayerThroughputReflectance(
                        AverageAlphaForEnergy(data.roughness), NdotV,
                        DielectricInterfaceReflectanceUntinted(
                            data, NdotV, effectiveIor,
                            Dot(normalShdWldOut, omegaOutWld) < 0.0f));
                    throughputRgb -=
                        reflectance * Clamp01(data.reflectionWeight);
                }
            }
            return Clamp01(throughputRgb);
        } else if constexpr (std::is_same_v<T, Bsdf::ConductorData>) {
            const Vec3f normalShdLobeWldOut =
                ResolveReflectionNormal(data, normalShdWldOut, omegaOutWld);
            const float NdotV = std::max(
                std::abs(Dot(normalShdLobeWldOut, omegaOutWld)), kEpsilon);
            const Vec3f reflectance = LayerThroughputReflectance(
                AverageAlphaForEnergy(data.roughness),
                NdotV,
                ConductorReflectionFresnel(data, NdotV));
            return Clamp01(
                Vec3f(1.0f) -
                reflectance * data.weight);
        } else if constexpr (std::is_same_v<T, Bsdf::GeneralizedSchlickData>) {
            const Vec3f normalShdLobeWldOut =
                ResolveReflectionNormal(data, normalShdWldOut, omegaOutWld);
            const float NdotV = std::max(
                std::abs(Dot(normalShdLobeWldOut, omegaOutWld)), kEpsilon);
            const Vec3f reflectance = LayerThroughputReflectance(
                AverageAlphaForEnergy(data.roughness),
                NdotV,
                GeneralizedSchlickReflectionFresnel(data, NdotV));
            return Clamp01(
                Vec3f(1.0f) -
                reflectance * data.weight);
        } else if constexpr (std::is_same_v<T, Bsdf::SheenData>) {
            const Vec3f normalShdLobeWldOut =
                ResolveReflectionNormal(data, normalShdWldOut, omegaOutWld);
            float NdotV = std::max(
                std::abs(Dot(normalShdLobeWldOut, omegaOutWld)), kEpsilon);
            float dirAlbedo = ApproxSheenDirAlbedo(NdotV, data.roughness);
            return Clamp01(Vec3f(1.0f - dirAlbedo * data.weight));
        } else if constexpr (std::is_same_v<T, Bsdf::AdobeOpenPbrData>) {
            return Vec3f(1.0f);
        } else if constexpr (std::is_same_v<T, Bsdf::MixData>) {
            return LerpVec(_EvalThroughput(tree, data.bg, normalShdWldOut,
                                            omegaOutWld, heroWavelengthNm),
                            _EvalThroughput(tree, data.fg, normalShdWldOut,
                                            omegaOutWld, heroWavelengthNm),
                            Clamp01(data.mix));
        } else if constexpr (std::is_same_v<T, Bsdf::LayerData>) {
            return CompMul(_EvalThroughput(tree, data.top, normalShdWldOut,
                                           omegaOutWld, heroWavelengthNm),
                           _EvalThroughput(tree, data.base, normalShdWldOut,
                                           omegaOutWld, heroWavelengthNm));
        } else if constexpr (std::is_same_v<T, Bsdf::AddData>) {
            Vec3f t = _EvalThroughput(tree, data.in1, normalShdWldOut,
                                      omegaOutWld, heroWavelengthNm) +
                      _EvalThroughput(tree, data.in2, normalShdWldOut,
                                      omegaOutWld, heroWavelengthNm) -
                      Vec3f(1.0f);
            return Vec3f(std::max(t[0], 0.0f),
                         std::max(t[1], 0.0f),
                         std::max(t[2], 0.0f));
        } else if constexpr (std::is_same_v<T, Bsdf::MultiplyData>) {
            return _EvalThroughput(tree, data.input, normalShdWldOut,
                                   omegaOutWld, heroWavelengthNm);
        } else {
            return Vec3f(0.0f);
        }
    }, node->data);
}

static float
_ApproxWeight(const Bsdf::ClosureTree& tree, Bsdf::NodeId nodeId,
              const Vec3f& normalShdWldOut, const Vec3f& omegaOutWld,
              float heroWavelengthNm)
{
    const auto luminance = [&](const Vec3f& value) {
        return Bsdf::detail::Luminance(value, tree.luminanceCoefficients);
    };
    const Bsdf::Node* node = tree.Get(nodeId);
    if (!node) {
        return 0.0f;
    }

    return std::visit([&](const auto& data) -> float {
        using T = std::decay_t<decltype(data)>;
        if constexpr (std::is_same_v<T, Bsdf::OrenNayarDiffuseData> ||
                      std::is_same_v<T, Bsdf::BurleyDiffuseData>) {
            return data.weight * std::max(luminance(data.color), 0.0f);
        } else if constexpr (std::is_same_v<T, Bsdf::TranslucentData>) {
            return data.weight * std::max(luminance(data.color), 0.0f);
        } else if constexpr (std::is_same_v<T, Bsdf::SubsurfaceData>) {
            return data.weight * std::max(luminance(data.color), 0.0f);
        } else if constexpr (std::is_same_v<T, Bsdf::DielectricData>) {
            const Vec3f normalShdLobeWldOut =
                ResolveReflectionNormal(data, normalShdWldOut, omegaOutWld);
            const float NdotV = std::max(
                std::abs(Dot(normalShdLobeWldOut, omegaOutWld)), kEpsilon);
            const float effectiveIor =
                ResolveDielectricIor(data, heroWavelengthNm);
            const Vec3f reflectance =
                DielectricReflectionFresnelUntinted(
                    data, NdotV, effectiveIor);
            const float baseReflectance =
                SchlickFresnelScalar(effectiveIor, NdotV);
            const Vec3f transmissionScale = TransmissionScale(
                baseReflectance,
                reflectance);
            if (data.scatterMode == Bsdf::ScatterMode::Reflection) {
                return std::max(luminance(reflectance) * data.weight, 0.05f);
            }
            if (data.scatterMode == Bsdf::ScatterMode::Transmission) {
                return std::max(
                    data.weight *
                        luminance(CompMul(data.tint, transmissionScale)),
                    0.05f);
            }
            return std::max(
                data.weight *
                    (luminance(reflectance) +
                     luminance(CompMul(data.tint, transmissionScale))),
                0.05f);
        } else if constexpr (
            std::is_same_v<T, Bsdf::DielectricInterfaceData>) {
            const Vec3f normalShdLobeWldOut =
                ResolveReflectionNormal(data, normalShdWldOut, omegaOutWld);
            const float NdotV = std::max(
                std::abs(Dot(normalShdLobeWldOut, omegaOutWld)), kEpsilon);
            const float effectiveIor =
                ResolveDielectricIor(data, heroWavelengthNm);
            const Vec3f reflection = DielectricInterfaceReflectionCoefficient(
                data, NdotV, effectiveIor,
                Dot(normalShdWldOut, omegaOutWld) < 0.0f);
            const Vec3f transmission =
                DielectricInterfaceTransmissionCoefficient(
                    data, NdotV, effectiveIor,
                    Dot(normalShdWldOut, omegaOutWld) < 0.0f);
            const float weight =
                luminance(reflection) + luminance(transmission);
            return weight > 0.0f ? std::max(weight, 0.05f) : 0.0f;
        } else if constexpr (std::is_same_v<T, Bsdf::ConductorData>) {
            const Vec3f normalShdLobeWldOut =
                ResolveReflectionNormal(data, normalShdWldOut, omegaOutWld);
            return data.weight *
                   std::max(luminance(ConductorReflectionFresnel(
                                data, std::max(std::abs(Dot(normalShdLobeWldOut,
                                                            omegaOutWld)),
                                               kEpsilon))),
                            0.05f);
        } else if constexpr (std::is_same_v<T, Bsdf::GeneralizedSchlickData>) {
            const Vec3f normalShdLobeWldOut =
                ResolveReflectionNormal(data, normalShdWldOut, omegaOutWld);
            const float NdotV = std::max(
                std::abs(Dot(normalShdLobeWldOut, omegaOutWld)), kEpsilon);
            const Vec3f reflectance = GeneralizedSchlickReflectionFresnel(
                data,
                NdotV);
            if (data.scatterMode == Bsdf::ScatterMode::Transmission) {
                const float avgF0 =
                    Clamp01(luminance(Clamp01(data.color0)));
                const float sqrtF0 = std::sqrt(std::max(avgF0, 0.01f));
                const float ior = (1.0f + sqrtF0) / (1.0f - sqrtF0);
                return std::max(
                    data.weight *
                        luminance(TransmissionScale(
                            SchlickFresnelScalar(ior, NdotV),
                            reflectance)),
                    0.05f);
            }
            if (data.scatterMode == Bsdf::ScatterMode::ReflectionTransmission) {
                const float avgF0 =
                    Clamp01(luminance(Clamp01(data.color0)));
                const float sqrtF0 = std::sqrt(std::max(avgF0, 0.01f));
                const float ior = (1.0f + sqrtF0) / (1.0f - sqrtF0);
                return std::max(
                    data.weight *
                        (luminance(reflectance) +
                         luminance(TransmissionScale(
                             SchlickFresnelScalar(ior, NdotV),
                             reflectance))),
                    0.05f);
            }
            return data.weight *
                std::max(luminance(reflectance), 0.05f);
        } else if constexpr (std::is_same_v<T, Bsdf::SheenData>) {
            return data.weight * std::max(luminance(data.color), 0.0f) * 0.25f;
        } else if constexpr (std::is_same_v<T, Bsdf::AdobeOpenPbrData>) {
            return 1.0f;
        } else if constexpr (std::is_same_v<T, Bsdf::UnsupportedData>) {
            return 0.0f;
        } else if constexpr (std::is_same_v<T, Bsdf::MixData>) {
            return LerpVec(Vec3f(_ApproxWeight(tree, data.bg, normalShdWldOut,
                                                omegaOutWld, heroWavelengthNm)),
                            Vec3f(_ApproxWeight(tree, data.fg, normalShdWldOut,
                                                omegaOutWld, heroWavelengthNm)),
                            Clamp01(data.mix))[0];
        } else if constexpr (std::is_same_v<T, Bsdf::LayerData>) {
            return _ApproxWeight(tree, data.top, normalShdWldOut, omegaOutWld,
                                 heroWavelengthNm) +
                   _ApproxWeight(tree, data.base, normalShdWldOut, omegaOutWld,
                                 heroWavelengthNm) *
                       luminance(_EvalThroughput(tree, data.top,
                                                  normalShdWldOut, omegaOutWld,
                                                  heroWavelengthNm));
        } else if constexpr (std::is_same_v<T, Bsdf::AddData>) {
            return _ApproxWeight(tree, data.in1, normalShdWldOut, omegaOutWld,
                                 heroWavelengthNm) +
                   _ApproxWeight(tree, data.in2, normalShdWldOut, omegaOutWld,
                                 heroWavelengthNm);
        } else if constexpr (std::is_same_v<T, Bsdf::MultiplyData>) {
            return _ApproxWeight(tree, data.input, normalShdWldOut, omegaOutWld,
                                 heroWavelengthNm) *
                   std::max(luminance(data.weight), 0.0f);
        } else {
            return 0.0f;
        }
    }, node->data);
}

float
PdfNode(const Bsdf::ClosureTree& tree, Bsdf::NodeId nodeId,
         const Vec3f& normalShdWldOut, const Vec3f& omegaInWld,
         const Vec3f& omegaOutWld, float heroWavelengthNm)
{
    const auto luminance = [&](const Vec3f& value) {
        return Bsdf::detail::Luminance(value, tree.luminanceCoefficients);
    };
    const Bsdf::Node* node = tree.Get(nodeId);
    if (!node) {
        return 0.0f;
    }

    return std::visit([&](const auto& data) -> float {
        using T = std::decay_t<decltype(data)>;
        if constexpr (std::is_same_v<T, Bsdf::OrenNayarDiffuseData> ||
                      std::is_same_v<T, Bsdf::BurleyDiffuseData>) {
            const Vec3f normalShdLobeWldOut =
                ResolveReflectionNormal(data, normalShdWldOut, omegaOutWld);
            return (Dot(normalShdLobeWldOut, omegaInWld) > 0.0f)
                       ? Bsdf::PdfLambertian(normalShdLobeWldOut, omegaInWld)
                       : 0.0f;
        } else if constexpr (std::is_same_v<T, Bsdf::TranslucentData>) {
            const Vec3f normalShdLobeWldOut =
                ResolveReflectionNormal(data, normalShdWldOut, omegaOutWld);
            return PdfTranslucent(normalShdLobeWldOut, omegaInWld);
        } else if constexpr (std::is_same_v<T, Bsdf::SubsurfaceData>) {
            return 0.0f;
        } else if constexpr (std::is_same_v<T, Bsdf::DielectricData>) {
            bool sameSide =
                IsSameSide(normalShdWldOut, omegaInWld, omegaOutWld);
            const Vec3f normalShdLobeWldOut =
                ResolveReflectionNormal(data, normalShdWldOut, omegaOutWld);
            const float effectiveIor =
                ResolveDielectricIor(data, heroWavelengthNm);
            if (IsEffectivelyDeltaAlpha(data.roughness)) {
                return 0.0f;
            }
            if (sameSide &&
                data.scatterMode != Bsdf::ScatterMode::Transmission) {
                if (IsEffectivelyIsotropic(data.roughness)) {
                    return Bsdf::PdfGGXSpecular(
                        AverageAlphaAsRoughness(data.roughness),
                        normalShdLobeWldOut, omegaInWld, omegaOutWld);
                }
                return PdfGGXSpecularAnisotropic(data.roughness, data.tangent,
                                                  normalShdLobeWldOut,
                                                  omegaInWld, omegaOutWld);
            }
            if (!sameSide &&
                data.scatterMode != Bsdf::ScatterMode::Reflection) {
                return Bsdf::PdfGGXTransmission(
                    AverageAlphaAsRoughness(data.roughness), effectiveIor,
                    normalShdWldOut, omegaInWld, omegaOutWld);
            }
            return 0.0f;
        } else if constexpr (
            std::is_same_v<T, Bsdf::DielectricInterfaceData>) {
            const bool sameSide =
                IsSameSide(normalShdWldOut, omegaInWld, omegaOutWld);
            const Vec3f normalShdLobeWldOut =
                ResolveReflectionNormal(data, normalShdWldOut, omegaOutWld);
            const float effectiveIor =
                ResolveDielectricIor(data, heroWavelengthNm);
            if (IsEffectivelyDeltaAlpha(data.roughness)) {
                return 0.0f;
            }
            if (UsesCoupledRoughDielectricSampling(data)) {
                return PdfCoupledRoughDielectric(
                    data, effectiveIor, normalShdWldOut, normalShdLobeWldOut,
                    omegaInWld, omegaOutWld);
            }
            const float NdotV = std::max(
                std::abs(Dot(normalShdLobeWldOut, omegaOutWld)), kEpsilon);
            const DielectricInterfaceSelection selection =
                DielectricInterfaceSelectionProbabilities(
                    data, NdotV, effectiveIor,
                    Dot(normalShdWldOut, omegaOutWld) < 0.0f,
                    tree.luminanceCoefficients);
            if (sameSide && data.reflectionWeight > 0.0f) {
                const float branchPdf =
                    IsEffectivelyIsotropic(data.roughness)
                        ? Bsdf::PdfGGXSpecular(
                              AverageAlphaAsRoughness(data.roughness),
                              normalShdLobeWldOut, omegaInWld, omegaOutWld)
                        : PdfGGXSpecularAnisotropic(
                              data.roughness, data.tangent, normalShdLobeWldOut,
                              omegaInWld, omegaOutWld);
                return selection.reflection * branchPdf;
            }
            if (!sameSide && data.transmissionWeight > 0.0f) {
                if (data.thinWalled) {
                    const Vec3f transmissionN = -normalShdLobeWldOut;
                    Vec3f omegaOutMirroredWld =
                        MirrorAcrossSurface(omegaOutWld, normalShdLobeWldOut);
                    omegaOutMirroredWld.normalize();
                    const float branchPdf =
                        IsEffectivelyIsotropic(data.roughness)
                            ? Bsdf::PdfGGXSpecular(
                                  AverageAlphaAsRoughness(data.roughness),
                                  transmissionN, omegaInWld,
                                  omegaOutMirroredWld)
                            : PdfGGXSpecularAnisotropic(
                                  data.roughness, data.tangent, transmissionN,
                                  omegaInWld, omegaOutMirroredWld);
                    return selection.transmission * branchPdf;
                }

                return selection.transmission *
                       Bsdf::PdfGGXTransmission(
                           AverageAlphaAsRoughness(data.roughness),
                           effectiveIor, normalShdWldOut, omegaInWld,
                           omegaOutWld);
            }
            return 0.0f;
        } else if constexpr (std::is_same_v<T, Bsdf::ConductorData>) {
            if (IsEffectivelyDeltaAlpha(data.roughness)) {
                return 0.0f;
            }
            const Vec3f normalShdLobeWldOut =
                ResolveReflectionNormal(data, normalShdWldOut, omegaOutWld);
            return (Dot(normalShdLobeWldOut, omegaInWld) > 0.0f)
                       ? (IsEffectivelyIsotropic(data.roughness)
                              ? Bsdf::PdfGGXSpecular(
                                    AverageAlphaAsRoughness(data.roughness),
                                    normalShdLobeWldOut, omegaInWld,
                                    omegaOutWld)
                              : PdfGGXSpecularAnisotropic(
                                    data.roughness, data.tangent,
                                    normalShdLobeWldOut, omegaInWld,
                                    omegaOutWld))
                       : 0.0f;
        } else if constexpr (std::is_same_v<T, Bsdf::GeneralizedSchlickData>) {
            bool sameSide =
                IsSameSide(normalShdWldOut, omegaInWld, omegaOutWld);
            const Vec3f normalShdLobeWldOut =
                ResolveReflectionNormal(data, normalShdWldOut, omegaOutWld);
            if (IsEffectivelyDeltaAlpha(data.roughness)) {
                return 0.0f;
            }
            if (sameSide &&
                data.scatterMode != Bsdf::ScatterMode::Transmission) {
                if (IsEffectivelyIsotropic(data.roughness)) {
                    return Bsdf::PdfGGXSpecular(
                        AverageAlphaAsRoughness(data.roughness),
                        normalShdLobeWldOut, omegaInWld, omegaOutWld);
                }
                return PdfGGXSpecularAnisotropic(data.roughness, data.tangent,
                                                  normalShdLobeWldOut,
                                                  omegaInWld, omegaOutWld);
            }
            if (!sameSide &&
                data.scatterMode != Bsdf::ScatterMode::Reflection) {
                float avgF0 = Clamp01(luminance(Clamp01(data.color0)));
                float sqrtF0 = std::sqrt(std::max(avgF0, 0.01f));
                float ior = (1.0f + sqrtF0) / (1.0f - sqrtF0);
                return Bsdf::PdfGGXTransmission(
                    AverageAlphaAsRoughness(data.roughness), ior,
                    normalShdWldOut, omegaInWld, omegaOutWld);
            }
            return 0.0f;
        } else if constexpr (std::is_same_v<T, Bsdf::SheenData>) {
            const Vec3f normalShdLobeWldOut =
                ResolveReflectionNormal(data, normalShdWldOut, omegaOutWld);
            return (Dot(normalShdLobeWldOut, omegaInWld) > 0.0f)
                       ? Bsdf::PdfLambertian(normalShdLobeWldOut, omegaInWld)
                       : 0.0f;
        } else if constexpr (std::is_same_v<T, Bsdf::AdobeOpenPbrData>) {
            return PdfAdobeOpenPbr(data, normalShdWldOut, omegaInWld,
                                   omegaOutWld);
        } else if constexpr (std::is_same_v<T, Bsdf::UnsupportedData>) {
            return 0.0f;
        } else if constexpr (std::is_same_v<T, Bsdf::MixData>) {
            float mix = Clamp01(data.mix);
            return (1.0f - mix) * PdfNode(tree, data.bg, normalShdWldOut,
                                           omegaInWld, omegaOutWld,
                                           heroWavelengthNm) +
                   mix * PdfNode(tree, data.fg, normalShdWldOut, omegaInWld,
                                  omegaOutWld, heroWavelengthNm);
        } else if constexpr (std::is_same_v<T, Bsdf::LayerData>) {
            float topWeight = _ApproxWeight(tree, data.top, normalShdWldOut,
                                            omegaOutWld, heroWavelengthNm);
            float baseWeight =
                _ApproxWeight(tree, data.base, normalShdWldOut, omegaOutWld,
                              heroWavelengthNm) *
                luminance(_EvalThroughput(tree, data.top, normalShdWldOut,
                                           omegaOutWld, heroWavelengthNm));
            float total = topWeight + baseWeight;
            if (total <= 0.0f) {
                return 0.0f;
            }
            return (topWeight / total) *
                       PdfNode(tree, data.top, normalShdWldOut, omegaInWld,
                                omegaOutWld, heroWavelengthNm) +
                   (baseWeight / total) *
                       PdfNode(tree, data.base, normalShdWldOut, omegaInWld,
                                omegaOutWld, heroWavelengthNm);
        } else if constexpr (std::is_same_v<T, Bsdf::AddData>) {
            float w1 = _ApproxWeight(tree, data.in1, normalShdWldOut,
                                     omegaOutWld, heroWavelengthNm);
            float w2 = _ApproxWeight(tree, data.in2, normalShdWldOut,
                                     omegaOutWld, heroWavelengthNm);
            float total = w1 + w2;
            if (total <= 0.0f) {
                return 0.0f;
            }
            return (w1 / total) * PdfNode(tree, data.in1, normalShdWldOut,
                                           omegaInWld, omegaOutWld,
                                           heroWavelengthNm) +
                   (w2 / total) * PdfNode(tree, data.in2, normalShdWldOut,
                                           omegaInWld, omegaOutWld,
                                           heroWavelengthNm);
        } else if constexpr (std::is_same_v<T, Bsdf::MultiplyData>) {
            return PdfNode(tree, data.input, normalShdWldOut, omegaInWld,
                            omegaOutWld, heroWavelengthNm);
        } else {
            return 0.0f;
        }
    }, node->data);
}

static Bsdf::BsdfSample
_FinalizeSubtreeSample(const Bsdf::ClosureTree& tree, Bsdf::NodeId nodeId,
                       const Vec3f& normalShdWldOut, const Vec3f& omegaOutWld,
                       Bsdf::BsdfSample sample, float heroWavelengthNm)
{
    if (sample.isSubsurface) {
        sample.pdfSolidAngle = std::max(sample.pdfSolidAngle, 1.0f);
        return sample;
    }
    if (sample.pdfSolidAngle <= 0.0f) {
        return sample;
    }
    if (sample.isSpecular) {
        sample.pdfSolidAngle = std::max(sample.pdfSolidAngle, 1.0f);
        return sample;
    }
    sample.bsdfValue =
        EvalNode(tree, nodeId, normalShdWldOut, sample.omegaInWld, omegaOutWld,
                  heroWavelengthNm);
    sample.pdfSolidAngle =
        PdfNode(tree, nodeId, normalShdWldOut, sample.omegaInWld, omegaOutWld,
                 heroWavelengthNm);
    return sample;
}

Bsdf::BsdfSample
SampleNode(const Bsdf::ClosureTree& tree, Bsdf::NodeId nodeId,
            const Vec3f& normalShdWldOut, const Vec3f& omegaOutWld, float u1,
            float u2, float uChoice, float heroWavelengthNm)
{
    const auto luminance = [&](const Vec3f& value) {
        return Bsdf::detail::Luminance(value, tree.luminanceCoefficients);
    };
    const Bsdf::Node* node = tree.Get(nodeId);
    if (!node) {
        return Bsdf::BsdfSample{Vec3f(0.0f), Vec3f(0.0f), 0.0f, false};
    }

    return std::visit([&](const auto& data) -> Bsdf::BsdfSample {
        using T = std::decay_t<decltype(data)>;
        if constexpr (std::is_same_v<T, Bsdf::OrenNayarDiffuseData>) {
            const Vec3f normalShdLobeWldOut =
                ResolveReflectionNormal(data, normalShdWldOut, omegaOutWld);
            auto sample = Bsdf::SampleLambertian(data.color * data.weight,
                                                 normalShdLobeWldOut,
                                                 omegaOutWld, u1, u2);
            return _FinalizeSubtreeSample(tree, nodeId, normalShdWldOut,
                                          omegaOutWld, sample,
                                          heroWavelengthNm);
        } else if constexpr (std::is_same_v<T, Bsdf::BurleyDiffuseData>) {
            const Vec3f normalShdLobeWldOut =
                ResolveReflectionNormal(data, normalShdWldOut, omegaOutWld);
            auto sample = Bsdf::SampleLambertian(data.color * data.weight,
                                                 normalShdLobeWldOut,
                                                 omegaOutWld, u1, u2);
            return _FinalizeSubtreeSample(tree, nodeId, normalShdWldOut,
                                          omegaOutWld, sample,
                                          heroWavelengthNm);
        } else if constexpr (std::is_same_v<T, Bsdf::TranslucentData>) {
            const Vec3f normalShdLobeWldOut =
                ResolveReflectionNormal(data, normalShdWldOut, omegaOutWld);
            Frame frame = Frame::FromNormal(-normalShdLobeWldOut);
            Vec3f omegaInLocal = SampleCosineHemisphere(u1, u2);
            Vec3f omegaInWld = frame.ToWorld(omegaInLocal);
            Bsdf::BsdfSample sample{
                omegaInWld,
                EvalTranslucent(data.color, data.weight, normalShdLobeWldOut,
                                 omegaInWld),
                CosineHemispherePdf(omegaInLocal[2]), false};
            sample.isDiffuseLike = true;
            return _FinalizeSubtreeSample(tree, nodeId, normalShdWldOut,
                                          omegaOutWld, sample,
                                          heroWavelengthNm);
        } else if constexpr (std::is_same_v<T, Bsdf::SubsurfaceData>) {
            Bsdf::BsdfSample sample{
                Vec3f(0.0f),
                Vec3f(data.weight),
                1.0f,
                false
            };
            sample.isSubsurface = true;
            sample.isDiffuseLike = true;
            return sample;
        } else if constexpr (std::is_same_v<T, Bsdf::DielectricData>) {
            const Vec3f normalShdLobeWldOut =
                ResolveReflectionNormal(data, normalShdWldOut, omegaOutWld);
            const float NdotV = std::max(
                std::abs(Dot(normalShdLobeWldOut, omegaOutWld)), kEpsilon);
            const float effectiveIor =
                ResolveDielectricIor(data, heroWavelengthNm);
            const bool hasDeltaRoughness =
                IsEffectivelyDeltaAlpha(data.roughness);
            if (hasDeltaRoughness &&
                data.scatterMode != Bsdf::ScatterMode::Reflection &&
                WouldTotalInternalReflect(effectiveIor, normalShdWldOut,
                                           omegaOutWld)) {
                if (data.scatterMode == Bsdf::ScatterMode::Transmission) {
                    // Transmission-only lobes are paired with a separate
                    // reflection lobe; the helper splits the TIR energy with
                    // that pair instead of double-counting it.
                    return SampleDeltaDielectricTransmission(
                        data, effectiveIor, NdotV, normalShdWldOut,
                        omegaOutWld, tree.luminanceCoefficients);
                }
                return SampleDeltaTotalInternalReflection(
                    data.weight, normalShdWldOut, omegaOutWld);
            }
            float fresnelProb = Clamp01(luminance(
                DielectricReflectionFresnelUntinted(
                    data, NdotV, effectiveIor)));
            if (data.scatterMode == Bsdf::ScatterMode::Reflection) {
                if (hasDeltaRoughness) {
                    return SampleDeltaDielectricReflection(
                        data, effectiveIor, normalShdLobeWldOut, omegaOutWld);
                }
                if (IsEffectivelyIsotropic(data.roughness)) {
                    auto sample = Bsdf::SampleGGXSpecular(
                        AverageAlphaAsRoughness(data.roughness), effectiveIor,
                        data.tint, normalShdLobeWldOut, omegaOutWld, u1, u2);
                    return _FinalizeSubtreeSample(tree, nodeId, normalShdWldOut,
                                                  omegaOutWld, sample,
                                                  heroWavelengthNm);
                }
                auto sample = SampleGGXSpecularAnisotropic(
                    data.roughness, data.tangent, normalShdLobeWldOut,
                    omegaOutWld, u1, u2);
                return _FinalizeSubtreeSample(tree, nodeId, normalShdWldOut,
                                              omegaOutWld, sample,
                                              heroWavelengthNm);
            }
            if (data.scatterMode == Bsdf::ScatterMode::Transmission) {
                if (hasDeltaRoughness) {
                    return SampleDeltaDielectricTransmission(
                        data, effectiveIor, NdotV, normalShdWldOut,
                        omegaOutWld, tree.luminanceCoefficients);
                }
                auto sample = Bsdf::SampleGGXTransmission(
                    AverageAlphaAsRoughness(data.roughness), effectiveIor,
                    data.tint, normalShdWldOut, omegaOutWld, u1, u2);
                if (sample.pdfSolidAngle <= 0.0f) {
                    return SampleDeltaDielectricTransmission(
                        data, effectiveIor, NdotV, normalShdWldOut,
                        omegaOutWld, tree.luminanceCoefficients);
                }
                sample.bsdfValue *= data.weight;
                return _FinalizeSubtreeSample(tree, nodeId, normalShdWldOut,
                                              omegaOutWld, sample,
                                              heroWavelengthNm);
            }
            if (uChoice < fresnelProb) {
                if (hasDeltaRoughness) {
                    return ScaleDiscreteSpecularSample(
                        SampleDeltaDielectricReflection(data, effectiveIor,
                                                         normalShdLobeWldOut,
                                                         omegaOutWld),
                        fresnelProb);
                }
                if (IsEffectivelyIsotropic(data.roughness)) {
                    auto sample = Bsdf::SampleGGXSpecular(
                        AverageAlphaAsRoughness(data.roughness), effectiveIor,
                        data.tint, normalShdLobeWldOut, omegaOutWld, u1, u2);
                    return _FinalizeSubtreeSample(tree, nodeId, normalShdWldOut,
                                                  omegaOutWld, sample,
                                                  heroWavelengthNm);
                }
                auto sample = SampleGGXSpecularAnisotropic(
                    data.roughness, data.tangent, normalShdLobeWldOut,
                    omegaOutWld, u1, u2);
                return _FinalizeSubtreeSample(tree, nodeId, normalShdWldOut,
                                              omegaOutWld, sample,
                                              heroWavelengthNm);
            }
            {
                if (hasDeltaRoughness) {
                    return ScaleDiscreteSpecularSample(
                        SampleDeltaDielectricTransmission(
                            data, effectiveIor, NdotV, normalShdWldOut,
                            omegaOutWld, tree.luminanceCoefficients),
                        1.0f - fresnelProb);
                }
                auto sample = Bsdf::SampleGGXTransmission(
                    AverageAlphaAsRoughness(data.roughness), effectiveIor,
                    data.tint, normalShdWldOut, omegaOutWld, u1, u2);
                if (sample.pdfSolidAngle <= 0.0f) {
                    return ScaleDiscreteSpecularSample(
                        SampleDeltaDielectricTransmission(
                            data, effectiveIor, NdotV, normalShdWldOut,
                            omegaOutWld, tree.luminanceCoefficients),
                        1.0f - fresnelProb);
                }
                sample.bsdfValue *= data.weight;
                return _FinalizeSubtreeSample(tree, nodeId, normalShdWldOut,
                                              omegaOutWld, sample,
                                              heroWavelengthNm);
            }
        } else if constexpr (
            std::is_same_v<T, Bsdf::DielectricInterfaceData>) {
            const Vec3f normalShdLobeWldOut =
                ResolveReflectionNormal(data, normalShdWldOut, omegaOutWld);
            const float NdotV = std::max(
                std::abs(Dot(normalShdLobeWldOut, omegaOutWld)), kEpsilon);
            const float effectiveIor =
                ResolveDielectricIor(data, heroWavelengthNm);
            const DielectricInterfaceSelection selection =
                DielectricInterfaceSelectionProbabilities(
                    data, NdotV, effectiveIor,
                    Dot(normalShdWldOut, omegaOutWld) < 0.0f,
                    tree.luminanceCoefficients);
            if (selection.reflection + selection.transmission <= 0.0f) {
                return Bsdf::BsdfSample{
                    Vec3f(0.0f), Vec3f(0.0f), 0.0f, false};
            }

            const bool hasDeltaRoughness =
                IsEffectivelyDeltaAlpha(data.roughness);
            if (UsesCoupledRoughDielectricSampling(data)) {
                auto sample = SampleCoupledRoughDielectric(
                    data, effectiveIor, normalShdWldOut, normalShdLobeWldOut,
                    omegaOutWld, u1, u2, uChoice);
                return _FinalizeSubtreeSample(tree, nodeId, normalShdWldOut,
                                              omegaOutWld, sample,
                                              heroWavelengthNm);
            }
            if (hasDeltaRoughness && !data.thinWalled &&
                WouldTotalInternalReflect(effectiveIor, normalShdWldOut,
                                           omegaOutWld)) {
                const float tirWeight = std::max(
                    Clamp01(data.reflectionWeight),
                    Clamp01(data.transmissionWeight));
                return SampleDeltaTotalInternalReflection(
                    tirWeight, normalShdWldOut, omegaOutWld);
            }
            if (uChoice < selection.reflection) {
                if (selection.reflection <= 0.0f) {
                    return Bsdf::BsdfSample{
                        Vec3f(0.0f), Vec3f(0.0f), 0.0f, false};
                }
                if (hasDeltaRoughness) {
                    return ScaleDiscreteSpecularSample(
                        SampleDeltaDielectricInterfaceReflection(
                            data, effectiveIor, normalShdLobeWldOut,
                            omegaOutWld,
                            Dot(normalShdWldOut, omegaOutWld) < 0.0f),
                        selection.reflection);
                }
                if (IsEffectivelyIsotropic(data.roughness)) {
                    auto sample = Bsdf::SampleGGXSpecular(
                        AverageAlphaAsRoughness(data.roughness), effectiveIor,
                        data.reflectionTint, normalShdLobeWldOut, omegaOutWld,
                        u1, u2);
                    return _FinalizeSubtreeSample(tree, nodeId, normalShdWldOut,
                                                  omegaOutWld, sample,
                                                  heroWavelengthNm);
                }
                auto sample = SampleGGXSpecularAnisotropic(
                    data.roughness, data.tangent, normalShdLobeWldOut,
                    omegaOutWld, u1, u2);
                return _FinalizeSubtreeSample(tree, nodeId, normalShdWldOut,
                                              omegaOutWld, sample,
                                              heroWavelengthNm);
            }

            if (selection.transmission <= 0.0f) {
                return Bsdf::BsdfSample{
                    Vec3f(0.0f), Vec3f(0.0f), 0.0f, false};
            }
            if (hasDeltaRoughness) {
                return ScaleDiscreteSpecularSample(
                    SampleDeltaDielectricInterfaceTransmission(
                        data, effectiveIor, NdotV, normalShdWldOut,
                        omegaOutWld),
                    selection.transmission);
            }
            if (data.thinWalled) {
                const Vec3f transmissionN = -normalShdLobeWldOut;
                Vec3f omegaOutMirroredWld =
                    MirrorAcrossSurface(omegaOutWld, normalShdLobeWldOut);
                omegaOutMirroredWld.normalize();
                if (IsEffectivelyIsotropic(data.roughness)) {
                    auto sample = Bsdf::SampleGGXSpecular(
                        AverageAlphaAsRoughness(data.roughness), 1.0f,
                        data.transmissionTint, transmissionN,
                        omegaOutMirroredWld, u1, u2);
                    if (sample.pdfSolidAngle <= 0.0f) {
                        return ScaleDiscreteSpecularSample(
                            SampleDeltaDielectricInterfaceTransmission(
                                data, effectiveIor, NdotV, normalShdWldOut,
                                omegaOutWld),
                            selection.transmission);
                    }
                    return _FinalizeSubtreeSample(tree, nodeId, normalShdWldOut,
                                                  omegaOutWld, sample,
                                                  heroWavelengthNm);
                }
                auto sample = SampleGGXSpecularAnisotropic(
                    data.roughness, data.tangent, transmissionN,
                    omegaOutMirroredWld, u1, u2);
                if (sample.pdfSolidAngle <= 0.0f) {
                    return ScaleDiscreteSpecularSample(
                        SampleDeltaDielectricInterfaceTransmission(
                            data, effectiveIor, NdotV, normalShdWldOut,
                            omegaOutWld),
                        selection.transmission);
                }
                return _FinalizeSubtreeSample(tree, nodeId, normalShdWldOut,
                                              omegaOutWld, sample,
                                              heroWavelengthNm);
            }
            auto sample = Bsdf::SampleGGXTransmission(
                AverageAlphaAsRoughness(data.roughness), effectiveIor,
                data.transmissionTint, normalShdWldOut, omegaOutWld, u1, u2);
            // Rough samples are finalized by re-evaluating this interface,
            // which applies the same BSDL front/back transmission-energy
            // scale used by direct evaluation.
            if (sample.pdfSolidAngle <= 0.0f) {
                return ScaleDiscreteSpecularSample(
                    SampleDeltaDielectricInterfaceTransmission(
                        data, effectiveIor, NdotV, normalShdWldOut,
                        omegaOutWld),
                    selection.transmission);
            }
            sample.bsdfValue *= Clamp01(data.transmissionWeight);
            return _FinalizeSubtreeSample(tree, nodeId, normalShdWldOut,
                                          omegaOutWld, sample,
                                          heroWavelengthNm);
        } else if constexpr (std::is_same_v<T, Bsdf::ConductorData>) {
            const Vec3f normalShdLobeWldOut =
                ResolveReflectionNormal(data, normalShdWldOut, omegaOutWld);
            if (IsEffectivelyDeltaAlpha(data.roughness)) {
                return SampleDeltaConductorReflection(
                    data, normalShdLobeWldOut, omegaOutWld);
            }
            if (IsEffectivelyIsotropic(data.roughness)) {
                auto sample = Bsdf::SampleGGXSpecular(
                    AverageAlphaAsRoughness(data.roughness), 1.5f,
                    ConductorF0(data.ior, data.extinction),
                    normalShdLobeWldOut, omegaOutWld, u1, u2);
                return _FinalizeSubtreeSample(tree, nodeId, normalShdWldOut,
                                              omegaOutWld, sample,
                                              heroWavelengthNm);
            }
            auto sample = SampleGGXSpecularAnisotropic(
                data.roughness, data.tangent, normalShdLobeWldOut, omegaOutWld,
                u1, u2);
            return _FinalizeSubtreeSample(tree, nodeId, normalShdWldOut,
                                          omegaOutWld, sample,
                                          heroWavelengthNm);
        } else if constexpr (std::is_same_v<T, Bsdf::GeneralizedSchlickData>) {
            const Vec3f normalShdLobeWldOut =
                ResolveReflectionNormal(data, normalShdWldOut, omegaOutWld);
            const float NdotV = std::max(
                std::abs(Dot(normalShdLobeWldOut, omegaOutWld)), kEpsilon);
            const bool hasDeltaRoughness =
                IsEffectivelyDeltaAlpha(data.roughness);
            float fresnelProb = Clamp01(luminance(
                GeneralizedSchlickReflectionFresnel(data, NdotV)));
            if (data.scatterMode == Bsdf::ScatterMode::Transmission) {
                float avgF0 = Clamp01(luminance(Clamp01(data.color0)));
                float sqrtF0 = std::sqrt(std::max(avgF0, 0.01f));
                float ior = (1.0f + sqrtF0) / (1.0f - sqrtF0);
                // A transmission-only lobe is paired with a separate
                // reflection lobe that keeps contributing its Schlick
                // reflectance from inside the medium, so a full-weight TIR
                // sample would double-count reflection and gain energy on
                // every internal bounce.  Carry only the remainder so the
                // reflection/transmission pair totals one.
                const auto samplePairedTir = [&]() {
                    auto tir = SampleDeltaTotalInternalReflection(
                        data.weight, normalShdWldOut, omegaOutWld);
                    tir.bsdfValue = CompMul(
                        tir.bsdfValue,
                        Clamp01(
                            Vec3f(1.0f) -
                            GeneralizedSchlickReflectionFresnel(data, NdotV)));
                    return tir;
                };
                if (hasDeltaRoughness &&
                    WouldTotalInternalReflect(ior, normalShdWldOut,
                                               omegaOutWld)) {
                    return samplePairedTir();
                }
                if (hasDeltaRoughness) {
                    auto deltaSample =
                        SampleDeltaTransmission(ior, Vec3f(1.0f), data.weight,
                                                 normalShdWldOut, omegaOutWld);
                    deltaSample.bsdfValue = CompMul(
                        deltaSample.bsdfValue,
                        TransmissionScale(
                            SchlickFresnelScalar(ior, NdotV),
                            GeneralizedSchlickReflectionFresnel(data, NdotV)));
                    return deltaSample;
                }
                auto sample = Bsdf::SampleGGXTransmission(
                    AverageAlphaAsRoughness(data.roughness), ior, Vec3f(1.0f),
                    normalShdWldOut, omegaOutWld, u1, u2);
                if (sample.pdfSolidAngle <= 0.0f) {
                    if (WouldTotalInternalReflect(ior, normalShdWldOut,
                                                   omegaOutWld)) {
                        return samplePairedTir();
                    }
                    auto deltaSample =
                        SampleDeltaTransmission(ior, Vec3f(1.0f), data.weight,
                                                 normalShdWldOut, omegaOutWld);
                    deltaSample.bsdfValue = CompMul(
                        deltaSample.bsdfValue,
                        TransmissionScale(
                            SchlickFresnelScalar(ior, NdotV),
                            GeneralizedSchlickReflectionFresnel(data, NdotV)));
                    return deltaSample;
                }
                sample.bsdfValue *= data.weight;
                return _FinalizeSubtreeSample(tree, nodeId, normalShdWldOut,
                                              omegaOutWld, sample,
                                              heroWavelengthNm);
            }
            if (data.scatterMode == Bsdf::ScatterMode::ReflectionTransmission &&
                uChoice >= fresnelProb) {
                float avgF0 = Clamp01(luminance(Clamp01(data.color0)));
                float sqrtF0 = std::sqrt(std::max(avgF0, 0.01f));
                float ior = (1.0f + sqrtF0) / (1.0f - sqrtF0);
                if (hasDeltaRoughness &&
                    WouldTotalInternalReflect(ior, normalShdWldOut,
                                               omegaOutWld)) {
                    return SampleDeltaTotalInternalReflection(
                        data.weight, normalShdWldOut, omegaOutWld);
                }
                if (hasDeltaRoughness) {
                    auto deltaSample =
                        SampleDeltaTransmission(ior, Vec3f(1.0f), data.weight,
                                                 normalShdWldOut, omegaOutWld);
                    deltaSample.bsdfValue = CompMul(
                        deltaSample.bsdfValue,
                        TransmissionScale(
                            SchlickFresnelScalar(ior, NdotV),
                            GeneralizedSchlickReflectionFresnel(data, NdotV)));
                    return ScaleDiscreteSpecularSample(
                        deltaSample,
                        1.0f - fresnelProb);
                }
                auto sample = Bsdf::SampleGGXTransmission(
                    AverageAlphaAsRoughness(data.roughness), ior, Vec3f(1.0f),
                    normalShdWldOut, omegaOutWld, u1, u2);
                if (sample.pdfSolidAngle <= 0.0f) {
                    auto deltaSample =
                        SampleDeltaTransmission(ior, Vec3f(1.0f), data.weight,
                                                 normalShdWldOut, omegaOutWld);
                    deltaSample.bsdfValue = CompMul(
                        deltaSample.bsdfValue,
                        TransmissionScale(
                            SchlickFresnelScalar(ior, NdotV),
                            GeneralizedSchlickReflectionFresnel(data, NdotV)));
                    return deltaSample;
                }
                sample.bsdfValue *= data.weight;
                return _FinalizeSubtreeSample(tree, nodeId, normalShdWldOut,
                                              omegaOutWld, sample,
                                              heroWavelengthNm);
            }
            if (hasDeltaRoughness) {
                auto sample = SampleDeltaGeneralizedSchlickReflection(
                    data, normalShdLobeWldOut, omegaOutWld);
                if (data.scatterMode ==
                    Bsdf::ScatterMode::ReflectionTransmission) {
                    return ScaleDiscreteSpecularSample(sample, fresnelProb);
                }
                return sample;
            }
            if (IsEffectivelyIsotropic(data.roughness)) {
                auto sample = Bsdf::SampleGGXSpecular(
                    AverageAlphaAsRoughness(data.roughness), 1.5f, data.color0,
                    normalShdLobeWldOut, omegaOutWld, u1, u2);
                return _FinalizeSubtreeSample(tree, nodeId, normalShdWldOut,
                                              omegaOutWld, sample,
                                              heroWavelengthNm);
            }
            auto sample = SampleGGXSpecularAnisotropic(
                data.roughness, data.tangent, normalShdLobeWldOut, omegaOutWld,
                u1, u2);
            return _FinalizeSubtreeSample(tree, nodeId, normalShdWldOut,
                                          omegaOutWld, sample,
                                          heroWavelengthNm);
        } else if constexpr (std::is_same_v<T, Bsdf::SheenData>) {
            const Vec3f normalShdLobeWldOut =
                ResolveReflectionNormal(data, normalShdWldOut, omegaOutWld);
            auto sample = Bsdf::SampleLambertian(data.color * data.weight,
                                                 normalShdLobeWldOut,
                                                 omegaOutWld, u1, u2);
            return _FinalizeSubtreeSample(tree, nodeId, normalShdWldOut,
                                          omegaOutWld, sample,
                                          heroWavelengthNm);
        } else if constexpr (std::is_same_v<T, Bsdf::AdobeOpenPbrData>) {
            return SampleAdobeOpenPbr(data, normalShdWldOut, omegaOutWld, u1,
                                      u2, uChoice);
        } else if constexpr (std::is_same_v<T, Bsdf::UnsupportedData>) {
            return Bsdf::BsdfSample{Vec3f(0.0f), Vec3f(0.0f), 0.0f, false};
        } else if constexpr (std::is_same_v<T, Bsdf::MixData>) {
            float mix = Clamp01(data.mix);
            bool chooseFg = (uChoice < mix);
            Bsdf::NodeId chosen = chooseFg ? data.fg : data.bg;
            float chooseProb = chooseFg ? mix : (1.0f - mix);
            float remapped = (uChoice < mix)
                ? (mix > 0.0f ? uChoice / mix : 0.0f)
                : ((1.0f - mix) > 0.0f ? (uChoice - mix) / (1.0f - mix) : 0.0f);
            auto sample =
                SampleNode(tree, chosen, normalShdWldOut, omegaOutWld, u1, u2,
                            remapped, heroWavelengthNm);
            if ((sample.isSpecular || sample.isSubsurface) &&
                sample.pdfSolidAngle > 0.0f && chooseProb > 0.0f) {
                sample.bsdfValue /= chooseProb;
            }
            return _FinalizeSubtreeSample(tree, nodeId, normalShdWldOut,
                                          omegaOutWld, sample,
                                          heroWavelengthNm);
        } else if constexpr (std::is_same_v<T, Bsdf::LayerData>) {
            float topWeight = _ApproxWeight(tree, data.top, normalShdWldOut,
                                            omegaOutWld, heroWavelengthNm);
            float baseWeight =
                _ApproxWeight(tree, data.base, normalShdWldOut, omegaOutWld,
                              heroWavelengthNm) *
                luminance(_EvalThroughput(tree, data.top, normalShdWldOut,
                                           omegaOutWld, heroWavelengthNm));
            float total = topWeight + baseWeight;
            if (total <= 0.0f) {
                return Bsdf::BsdfSample{Vec3f(0.0f), Vec3f(0.0f), 0.0f, false};
            }
            float pTop = topWeight / total;
            bool chooseTop = (uChoice < pTop);
            Bsdf::NodeId chosen = chooseTop ? data.top : data.base;
            float chooseProb = chooseTop ? pTop : (1.0f - pTop);
            float remapped = (uChoice < pTop)
                ? (pTop > 0.0f ? uChoice / pTop : 0.0f)
                : ((1.0f - pTop) > 0.0f ? (uChoice - pTop) / (1.0f - pTop) : 0.0f);
            auto sample =
                SampleNode(tree, chosen, normalShdWldOut, omegaOutWld, u1, u2,
                            remapped, heroWavelengthNm);
            if ((sample.isSpecular || sample.isSubsurface) &&
                sample.pdfSolidAngle > 0.0f) {
                if (!chooseTop) {
                    sample.bsdfValue = CompMul(
                        sample.bsdfValue, _EvalLayerBaseThroughput(
                                              tree, data.top, normalShdWldOut,
                                              omegaOutWld, heroWavelengthNm));
                }
                if (chooseProb > 0.0f) {
                    sample.bsdfValue /= chooseProb;
                }
            }
            return _FinalizeSubtreeSample(tree, nodeId, normalShdWldOut,
                                          omegaOutWld, sample,
                                          heroWavelengthNm);
        } else if constexpr (std::is_same_v<T, Bsdf::AddData>) {
            float w1 = _ApproxWeight(tree, data.in1, normalShdWldOut,
                                     omegaOutWld, heroWavelengthNm);
            float w2 = _ApproxWeight(tree, data.in2, normalShdWldOut,
                                     omegaOutWld, heroWavelengthNm);
            float total = w1 + w2;
            if (total <= 0.0f) {
                return Bsdf::BsdfSample{Vec3f(0.0f), Vec3f(0.0f), 0.0f, false};
            }
            float p1 = w1 / total;
            bool chooseFirst = (uChoice < p1);
            Bsdf::NodeId chosen = chooseFirst ? data.in1 : data.in2;
            float chooseProb = chooseFirst ? p1 : (1.0f - p1);
            float remapped = (uChoice < p1)
                ? (p1 > 0.0f ? uChoice / p1 : 0.0f)
                : ((1.0f - p1) > 0.0f ? (uChoice - p1) / (1.0f - p1) : 0.0f);
            auto sample =
                SampleNode(tree, chosen, normalShdWldOut, omegaOutWld, u1, u2,
                            remapped, heroWavelengthNm);
            if ((sample.isSpecular || sample.isSubsurface) &&
                sample.pdfSolidAngle > 0.0f && chooseProb > 0.0f) {
                sample.bsdfValue /= chooseProb;
            }
            return _FinalizeSubtreeSample(tree, nodeId, normalShdWldOut,
                                          omegaOutWld, sample,
                                          heroWavelengthNm);
        } else if constexpr (std::is_same_v<T, Bsdf::MultiplyData>) {
            auto sample =
                SampleNode(tree, data.input, normalShdWldOut, omegaOutWld, u1,
                            u2, uChoice, heroWavelengthNm);
            if ((sample.isSpecular || sample.isSubsurface) &&
                sample.pdfSolidAngle > 0.0f) {
                sample.bsdfValue = CompMul(data.weight, sample.bsdfValue);
            }
            return _FinalizeSubtreeSample(tree, nodeId, normalShdWldOut,
                                          omegaOutWld, sample,
                                          heroWavelengthNm);
        } else {
            return Bsdf::BsdfSample{Vec3f(0.0f), Vec3f(0.0f), 0.0f, false};
        }
    }, node->data);
}

Bsdf::ClosureTree
PruneCausticClassLobes(const Bsdf::ClosureTree& tree)
{
    CausticClassPruner pruner(tree);
    return pruner.Run();
}

}  // namespace detail
}  // namespace Bsdf
}  // namespace mxcpp
