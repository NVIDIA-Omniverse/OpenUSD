//
// Copyright 2018 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
// Surface and participating-medium direct lighting.

#include "pxr/imaging/plugin/hdEmbree/renderer/renderer.h"
#include "../rendererImpl.h"
#include "pxr/imaging/plugin/hdEmbree/renderer/renderBuffer.h"

#include "pxr/imaging/hd/perfLog.h"
#include "pxr/base/work/loops.h"
#include "pxr/base/work/threadLimits.h"

#include <chrono>
#include <cstdio>
#include <thread>

PXR_NAMESPACE_OPEN_SCOPE

void
HdEmbreeRenderer::_AccumulateEnvironment(_PathState* state) const
{
    if (!state) {
        return;
    }

    if (state->isFirstBounce) {
        if (_lights.GetDomes().empty() ||
            !_settings.domeLightCameraVisibility) {
            state->radianceAccumulated = GfVec3f(
                _colorClearValue[0], _colorClearValue[1], _colorClearValue[2]);
            return;
        }

        for (auto* dome : _lights.GetDomes()) {
            if (dome->visible) {
                state->radianceAccumulated +=
                    HdEmbreeLightSampler::EvaluateDomeLightDirection(
                        *dome, state->directionRayWld, _renderColorSpace)
                        .radianceIn;
            }
        }
        return;
    }

    for (auto const& it : _lights.GetLights()) {
        if (!it.second) {
            continue;
        }

        auto const& light = *it.second;
        if (!light.visible ||
            !std::holds_alternative<HdEmbree_Distant>(light.lightVariant) ||
            (!light.lightLink.IsEmpty() &&
             (!state->lastScatterCategories ||
              !HdEmbreeMatchesLink(
                  light.lightLink, *state->lastScatterCategories)))) {
            continue;
        }

        const HdEmbreeLightSampler::LightSample sample =
            HdEmbreeLightSampler::EvaluateLightDirection(
                light, state->positionRayOriginWld, state->directionRayWld,
                _renderColorSpace);
        if (!sample.valid) {
            continue;
        }

        GfVec3f radianceEnvironment = sample.radianceIn;
        if (state->lastBsdfPdf > 0.0f && sample.pdfSolidAngleInverse > 0.0f) {
            radianceEnvironment *= mxcpp::Bsdf::PowerHeuristic(
                state->lastBsdfPdf,
                _GetMultiSampleMisLightPdf(1.0f / sample.pdfSolidAngleInverse,
                                           _settings.lightSamplesPerHit));
        }
        _AddPathRadiance(_WeightPathRadiance(radianceEnvironment, *state),
                         state);
    }

    for (auto* dome : _lights.GetDomes()) {
        if (!dome->visible ||
            (!dome->lightLink.IsEmpty() &&
             (!state->lastScatterCategories ||
              !HdEmbreeMatchesLink(
                  dome->lightLink, *state->lastScatterCategories)))) {
            continue;
        }

        const HdEmbreeLightSampler::SamplingMode samplingMode =
            state->lastScatterWasMedium
                ? HdEmbreeLightSampler::SamplingMode::FullSphere
                : state->lastLightSamplingMode;
        HdEmbreeLightSampler::LightSample sample =
            samplingMode ==
                    HdEmbreeLightSampler::SamplingMode::ReflectionHemisphere
                ? HdEmbreeLightSampler::EvaluateDomeLightDirection(
                      *dome, state->directionRayWld,
                      state->lastLightSamplingNormal, samplingMode,
                      _renderColorSpace)
                : HdEmbreeLightSampler::EvaluateDomeLightDirection(
                      *dome, state->directionRayWld, _renderColorSpace);

        GfVec3f radianceEnvironment = sample.radianceIn;
        if (state->lastBsdfPdf > 0.0f) {
            const float pdfDomeSolidAngle = _GetMultiSampleMisLightPdf(
                sample.pdfSolidAngleInverse > 0.0f
                    ? 1.0f / sample.pdfSolidAngleInverse
                    : 0.0f,
                _settings.lightSamplesPerHit);
            radianceEnvironment *= mxcpp::Bsdf::PowerHeuristic(
                state->lastBsdfPdf, pdfDomeSolidAngle);
        }
        _AddPathRadiance(_WeightPathRadiance(radianceEnvironment, *state),
                         state);
    }
}

GfVec3f
HdEmbreeRenderer::_ComputeDirectLightingMIS(
    GfVec3f const& positionWld, GfVec3f const& normalShdWldOut,
    GfVec3f const& normalGeomWldExt, GfVec3f const& omegaOutWld,
    HdEmbreeSampleDomain const& domain, bool frontFacing,
    bool includeBsdfSamplingMis, mxcpp::SurfaceClosure const* closure,
    HdEmbreeCategorySet const& receiverCategories,
    HdEmbreeMediumState const& mediumState, bool spectralActive,
    float heroWavelengthNm, float heroWavelengthPdf,
    mxcpp::AdobeOpenPbrPreparedSurface const* adobeOpenPbrSurface) const
{
    const _HeroWavelengthState hero{
        spectralActive, heroWavelengthNm, heroWavelengthPdf};
    GfVec3f radianceDirect(0.0f);
    const int lightSampleCount = _settings.lightSamplesPerHit;
    const float lightSampleCountInverse =
        1.0f / static_cast<float>(lightSampleCount);
    const HdEmbreeLightSampler::SamplingMode lightSamplingMode =
        (closure && _IsReflectionOnlyClosure(*closure))
            ? HdEmbreeLightSampler::SamplingMode::ReflectionHemisphere
            : HdEmbreeLightSampler::SamplingMode::FullSphere;

    // For stratification: compute grid dimensions for lightSampleCount samples.
    // Find the largest sqrtN such that sqrtN*sqrtN <= lightSampleCount, then
    // use sqrtN x ceilN grid where ceilN = ceil(lightSampleCount / sqrtN).
    int stratDimU = 1, stratDimV = 1;
    if (_settings.stratifyLightSamples && lightSampleCount > 1) {
        stratDimU =
            static_cast<int>(std::sqrt(static_cast<float>(lightSampleCount)));
        if (stratDimU < 1) stratDimU = 1;
        stratDimV = (lightSampleCount + stratDimU - 1) / stratDimU;
    }

    int lightIndex = 0;
    for (auto const& it : _lights.GetLights())
    {
        if (!it.second) {
            ++lightIndex;
            continue;
        }
        auto const& light = *it.second;
        if (!light.visible ||
            !HdEmbreeMatchesLink(light.lightLink, receiverCategories)) {
            ++lightIndex;
            continue;
        }
        const HdEmbreeSampleDomain lightDomain =
            domain.Chain(HdEmbreeSampleDomainKey::DirectLightSelect,
                         lightIndex);

        GfVec3f radianceLight(0.0f);

        for (int indexSampleLight = 0; indexSampleLight < lightSampleCount;
             ++indexSampleLight) {
            const GfVec2f sample =
                lightDomain
                    .Split(HdEmbreeSampleDomainKey::DirectLightSample,
                           lightSampleCount, indexSampleLight)
                    .Draw2D();
            // Generate sample coordinates, optionally stratified.
            float u1, u2;
            if (_settings.stratifyLightSamples && lightSampleCount > 1) {
                int indexStratumU = indexSampleLight % stratDimU;
                int indexStratumV = indexSampleLight / stratDimU;
                u1 =
                    (indexStratumU + sample[0]) / static_cast<float>(stratDimU);
                u2 =
                    (indexStratumV + sample[1]) / static_cast<float>(stratDimV);
            } else {
                u1 = sample[0];
                u2 = sample[1];
            }

            HdEmbreeLightSampler::LightSample ls =
                HdEmbreeLightSampler::GetLightSample(light, positionWld,
                                                     normalShdWldOut, u1, u2,
                                                     lightSamplingMode,
                                                     _renderColorSpace);
            if (GfIsClose(ls.radianceIn, GfVec3f(0.0f), _minLuminanceCutoff)) {
                continue;
            }

            // Keep the BSDF normal fixed relative to omegaOutWld. Reflection
            // lobes reject backside omegaInWld internally; transmission lobes
            // need those samples to survive direct-light evaluation.
            const float cosThetaLightAbsolute =
                std::abs(GfDot(ls.omegaInWld, normalShdWldOut));
            if (cosThetaLightAbsolute <= 0.0f) {
                continue;
            }
            const bool shadingReflection =
                GfDot(omegaOutWld, normalShdWldOut) *
                    GfDot(ls.omegaInWld, normalShdWldOut) >
                0.0f;
            const bool geometricReflection =
                GfDot(omegaOutWld, normalGeomWldExt) *
                    GfDot(ls.omegaInWld, normalGeomWldExt) >
                0.0f;
            if (shadingReflection != geometricReflection) {
                continue;
            }

            GfVec3f visibility = _Visibility(
                positionWld, normalGeomWldExt, ls.omegaInWld,
                ls.distanceWld * 0.99f, light.shadowLink, mediumState);
            if (_IsNearlyBlack(visibility)) {
                continue;
            }

            GfVec3f radianceSample(0.0f);
            if (closure) {
                const mxcpp::Vec3f normalMx = _ToMx(normalShdWldOut);
                const mxcpp::Vec3f interfaceNormalMx =
                    frontFacing ? normalMx : -normalMx;
                const mxcpp::Vec3f omegaInWldMx = _ToMx(ls.omegaInWld);
                const mxcpp::Vec3f omegaOutWldMx = _ToMx(omegaOutWld);
                const mxcpp::AdobeOpenPbrEvalPdfResult adobeEvalPdf =
                    (adobeOpenPbrSurface && adobeOpenPbrSurface->valid)
                        ? mxcpp::EvalPdfPreparedAdobeOpenPbrSurface(
                              *adobeOpenPbrSurface, omegaInWldMx)
                        : mxcpp::TryEvalPdfAdobeOpenPbrSurface(
                              *closure, interfaceNormalMx, omegaInWldMx,
                              omegaOutWldMx);

                GfVec3f bsdfValue(0.0f);
                float pdfBsdfSolidAngle = 0.0f;
                if (adobeEvalPdf.evaluated) {
                    bsdfValue = _ToGf(adobeEvalPdf.value);
                    pdfBsdfSolidAngle = adobeEvalPdf.pdfSolidAngle;
                } else {
                    bsdfValue = _ToGf(mxcpp::Bsdf::EvalSurface(
                        *closure, normalMx, omegaInWldMx, omegaOutWldMx,
                        heroWavelengthNm, frontFacing));
                    pdfBsdfSolidAngle = mxcpp::Bsdf::PdfSurface(
                        *closure, normalMx, omegaInWldMx, omegaOutWldMx,
                        heroWavelengthNm, frontFacing);
                }

                for (int i = 0; i < 3; ++i) {
                    if (!std::isfinite(bsdfValue[i])) bsdfValue[i] = 0.0f;
                }

                // MIS weight for the multi-sample estimator. The contribution
                // itself is averaged by 1/lightSampleCount outside this loop,
                // but Veach'indexSampleLight multi-sample MIS weights use n_i *
                // p_i for each strategy.
                float weightMis = 1.0f;
                if (includeBsdfSamplingMis && !ls.delta) {
                    float pdfLightSolidAngle =
                        (ls.pdfSolidAngleInverse > 0.0f)
                            ? 1.0f / ls.pdfSolidAngleInverse
                            : 0.0f;
                    const float pdfLightSolidAngleEffective =
                        _GetMultiSampleMisLightPdf(pdfLightSolidAngle,
                                                   lightSampleCount);
                    weightMis = mxcpp::Bsdf::PowerHeuristic(
                        pdfLightSolidAngleEffective, pdfBsdfSolidAngle);
                }

                if (hero.active) {
                    const float radianceInSpectral =
                        _RgbToSpectralValue(
                            ls.radianceIn, hero, _renderColorSpace);
                    const float spectralVis =
                        _RgbToSpectralValue(
                            visibility, hero, _renderColorSpace);
                    const float spectralBsdf =
                        _RgbToSpectralValue(
                            bsdfValue, hero, _renderColorSpace);
                    radianceSample = _SpectralValueToRgb(
                        radianceInSpectral * spectralBsdf *
                            cosThetaLightAbsolute * spectralVis *
                            ls.pdfSolidAngleInverse * weightMis,
                        hero, _renderColorSpace);
                } else {
                    radianceSample =
                        GfCompMult(GfCompMult(ls.radianceIn, bsdfValue),
                                   visibility) *
                        cosThetaLightAbsolute * ls.pdfSolidAngleInverse *
                        weightMis;
                }
            } else {
                float brdf = 1.0f / _pi<float>;
                if (hero.active) {
                    const float radianceInSpectral =
                        _RgbToSpectralValue(
                            ls.radianceIn, hero, _renderColorSpace);
                    const float spectralVis =
                        _RgbToSpectralValue(
                            visibility, hero, _renderColorSpace);
                    radianceSample = _SpectralValueToRgb(
                        radianceInSpectral * cosThetaLightAbsolute * brdf *
                            spectralVis * ls.pdfSolidAngleInverse,
                        hero, _renderColorSpace);
                } else {
                    radianceSample = GfCompMult(ls.radianceIn, visibility) *
                                     cosThetaLightAbsolute * brdf *
                                     ls.pdfSolidAngleInverse;
                }
            }

            radianceSample = _ClampFireflyContribution(
                radianceSample,
                _settings.fireflyClampThreshold,
                _materialEvalServices.luminanceCoefficients);

            radianceLight += radianceSample;
        }

        radianceDirect += radianceLight * lightSampleCountInverse;
        ++lightIndex;
    }
    return radianceDirect;
}

GfVec3f
HdEmbreeRenderer::_ComputeMediumDirectLighting(
    GfVec3f const& positionWld, GfVec3f const& omegaOutWld,
    HdEmbreeMediumState const& mediumState, HdEmbreeSampleDomain const& domain,
    bool includePhaseSamplingMis, bool spectralActive, float heroWavelengthNm,
    float heroWavelengthPdf) const
{
    const _HeroWavelengthState hero{
        spectralActive, heroWavelengthNm, heroWavelengthPdf};
    if (!mediumState.active || mediumState.medium.IsAbsorbingOnly()) {
        return GfVec3f(0.0f);
    }
    const bool useAdobeVolumeTransport =
        _settings.useAdobeOpenPBR &&
        mediumState.medium.transportModel ==
            mxcpp::MediumTransportModel::AdobeOpenPBR;

    GfVec3f radianceDirect(0.0f);
    const HdEmbreeCategorySet emptyCategories;
    HdEmbreeCategorySet const& receiverCategories = mediumState.categories
        ? *mediumState.categories
        : emptyCategories;
    const int lightSampleCount = _settings.lightSamplesPerHit;
    const float lightSampleCountInverse =
        1.0f / static_cast<float>(lightSampleCount);

    int stratDimU = 1, stratDimV = 1;
    if (_settings.stratifyLightSamples && lightSampleCount > 1) {
        stratDimU =
            static_cast<int>(std::sqrt(static_cast<float>(lightSampleCount)));
        if (stratDimU < 1) {
            stratDimU = 1;
        }
        stratDimV = (lightSampleCount + stratDimU - 1) / stratDimU;
    }

    int lightIndex = 0;
    for (auto const& it : _lights.GetLights()) {
        if (!it.second) {
            ++lightIndex;
            continue;
        }
        auto const& light = *it.second;
        if (!light.visible ||
            !HdEmbreeMatchesLink(light.lightLink, receiverCategories)) {
            ++lightIndex;
            continue;
        }
        const HdEmbreeSampleDomain lightDomain =
            domain.Chain(HdEmbreeSampleDomainKey::MediumDirectLightSelect,
                         lightIndex);

        GfVec3f radianceLight(0.0f);
        for (int indexSampleLight = 0; indexSampleLight < lightSampleCount;
             ++indexSampleLight) {
            const GfVec2f sample =
                lightDomain
                    .Split(HdEmbreeSampleDomainKey::MediumDirectLightSample,
                           lightSampleCount, indexSampleLight)
                    .Draw2D();
            float u1 = 0.0f;
            float u2 = 0.0f;
            if (_settings.stratifyLightSamples && lightSampleCount > 1) {
                int indexStratumU = indexSampleLight % stratDimU;
                int indexStratumV = indexSampleLight / stratDimU;
                u1 =
                    (indexStratumU + sample[0]) / static_cast<float>(stratDimU);
                u2 =
                    (indexStratumV + sample[1]) / static_cast<float>(stratDimV);
            } else {
                u1 = sample[0];
                u2 = sample[1];
            }

            const HdEmbreeLightSampler::LightSample ls =
                HdEmbreeLightSampler::GetLightSample(light, positionWld,
                                                     GfVec3f(0.0f), u1, u2,
                                                     HdEmbreeLightSampler::
                                                         SamplingMode::
                                                             FullSphere,
                                                     _renderColorSpace);
            if (GfIsClose(ls.radianceIn, GfVec3f(0.0f), _minLuminanceCutoff)) {
                continue;
            }

            // A medium event has no surface normal. Use omegaInWld as the
            // ray-offset reference so the origin advances toward the light.
            const GfVec3f visibility = _Visibility(
                positionWld, ls.omegaInWld, ls.omegaInWld,
                ls.distanceWld * 0.99f, light.shadowLink, mediumState);
            if (_IsNearlyBlack(visibility)) {
                continue;
            }

            const float pdfPhaseSolidAngle =
                useAdobeVolumeTransport
                    ? mxcpp::AdobeOpenPbrEvalVolumePhasePdf(
                          mediumState.medium, _ToMx(ls.omegaInWld),
                          _ToMx(omegaOutWld))
                    : mxcpp::PdfHenyeyGreenstein(_ToMx(ls.omegaInWld),
                                                 _ToMx(omegaOutWld),
                                                 mediumState.medium.anisotropy);
            if (pdfPhaseSolidAngle <= 0.0f) {
                continue;
            }

            float weightMis = 1.0f;
            if (includePhaseSamplingMis && !ls.delta &&
                ls.pdfSolidAngleInverse > 0.0f) {
                const float pdfLightSolidAngle = 1.0f / ls.pdfSolidAngleInverse;
                const float pdfLightSolidAngleEffective =
                    _GetMultiSampleMisLightPdf(pdfLightSolidAngle,
                                               lightSampleCount);
                if (pdfLightSolidAngleEffective > 0.0f) {
                    weightMis = mxcpp::Bsdf::PowerHeuristic(
                        pdfLightSolidAngleEffective, pdfPhaseSolidAngle);
                }
            }

            GfVec3f radianceSample(0.0f);
            if (hero.active) {
                const float radianceInSpectral =
                    _RgbToSpectralValue(
                        ls.radianceIn, hero, _renderColorSpace);
                const float spectralVis =
                    _RgbToSpectralValue(
                        visibility, hero, _renderColorSpace);
                radianceSample = _SpectralValueToRgb(
                    radianceInSpectral * spectralVis * pdfPhaseSolidAngle *
                        ls.pdfSolidAngleInverse * weightMis,
                    hero, _renderColorSpace);
            } else {
                radianceSample = GfCompMult(ls.radianceIn, visibility) *
                                 pdfPhaseSolidAngle * ls.pdfSolidAngleInverse *
                                 weightMis;
            }

            radianceSample = _ClampFireflyContribution(
                radianceSample,
                _settings.fireflyClampThreshold,
                _materialEvalServices.luminanceCoefficients);

            radianceLight += radianceSample;
        }

        radianceDirect += radianceLight * lightSampleCountInverse;
        ++lightIndex;
    }

    return radianceDirect;
}

PXR_NAMESPACE_CLOSE_SCOPE
