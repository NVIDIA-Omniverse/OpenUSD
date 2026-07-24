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
        if (_lights.GetDomes().empty() || !_domeLightCameraVisibility) {
            state->radiance = GfVec3f(
                _colorClearValue[0],
                _colorClearValue[1],
                _colorClearValue[2]);
            return;
        }

        for (auto* dome : _lights.GetDomes()) {
            if (dome->visible) {
                state->radiance +=
                    HdEmbreeLightSampler::EvaluateDomeLightDirection(
                        *dome, state->rayDir).Li;
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
                light, state->rayOrigin, state->rayDir);
        if (!sample.valid) {
            continue;
        }

        GfVec3f contribution = sample.Li;
        if (state->lastBsdfPdf > 0.0f && sample.invPdfW > 0.0f) {
            contribution *= mxcpp::Bsdf::PowerHeuristic(
                state->lastBsdfPdf,
                _GetMultiSampleMisLightPdf(
                    1.0f / sample.invPdfW, _lightSamplesPerHit));
        }
        _AddPathRadiance(
            _WeightPathRadiance(contribution, *state), state);
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
                      *dome,
                      state->rayDir,
                      state->lastLightSamplingNormal,
                      samplingMode)
                : HdEmbreeLightSampler::EvaluateDomeLightDirection(
                      *dome, state->rayDir);

        GfVec3f contribution = sample.Li;
        if (state->lastBsdfPdf > 0.0f) {
            const float domePdf = _GetMultiSampleMisLightPdf(
                sample.invPdfW > 0.0f ? 1.0f / sample.invPdfW : 0.0f,
                _lightSamplesPerHit);
            contribution *= mxcpp::Bsdf::PowerHeuristic(
                state->lastBsdfPdf, domePdf);
        }
        _AddPathRadiance(
            _WeightPathRadiance(contribution, *state), state);
    }
}

GfVec3f
HdEmbreeRenderer::_ComputeDirectLightingMIS(
    GfVec3f const& position,
    GfVec3f const& normal,
    GfVec3f const& Ng,
    GfVec3f const& wo,
    HdEmbreeSampleDomain const& domain,
    bool frontFacing,
    bool includeBsdfSamplingMis,
    mxcpp::SurfaceClosure const* closure,
    HdEmbreeCategorySet const& receiverCategories,
    HdEmbreeMediumState const& mediumState,
    bool spectralActive,
    float heroWavelengthNm,
    float heroWavelengthPdf,
    mxcpp::AdobeOpenPbrPreparedSurface const* adobeOpenPbrSurface) const
{
    const _HeroWavelengthState hero{
        spectralActive, heroWavelengthNm, heroWavelengthPdf};
    GfVec3f finalColor(0.0f);
    const int N = _lightSamplesPerHit;
    const float invN = 1.0f / static_cast<float>(N);
    const HdEmbreeLightSampler::SamplingMode lightSamplingMode =
        (closure && _IsReflectionOnlyClosure(*closure))
            ? HdEmbreeLightSampler::SamplingMode::ReflectionHemisphere
            : HdEmbreeLightSampler::SamplingMode::FullSphere;

    // For stratification: compute grid dimensions for N samples.
    // Find the largest sqrtN such that sqrtN*sqrtN <= N, then
    // use sqrtN x ceilN grid where ceilN = ceil(N / sqrtN).
    int stratDimU = 1, stratDimV = 1;
    if (_stratifyLightSamples && N > 1) {
        stratDimU = static_cast<int>(std::sqrt(static_cast<float>(N)));
        if (stratDimU < 1) stratDimU = 1;
        stratDimV = (N + stratDimU - 1) / stratDimU;
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

        GfVec3f lightContrib(0.0f);

        for (int s = 0; s < N; ++s) {
            const GfVec2f sample =
                lightDomain
                    .Split(HdEmbreeSampleDomainKey::DirectLightSample, N, s)
                    .Draw2D();
            // Generate sample coordinates, optionally stratified.
            float u1, u2;
            if (_stratifyLightSamples && N > 1) {
                int su = s % stratDimU;
                int sv = s / stratDimU;
                u1 = (su + sample[0]) / static_cast<float>(stratDimU);
                u2 = (sv + sample[1]) / static_cast<float>(stratDimV);
            } else {
                u1 = sample[0];
                u2 = sample[1];
            }

            HdEmbreeLightSampler::LightSample ls =
                HdEmbreeLightSampler::GetLightSample(
                    light, position, normal, u1, u2, lightSamplingMode);
            if (GfIsClose(ls.Li, GfVec3f(0.0f), _minLuminanceCutoff)) {
                continue;
            }

            // Keep the BSDF normal fixed relative to wo. Reflection lobes
            // reject backside wi internally, while transmission lobes need
            // those samples to survive direct-light evaluation.
            const float absDotNL = std::abs(GfDot(ls.wI, normal));
            if (absDotNL <= 0.0f) {
                continue;
            }
            const bool shadingReflection =
                GfDot(wo, normal) * GfDot(ls.wI, normal) > 0.0f;
            const bool geometricReflection =
                GfDot(wo, Ng) * GfDot(ls.wI, Ng) > 0.0f;
            if (shadingReflection != geometricReflection) {
                continue;
            }

            GfVec3f vis = _Visibility(
                position,
                Ng,
                ls.wI,
                ls.dist * 0.99f,
                light.shadowLink,
                mediumState);
            if (_IsNearlyBlack(vis)) {
                continue;
            }

            GfVec3f sampleContrib(0.0f);
            if (closure) {
                const mxcpp::Vec3f normalMx = _ToMx(normal);
                const mxcpp::Vec3f interfaceNormalMx =
                    frontFacing ? normalMx : -normalMx;
                const mxcpp::Vec3f wiMx = _ToMx(ls.wI);
                const mxcpp::Vec3f woMx = _ToMx(wo);
                const mxcpp::AdobeOpenPbrEvalPdfResult adobeEvalPdf =
                    (adobeOpenPbrSurface && adobeOpenPbrSurface->valid)
                    ? mxcpp::EvalPdfPreparedAdobeOpenPbrSurface(
                          *adobeOpenPbrSurface,
                          wiMx)
                    : mxcpp::TryEvalPdfAdobeOpenPbrSurface(
                          *closure,
                          interfaceNormalMx,
                          wiMx,
                          woMx);

                GfVec3f bsdfValue(0.0f);
                float bsdfPdf = 0.0f;
                if (adobeEvalPdf.evaluated) {
                    bsdfValue = _ToGf(adobeEvalPdf.value);
                    bsdfPdf = adobeEvalPdf.pdf;
                } else {
                    bsdfValue = _ToGf(mxcpp::Bsdf::EvalSurface(
                        *closure,
                        normalMx,
                        wiMx,
                        woMx,
                        heroWavelengthNm,
                        frontFacing));
                    bsdfPdf = mxcpp::Bsdf::PdfSurface(
                        *closure,
                        normalMx,
                        wiMx,
                        woMx,
                        heroWavelengthNm,
                        frontFacing);
                }

                for (int i = 0; i < 3; ++i) {
                    if (!std::isfinite(bsdfValue[i])) bsdfValue[i] = 0.0f;
                }

                // MIS weight for the multi-sample estimator. The contribution
                // itself is averaged by 1/N outside this loop, but Veach's
                // multi-sample MIS weights use n_i * p_i for each strategy.
                float misW = 1.0f;
                if (includeBsdfSamplingMis && !ls.delta) {
                    float lightPdf = (ls.invPdfW > 0.0f)
                        ? 1.0f / ls.invPdfW : 0.0f;
                    const float effectiveLightPdf =
                        _GetMultiSampleMisLightPdf(lightPdf, N);
                    misW = mxcpp::Bsdf::PowerHeuristic(
                        effectiveLightPdf, bsdfPdf);
                }

                if (hero.active) {
                    const float spectralLi = _RgbToSpectralValue(ls.Li, hero);
                    const float spectralVis = _RgbToSpectralValue(vis, hero);
                    const float spectralBsdf =
                        _RgbToSpectralValue(bsdfValue, hero);
                    sampleContrib = _SpectralValueToRgb(
                        spectralLi * spectralBsdf *
                            absDotNL * spectralVis * ls.invPdfW * misW,
                        hero);
                } else {
                    sampleContrib = GfCompMult(
                        GfCompMult(ls.Li, bsdfValue),
                        vis) * absDotNL * ls.invPdfW * misW;
                }
            } else {
                float brdf = 1.0f / _pi<float>;
                if (hero.active) {
                    const float spectralLi = _RgbToSpectralValue(ls.Li, hero);
                    const float spectralVis = _RgbToSpectralValue(vis, hero);
                    sampleContrib = _SpectralValueToRgb(
                        spectralLi * absDotNL * brdf * spectralVis * ls.invPdfW,
                        hero);
                } else {
                    sampleContrib = GfCompMult(ls.Li, vis)
                        * absDotNL * brdf * ls.invPdfW;
                }
            }

            sampleContrib = _ClampFireflyContribution(
                sampleContrib, _fireflyClampThreshold);

            lightContrib += sampleContrib;
        }

        finalColor += lightContrib * invN;
        ++lightIndex;
    }
    return finalColor;
}

GfVec3f
HdEmbreeRenderer::_ComputeMediumDirectLighting(
    GfVec3f const& position,
    GfVec3f const& wo,
    HdEmbreeMediumState const& mediumState,
    HdEmbreeSampleDomain const& domain,
    bool includePhaseSamplingMis,
    bool spectralActive,
    float heroWavelengthNm,
    float heroWavelengthPdf) const
{
    const _HeroWavelengthState hero{
        spectralActive, heroWavelengthNm, heroWavelengthPdf};
    if (!mediumState.active || mediumState.medium.IsAbsorbingOnly()) {
        return GfVec3f(0.0f);
    }
    const bool useAdobeVolumeTransport =
        _useAdobeOpenPBR &&
        mediumState.medium.transportModel ==
            mxcpp::MediumTransportModel::AdobeOpenPBR;

    GfVec3f finalColor(0.0f);
    const HdEmbreeCategorySet emptyCategories;
    HdEmbreeCategorySet const& receiverCategories = mediumState.categories
        ? *mediumState.categories
        : emptyCategories;
    const int N = _lightSamplesPerHit;
    const float invN = 1.0f / static_cast<float>(N);

    int stratDimU = 1, stratDimV = 1;
    if (_stratifyLightSamples && N > 1) {
        stratDimU = static_cast<int>(std::sqrt(static_cast<float>(N)));
        if (stratDimU < 1) {
            stratDimU = 1;
        }
        stratDimV = (N + stratDimU - 1) / stratDimU;
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

        GfVec3f lightContrib(0.0f);
        for (int s = 0; s < N; ++s) {
            const GfVec2f sample =
                lightDomain
                    .Split(
                        HdEmbreeSampleDomainKey::MediumDirectLightSample,
                        N,
                        s)
                    .Draw2D();
            float u1 = 0.0f;
            float u2 = 0.0f;
            if (_stratifyLightSamples && N > 1) {
                int su = s % stratDimU;
                int sv = s / stratDimU;
                u1 = (su + sample[0]) / static_cast<float>(stratDimU);
                u2 = (sv + sample[1]) / static_cast<float>(stratDimV);
            } else {
                u1 = sample[0];
                u2 = sample[1];
            }

            const HdEmbreeLightSampler::LightSample ls =
                HdEmbreeLightSampler::GetLightSample(
                    light,
                    position,
                    GfVec3f(0.0f),
                    u1,
                    u2);
            if (GfIsClose(ls.Li, GfVec3f(0.0f), _minLuminanceCutoff)) {
                continue;
            }

            const GfVec3f vis = _Visibility(
                position, ls.wI, ls.wI, ls.dist * 0.99f,
                light.shadowLink, mediumState);
            if (_IsNearlyBlack(vis)) {
                continue;
            }

            const float phasePdf = useAdobeVolumeTransport
                ? mxcpp::AdobeOpenPbrEvalVolumePhasePdf(
                      mediumState.medium,
                      _ToMx(ls.wI),
                      _ToMx(wo))
                : mxcpp::PdfHenyeyGreenstein(
                      _ToMx(ls.wI),
                      _ToMx(wo),
                      mediumState.medium.anisotropy);
            if (phasePdf <= 0.0f) {
                continue;
            }

            float misW = 1.0f;
            if (includePhaseSamplingMis && !ls.delta && ls.invPdfW > 0.0f) {
                const float lightPdf = 1.0f / ls.invPdfW;
                const float effectiveLightPdf = _GetMultiSampleMisLightPdf(
                    lightPdf,
                    N);
                if (effectiveLightPdf > 0.0f) {
                    misW = mxcpp::Bsdf::PowerHeuristic(
                        effectiveLightPdf,
                        phasePdf);
                }
            }

            GfVec3f sampleContrib(0.0f);
            if (hero.active) {
                const float spectralLi = _RgbToSpectralValue(ls.Li, hero);
                const float spectralVis = _RgbToSpectralValue(vis, hero);
                sampleContrib = _SpectralValueToRgb(
                    spectralLi * spectralVis * phasePdf * ls.invPdfW * misW,
                    hero);
            } else {
                sampleContrib =
                    GfCompMult(ls.Li, vis) * phasePdf * ls.invPdfW * misW;
            }

            sampleContrib = _ClampFireflyContribution(
                sampleContrib, _fireflyClampThreshold);

            lightContrib += sampleContrib;
        }

        finalColor += lightContrib * invN;
        ++lightIndex;
    }

    return finalColor;
}

PXR_NAMESPACE_CLOSE_SCOPE
