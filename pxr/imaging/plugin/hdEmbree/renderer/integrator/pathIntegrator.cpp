//
// Copyright 2018 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
// Volume-segment transport and the main path bounce loop.

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

HdEmbreeRenderer::_VolumeTransmissionResult
HdEmbreeRenderer::_TraceVolumeTransmission(
    _VolumeTransmissionInput const& input,
    HdEmbreeMediumState const& mediumState,
    HdEmbreeSampleDomain const& domain,
    _VolumeTransmissionState* state) const
{
    if (!state || !mediumState.active) {
        return _VolumeTransmissionResult::ContinueSurface;
    }

    const _HeroWavelengthState hero{
        input.spectralActive,
        input.heroWavelengthNm,
        input.heroWavelengthPdf};

    const auto applyWeight = [&](GfVec3f const& weight) {
        if (hero.active) {
            state->spectralThroughput *= _RgbToSpectralValue(weight, hero);
        } else {
            state->throughput = GfCompMult(state->throughput, weight);
        }
    };

    const auto rgbThroughput = [&]() {
        return hero.active
            ? _SpectralScalarToRgb(state->spectralThroughput, hero)
            : state->throughput;
    };

    const auto throughputIsBlack = [&]() {
        return _IsNearlyBlack(rgbThroughput(), _minLuminanceCutoff);
    };

    const auto clampRadiance = [&](GfVec3f contribution) {
        if (state->currentPathIsCaustic) {
            contribution = _ClampFireflyContribution(
                contribution, _causticsClampThreshold);
        }
        return _ClampFireflyContribution(
            contribution, _fireflyClampThreshold);
    };

    const auto addFiniteLightHit = [&]() {
        if (!input.finiteLightLink.IsEmpty() &&
            (!state->lastScatterCategories ||
             !HdEmbreeMatchesLink(
                 input.finiteLightLink, *state->lastScatterCategories))) {
            return _VolumeTransmissionResult::Terminate;
        }
        GfVec3f lightContrib = input.finiteLightHit.Li;
        if (state->lastBsdfPdf > 0.0f &&
            input.finiteLightHit.invPdfW > 0.0f) {
            const float lightPdf = 1.0f / input.finiteLightHit.invPdfW;
            const float effectiveLightPdf = _GetMultiSampleMisLightPdf(
                lightPdf,
                _lightSamplesPerHit);
            if (effectiveLightPdf > 0.0f) {
                lightContrib *= mxcpp::Bsdf::PowerHeuristic(
                    state->lastBsdfPdf,
                    effectiveLightPdf);
            }
        }

        if (hero.active) {
            const float spectralLight =
                _RgbToSpectralValue(lightContrib, hero);
            state->radiance += clampRadiance(
                _SpectralValueToRgb(
                    state->spectralThroughput * spectralLight,
                    hero));
        } else {
            state->radiance += clampRadiance(
                GfCompMult(state->throughput, lightContrib));
        }

        return _VolumeTransmissionResult::Terminate;
    };

    const mxcpp::MediumProperties& medium = mediumState.medium;
    const bool useAdobeVolumeTransport =
        _useAdobeOpenPBR &&
        medium.transportModel == mxcpp::MediumTransportModel::AdobeOpenPBR;

    if (!medium.IsAbsorbingOnly()) {
        GfVec3f sigmaT(0.0f);
        GfVec3f sigmaS(0.0f);
        GfVec3f channelPdf(0.0f);
        int channel = 0;

        if (!useAdobeVolumeTransport) {
            sigmaT = _ToGf(medium.SigmaT());
            sigmaS = _ToGf(medium.sigmaS);
            GfVec3f albedo(0.0f);
            for (int i = 0; i < 3; ++i) {
                if (sigmaT[i] > 1.0e-6f) {
                    albedo[i] =
                        std::clamp(sigmaS[i] / sigmaT[i], 0.0f, 1.0f);
                }
            }

            mxcpp::Vec3f channelPdfMx;
            channel = mxcpp::ChannelMIS(
                _ToMx(rgbThroughput()),
                _ToMx(albedo),
                domain.Fork(HdEmbreeSampleDomainKey::MediumChannel).Draw1D(),
                &channelPdfMx);
            channelPdf =
                GfVec3f(channelPdfMx[0], channelPdfMx[1], channelPdfMx[2]);
        }

        // Chiang channel MIS: sample a single RGB tracking channel, but
        // evaluate all RGB channels against the mixture pdf.
        const auto evalTransmittance = [&](float distance) {
            return _ToGf(useAdobeVolumeTransport
                ? mxcpp::AdobeOpenPbrEvalVolumeTransmittance(
                      medium, distance)
                : mxcpp::EvalBeerTransmittance(medium, distance));
        };

        const auto evalScatterWeight = [&](float distance) {
            if (useAdobeVolumeTransport) {
                return _ToGf(
                    mxcpp::AdobeOpenPbrCalculateVolumeEventWeight(
                        medium,
                        _ToMx(rgbThroughput()),
                        distance));
            }
            const GfVec3f transmittance = evalTransmittance(distance);
            const GfVec3f pdf = GfCompMult(sigmaT, transmittance);
            const GfVec3f sampleContrib =
                GfCompMult(sigmaS, transmittance);
            const float denom = GfDot(channelPdf, pdf);
            if (!std::isfinite(denom) || denom <= _volumePdfEps) {
                return GfVec3f(0.0f);
            }
            return sampleContrib * (1.0f / denom);
        };

        const auto evalTransmittanceWeight = [&](float distance) {
            if (useAdobeVolumeTransport) {
                return _ToGf(
                    mxcpp::AdobeOpenPbrCalculateVolumeSurfaceWeight(
                        medium,
                        _ToMx(rgbThroughput()),
                        distance));
            }
            const GfVec3f transmittance = evalTransmittance(distance);
            const float denom = GfDot(channelPdf, transmittance);
            if (!std::isfinite(denom) || denom <= _volumePdfEps) {
                return GfVec3f(0.0f);
            }
            return transmittance * (1.0f / denom);
        };

        const float scatterDist = useAdobeVolumeTransport
            ? mxcpp::AdobeOpenPbrSampleVolumeEventDistance(
                  medium,
                  _ToMx(rgbThroughput()),
                  domain
                      .Fork(HdEmbreeSampleDomainKey::MediumFreeFlight)
                      .Draw1D())
            : mxcpp::SampleFreeFlightChannel(
                  medium,
                  channel,
                  domain
                      .Fork(HdEmbreeSampleDomainKey::MediumFreeFlight)
                      .Draw1D());
        const float maxTravelDist =
            std::min(input.surfaceDist, input.finiteLightDist);
        if (scatterDist < maxTravelDist) {
            const GfVec3f scatterWeight = evalScatterWeight(scatterDist);
            applyWeight(scatterWeight);

            if (throughputIsBlack()) {
                return _VolumeTransmissionResult::Terminate;
            }

            const GfVec3f scatterPos =
                input.rayOrigin + input.rayDir * scatterDist;
            const GfVec3f wo = -input.rayDir;
            const GfVec3f direct = _ComputeMediumDirectLighting(
                scatterPos,
                wo,
                mediumState,
                domain.Fork(HdEmbreeSampleDomainKey::MediumDirectLighting),
                input.bounce < _maxBounces,
                hero.active,
                hero.wavelengthNm,
                hero.pdf);
            if (hero.active) {
                state->radiance += clampRadiance(
                    direct * state->spectralThroughput);
            } else {
                state->radiance += clampRadiance(
                    GfCompMult(state->throughput, direct));
            }

            if (input.bounce >= _maxBounces) {
                return _VolumeTransmissionResult::Terminate;
            }

            if (input.bounce >= _minBouncesBeforeRR) {
                float q = hero.active
                    ? std::max({
                        _SpectralScalarToRgb(
                            state->spectralThroughput, hero)[0],
                        _SpectralScalarToRgb(
                            state->spectralThroughput, hero)[1],
                        _SpectralScalarToRgb(
                            state->spectralThroughput, hero)[2]})
                    : std::max({
                        state->throughput[0],
                        state->throughput[1],
                        state->throughput[2]});
                q = std::min(q, 0.95f);
                if (q <= 0.0f ||
                    domain
                        .Fork(HdEmbreeSampleDomainKey::MediumRussianRoulette)
                        .Draw1D() > q) {
                    return _VolumeTransmissionResult::Terminate;
                }
                if (hero.active) {
                    state->spectralThroughput /= q;
                } else {
                    state->throughput /= q;
                }
            }

            const GfVec2f phaseSample =
                domain.Fork(HdEmbreeSampleDomainKey::MediumPhase).Draw2D();
            const GfVec3f wi = _ToGf(useAdobeVolumeTransport
                ? mxcpp::AdobeOpenPbrSampleVolumePhase(
                      medium,
                      _ToMx(wo),
                      phaseSample[0],
                      phaseSample[1])
                : mxcpp::SampleHenyeyGreenstein(
                      _ToMx(wo),
                      medium.anisotropy,
                      phaseSample[0],
                      phaseSample[1]));
            const float phasePdf = useAdobeVolumeTransport
                ? mxcpp::AdobeOpenPbrEvalVolumePhasePdf(
                      medium,
                      _ToMx(wi),
                      _ToMx(wo))
                : mxcpp::PdfHenyeyGreenstein(
                      _ToMx(wi),
                      _ToMx(wo),
                      medium.anisotropy);
            if (phasePdf <= 0.0f || !std::isfinite(phasePdf)) {
                return _VolumeTransmissionResult::Terminate;
            }

            state->rayOrigin =
                _OffsetRayOrigin(scatterPos, wi, wi, 1e-4f);
            state->rayDir = wi;
            state->rayDiff.hasDifferentials = false;
            state->lastBsdfPdf = phasePdf;
            state->lastScatterWasMedium = true;
            state->lastScatterCategories = mediumState.categories;
            state->anyNonSpecularBounces = true;
            state->hasDiffuseLikeAncestor = true;
            state->isFirstBounce = false;
            return _VolumeTransmissionResult::ContinueRay;
        }

        if (input.hasFiniteLightHit &&
            input.finiteLightDist < input.surfaceDist &&
            input.finiteLightDist > 0.0f) {
            applyWeight(evalTransmittanceWeight(input.finiteLightDist));

            if (throughputIsBlack()) {
                return _VolumeTransmissionResult::Terminate;
            }

            return addFiniteLightHit();
        }

        if (std::isfinite(input.surfaceDist) && input.surfaceDist > 0.0f) {
            applyWeight(evalTransmittanceWeight(input.surfaceDist));
        } else {
            return _VolumeTransmissionResult::Terminate;
        }
    } else if (input.hasFiniteLightHit &&
               input.finiteLightDist < input.surfaceDist &&
               input.finiteLightDist > 0.0f) {
        const GfVec3f transmittance = _ToGf(useAdobeVolumeTransport
            ? mxcpp::AdobeOpenPbrEvalVolumeTransmittance(
                  medium, input.finiteLightDist)
            : mxcpp::EvalBeerTransmittance(
                  medium, input.finiteLightDist));
        applyWeight(transmittance);

        if (throughputIsBlack()) {
            return _VolumeTransmissionResult::Terminate;
        }

        return addFiniteLightHit();
    } else if (std::isfinite(input.surfaceDist) && input.surfaceDist > 0.0f) {
        const GfVec3f transmittance = _ToGf(useAdobeVolumeTransport
            ? mxcpp::AdobeOpenPbrEvalVolumeTransmittance(
                  medium, input.surfaceDist)
            : mxcpp::EvalBeerTransmittance(
                  medium, input.surfaceDist));
        applyWeight(transmittance);
    } else {
        return _VolumeTransmissionResult::Terminate;
    }

    if (throughputIsBlack()) {
        return _VolumeTransmissionResult::Terminate;
    }

    return _VolumeTransmissionResult::ContinueSurface;
}

GfVec3f
HdEmbreeRenderer::_TracePath(
    GfVec3f const& origin,
    GfVec3f const& dir,
    HdEmbreeRayDifferential const& rayDiff,
    HdEmbreeSampleDomain const& domain) const
{
    HdEmbreeRayDifferential currentRayDiff = rayDiff;
    GfVec3f radiance(0.0f);
    GfVec3f throughput(1.0f);
    float spectralThroughput = 1.0f;
    _HeroWavelengthState hero;
    GfVec3f rayOrigin = origin;
    GfVec3f rayDir = dir;
    float lastBsdfPdf = 0.0f;
    bool lastScatterWasMedium = false;
    HdEmbreeCategorySet const* lastScatterCategories = nullptr;
    HdEmbreeLightSampler::SamplingMode lastLightSamplingMode =
        HdEmbreeLightSampler::SamplingMode::FullSphere;
    GfVec3f lastLightSamplingNormal(0.0f);
    bool isFirstBounce = true;
    bool anyNonSpecularBounces = false;
    // Tracks the stricter caustic-class ancestor used by enableCaustics=false.
    // Rough glossy dielectric traversal is finite-PDF but not diffuse-like;
    // otherwise camera-visible rough glass can be killed at its own exit face.
    bool hasDiffuseLikeAncestor = false;
    bool currentPathIsCaustic = false;
    bool useSyntheticLambertian = false;
    HdEmbreeSssOutput syntheticLambertianExit;
    HdEmbreeMediumState currentMedium;

    // Per-bounce derivative state for ray differential propagation
    GfVec3f lastDPdu(0.0f), lastDPdv(0.0f);
    GfVec3f lastDndu(0.0f), lastDndv(0.0f);
    GfVec3f lastDpdx(0.0f), lastDpdy(0.0f);
    float lastDudx = 0, lastDvdx = 0, lastDudy = 0, lastDvdy = 0;

    const auto addRadiance = [&](GfVec3f contribution) {
        if (currentPathIsCaustic) {
            contribution = _ClampFireflyContribution(
                contribution, _causticsClampThreshold);
        }
        radiance += _ClampFireflyContribution(
            contribution, _fireflyClampThreshold);
    };

    const int maxBounces = std::max(0, _maxBounces);

    for (int bounce = 0, pathEvent = 0;
         bounce <= maxBounces + 1;
         ++bounce, ++pathEvent) {
        const bool emitterOnlyBounce = bounce > maxBounces;
        const HdEmbreeSampleDomain bounceDomain =
            domain.Chain(HdEmbreeSampleDomainKey::PathBounce, pathEvent);

        RTCRayHit rayHit;
        rayHit.ray.flags = 0;
        const bool syntheticLambertianHit = useSyntheticLambertian;
        if (syntheticLambertianHit) {
            if (!_PopulateSssExitRayHit(&rayHit, syntheticLambertianExit)) {
                break;
            }
        } else {
            _PopulateRayHit(&rayHit, rayOrigin, rayDir,
                            isFirstBounce ? 0.0f : 1e-4f,
                            std::numeric_limits<float>::max(),
                            emitterOnlyBounce
                                ? HdEmbree_RayMask::Light
                                : HdEmbree_RayMask::Camera);
            rtcIntersect1(_scene, &rayHit);
        }

        HdEmbreeLightSampler::LightSample finiteLightHit{};
        TfToken finiteLightLink;
        const bool hitLightGeometry =
            !syntheticLambertianHit &&
            _EvaluateLightGeometryHit(
                rayHit, rayOrigin, rayDir, &finiteLightHit, &finiteLightLink);
        const float surfaceDist =
            rayHit.hit.geomID != RTC_INVALID_GEOMETRY_ID && !hitLightGeometry
                ? rayHit.ray.tfar
                : std::numeric_limits<float>::infinity();
        const bool hasAnalyticFiniteLightHit =
            !syntheticLambertianHit &&
            !hitLightGeometry &&
            !isFirstBounce &&
            _FindNearestFiniteLightHit(
                rayOrigin,
                rayDir,
                surfaceDist,
                &finiteLightHit,
                &finiteLightLink);
        const bool hasFiniteLightHit =
            hitLightGeometry || hasAnalyticFiniteLightHit;
        const float finiteLightDist =
            hasFiniteLightHit
                ? finiteLightHit.dist
                : std::numeric_limits<float>::infinity();

        if (currentMedium.active) {
            _VolumeTransmissionInput volumeInput;
            volumeInput.rayOrigin = rayOrigin;
            volumeInput.rayDir = rayDir;
            volumeInput.surfaceDist = surfaceDist;
            volumeInput.hasFiniteLightHit = hasFiniteLightHit;
            volumeInput.finiteLightHit = finiteLightHit;
            volumeInput.finiteLightLink = finiteLightLink;
            volumeInput.finiteLightDist = finiteLightDist;
            volumeInput.bounce = bounce;
            volumeInput.spectralActive = hero.active;
            volumeInput.heroWavelengthNm = hero.wavelengthNm;
            volumeInput.heroWavelengthPdf = hero.pdf;

            _VolumeTransmissionState volumeState;
            volumeState.radiance = radiance;
            volumeState.throughput = throughput;
            volumeState.spectralThroughput = spectralThroughput;
            volumeState.rayOrigin = rayOrigin;
            volumeState.rayDir = rayDir;
            volumeState.rayDiff = currentRayDiff;
            volumeState.lastBsdfPdf = lastBsdfPdf;
            volumeState.lastScatterWasMedium = lastScatterWasMedium;
            volumeState.anyNonSpecularBounces = anyNonSpecularBounces;
            volumeState.hasDiffuseLikeAncestor = hasDiffuseLikeAncestor;
            volumeState.currentPathIsCaustic = currentPathIsCaustic;
            volumeState.isFirstBounce = isFirstBounce;
            volumeState.lastScatterCategories = lastScatterCategories;

            const _VolumeTransmissionResult volumeResult =
                _TraceVolumeTransmission(
                    volumeInput,
                    currentMedium,
                    bounceDomain,
                    &volumeState);

            radiance = volumeState.radiance;
            throughput = volumeState.throughput;
            spectralThroughput = volumeState.spectralThroughput;
            rayOrigin = volumeState.rayOrigin;
            rayDir = volumeState.rayDir;
            currentRayDiff = volumeState.rayDiff;
            lastBsdfPdf = volumeState.lastBsdfPdf;
            lastScatterWasMedium = volumeState.lastScatterWasMedium;
            anyNonSpecularBounces = volumeState.anyNonSpecularBounces;
            hasDiffuseLikeAncestor = volumeState.hasDiffuseLikeAncestor;
            currentPathIsCaustic = volumeState.currentPathIsCaustic;
            isFirstBounce = volumeState.isFirstBounce;
            lastScatterCategories = volumeState.lastScatterCategories;

            if (volumeResult == _VolumeTransmissionResult::Terminate) {
                break;
            }
            if (volumeResult == _VolumeTransmissionResult::ContinueRay) {
                continue;
            }
        }

        if (hasFiniteLightHit && finiteLightDist < surfaceDist) {
            if (!isFirstBounce && !finiteLightLink.IsEmpty() &&
                (!lastScatterCategories ||
                 !HdEmbreeMatchesLink(
                     finiteLightLink, *lastScatterCategories))) {
                break;
            }
            GfVec3f lightContrib = finiteLightHit.Li;
            if (lastBsdfPdf > 0.0f && finiteLightHit.invPdfW > 0.0f) {
                const float lightPdf = 1.0f / finiteLightHit.invPdfW;
                const float effectiveLightPdf = _GetMultiSampleMisLightPdf(
                    lightPdf,
                    _lightSamplesPerHit);
                if (effectiveLightPdf > 0.0f) {
                    lightContrib *= mxcpp::Bsdf::PowerHeuristic(
                        lastBsdfPdf,
                        effectiveLightPdf);
                }
            }

            if (hero.active) {
                const float spectralLight =
                    _RgbToSpectralValue(lightContrib, hero);
                addRadiance(
                    _SpectralValueToRgb(
                        spectralThroughput * spectralLight,
                        hero));
            } else {
                addRadiance(GfCompMult(throughput, lightContrib));
            }
            break;
        }

        // --- Miss: infinite light contribution ---
        if (rayHit.hit.geomID == RTC_INVALID_GEOMETRY_ID) {
            for (auto const& it : _lights.GetLights()) {
                if (!it.second) {
                    continue;
                }

                auto const& light = *it.second;
                if (!light.visible ||
                    !std::holds_alternative<HdEmbree_Distant>(
                        light.lightVariant) ||
                    (!isFirstBounce && !light.lightLink.IsEmpty() &&
                     (!lastScatterCategories ||
                      !HdEmbreeMatchesLink(
                          light.lightLink, *lastScatterCategories)))) {
                    continue;
                }

                const HdEmbreeLightSampler::LightSample ls =
                    HdEmbreeLightSampler::EvaluateLightDirection(
                        light, rayOrigin, rayDir);
                if (!ls.valid) {
                    continue;
                }

                GfVec3f distantContrib = ls.Li;
                if (!isFirstBounce && lastBsdfPdf > 0.0f &&
                    ls.invPdfW > 0.0f) {
                    const float lightPdf = _GetMultiSampleMisLightPdf(
                        1.0f / ls.invPdfW,
                        _lightSamplesPerHit);
                    distantContrib *= mxcpp::Bsdf::PowerHeuristic(
                        lastBsdfPdf, lightPdf);
                }

                if (hero.active) {
                    const float spectralDistant =
                        _RgbToSpectralValue(distantContrib, hero);
                    addRadiance(
                        _SpectralValueToRgb(
                            spectralThroughput * spectralDistant,
                            hero));
                } else {
                    addRadiance(GfCompMult(throughput, distantContrib));
                }
            }

            for (auto* dome : _lights.GetDomes()) {
                if (!dome->visible ||
                    (!isFirstBounce &&
                     !dome->lightLink.IsEmpty() &&
                     (!lastScatterCategories ||
                      !HdEmbreeMatchesLink(
                          dome->lightLink,
                          *lastScatterCategories)))) {
                    continue;
                }
                const HdEmbreeLightSampler::SamplingMode domeSamplingMode =
                    (!lastScatterWasMedium)
                        ? lastLightSamplingMode
                        : HdEmbreeLightSampler::SamplingMode::FullSphere;
                HdEmbreeLightSampler::LightSample ls;
                if (domeSamplingMode ==
                    HdEmbreeLightSampler::SamplingMode::ReflectionHemisphere) {
                    ls = HdEmbreeLightSampler::EvaluateDomeLightDirection(
                        *dome,
                        rayDir,
                        lastLightSamplingNormal,
                        domeSamplingMode);
                } else {
                    ls = HdEmbreeLightSampler::EvaluateDomeLightDirection(
                        *dome, rayDir);
                }
                GfVec3f domeContrib = ls.Li;

                if (!isFirstBounce && lastBsdfPdf > 0.0f) {
                    // MIS weight for BSDF sampling strategy hitting dome.
                    float domePdf = (ls.invPdfW > 0.0f)
                        ? 1.0f / ls.invPdfW : 0.0f;
                    domePdf = _GetMultiSampleMisLightPdf(
                        domePdf,
                        _lightSamplesPerHit);
                    float misW = mxcpp::Bsdf::PowerHeuristic(
                        lastBsdfPdf, domePdf);
                    domeContrib *= misW;
                }

                if (hero.active) {
                    const float spectralDome =
                        _RgbToSpectralValue(domeContrib, hero);
                    addRadiance(
                        _SpectralValueToRgb(
                            spectralThroughput * spectralDome,
                            hero));
                } else {
                    addRadiance(GfCompMult(throughput, domeContrib));
                }
            }
            break;
        }

        if (emitterOnlyBounce) {
            break;
        }

        // --- Process hit ---
        const HdEmbreeInstanceContext *instanceContext =
            static_cast<HdEmbreeInstanceContext*>(
                rtcGetGeometryUserData(
                    rtcGetGeometry(_scene, rayHit.hit.instID[0])));

        const HdEmbreePrototypeContext *prototypeContext =
            static_cast<HdEmbreePrototypeContext*>(
                rtcGetGeometryUserData(
                    rtcGetGeometry(instanceContext->rootScene,
                                   rayHit.hit.geomID)));

        GfVec3f hitPos = GfVec3f(
            rayHit.ray.org_x + rayHit.ray.tfar * rayHit.ray.dir_x,
            rayHit.ray.org_y + rayHit.ray.tfar * rayHit.ray.dir_y,
            rayHit.ray.org_z + rayHit.ray.tfar * rayHit.ray.dir_z);

        // Normals.
        //
        // - `geometricNormal`: unflipped smooth shading normal (interpolated
        //   vertex / subdiv-limit normal, else face Ng). Kept in its
        //   world-oriented form because later stages (medium-boundary
        //   crossing detection, next-ray self-intersection bias) rely on
        //   its sign relative to world orientation rather than relative
        //   to the ray direction.
        // - `faceNg`: the true geometric face normal from Embree, then
        //   face-forwarded toward `wo`. This is Cycles' `sd->Ng` and is
        //   used for validity checks that can't tolerate shading-normal
        //   lies (e.g., rejecting an SSS entry whose refracted direction
        //   is outward relative to the face).
        // - `normal`: the shading normal used for BSDF evaluation /
        //   sampling. Starts from `geometricNormal`, then face-forwarded
        //   against `wo`, then perturbed by the material's tangent-space
        //   normal map. This is Cycles' `sd->N`.
        GfVec3f geometricNormal = _ResolveObjectSpaceNormal(
            prototypeContext, instanceContext->rootScene, rayHit.hit.geomID,
            rayHit);
        geometricNormal =
            instanceContext->objectToWorldMatrix.TransformDir(geometricNormal);
        geometricNormal.Normalize();

        GfVec3f faceNg(rayHit.hit.Ng_x, rayHit.hit.Ng_y, rayHit.hit.Ng_z);
        faceNg =
            instanceContext->objectToWorldMatrix.TransformDir(faceNg);
        faceNg.Normalize();

        GfVec3f normal = geometricNormal;

        GfVec3f wo = -rayDir;

        // Double-sided check
        bool doubleSided = false;
        HdEmbreePrototypeContext const* mesh = prototypeContext;
        if (mesh) {
            doubleSided = prototypeContext->doubleSided;
        }

        // Face-forward the shading normal and the face Ng against the
        // incoming ray, matching pbrt-v4 and Cycles. USD's doubleSided=
        // false doesn't prescribe back-face behavior in a path tracer;
        // without flipping here, back-face hits would leak light (BSDF
        // sampling would continue through an opaque surface since the
        // sampled hemisphere is oriented around a normal pointing away
        // from wo). Flipping both shading N and face Ng together (Cycles
        // kernel/geom/shader_data.h) also prevents shading-normal
        // terminator artifacts from rejecting SSS entries on otherwise
        // front-facing hits. `geometricNormal` is intentionally left
        // unflipped because downstream medium/bias code needs its world
        // orientation.
        if (GfDot(faceNg, wo) < 0.0f) {
            faceNg = -faceNg;
        }
        if (GfDot(normal, wo) < 0.0f) {
            normal = -normal;
        }

        // Build shading context via shared helper, also retrieving
        // normal derivatives for bounce propagation.
        mxcpp::ShadingContext ctx = _BuildShadingContext(
            rayHit, currentRayDiff,
            instanceContext, prototypeContext, hitPos, normal,
            &lastDndu, &lastDndv);
        _GeomPropCallbackData cbData{
            &prototypeContext->primvarMapByString,
            rayHit.hit.primID, rayHit.hit.u, rayHit.hit.v};
        ctx.geomPropLookup = &_SampleGeomProp;
        ctx.geomPropUserData = &cbData;
        ctx.uniformProps = &prototypeContext->uniformPrimvarMap;

        // Capture surface derivatives from ctx (already computed by
        // _BuildShadingContext) for bounce propagation.
        lastDPdu = _ToGf(ctx.dPdu);
        lastDPdv = _ToGf(ctx.dPdv);
        lastDpdx = _ToGf(ctx.dPdx);
        lastDpdy = _ToGf(ctx.dPdy);
        lastDudx = ctx.dudx;
        lastDvdx = ctx.dvdx;
        lastDudy = ctx.dudy;
        lastDvdy = ctx.dvdy;

        GfVec3f tangent = _ToGf(ctx.tangent);
        GfVec3f bitangent = _ToGf(ctx.bitangent);

        // --- Evaluate material ---
        mxcpp::EvalGraph *evalGraph = prototypeContext->material ? prototypeContext->material->evalGraph : nullptr;

        mxcpp::SurfaceClosure closure;
        bool hasClosure = false;

        if (evalGraph && !useSyntheticLambertian) {
            try {
                mxcpp::EvalOptions evalOptions;
                evalOptions.useAdobeOpenPBR = _useAdobeOpenPBR;
                closure = evalGraph->Evaluate(ctx, evalOptions);
                hasClosure = true;
            } catch (...) {
                hasClosure = false;
            }
        }

        // SSS exit synthesis: at the exit-side surface hit immediately
        // following HdEmbreeRandomWalkSSS, replace the evaluated closure
        // with a weight-1.0 Lambertian so the random-walk's albedo isn't
        // double-counted. Cycles uses the same trick.
        if (useSyntheticLambertian) {
            const bool hitOwner =
                (rayHit.hit.instID[0] ==
                 syntheticLambertianExit.exitInstanceId) &&
                (rayHit.hit.geomID == syntheticLambertianExit.exitGeomId);
            if (hitOwner) {
                closure = mxcpp::SurfaceClosure{};
                closure.baseColor = mxcpp::Vec3f(1.0f);
                closure.roughness = 1.0f;
                // SurfaceClosure defaults include a dielectric specular
                // lobe. Clear it so the SSS exit matches Cycles'
                // DiffuseBsdf replacement instead of adding Fresnel loss.
                closure.specular = 0.0f;
                closure.specularColor = mxcpp::Vec3f(0.0f);
                closure.opacity = 1.0f;
                closure.presence = 1.0f;
                hasClosure = true;
            }
            // Always clear flag; even on mismatch we don't want it to linger.
            useSyntheticLambertian = false;
            syntheticLambertianExit = HdEmbreeSssOutput{};
        }

        mxcpp::Vec3f resolvedNormal;
        if (hasClosure &&
            closure.ResolveNormal(
                _ToMx(tangent),
                _ToMx(bitangent),
                _ToMx(normal),
                &resolvedNormal)) {
            normal = _ToGf(resolvedNormal);
            // Re-orient toward the ray (face-forward, matches the geometric
            // normal treatment above).
            if (GfDot(normal, wo) < 0.0f) {
                normal = -normal;
            }
        }

        // Disabled for now: this regularization changes thick-glass caustic
        // color too much by widening the refractive lobe on indirect paths.
        //
        // // --- Path regularization ---
        // // After the first non-specular bounce, widen narrow specular lobes
        // // to reduce fireflies from sharp BSDFs on indirect paths.
        // if (hasClosure && anyNonSpecularBounces && _enableCaustics) {
        //     closure.Regularize();
        // }

        // --- Stochastic opacity pass-through ---
        if (hasClosure && closure.presence < 1.0f) {
            const auto advancePastHit = [&]() {
                const float advance = rayHit.ray.tfar + 1e-4f;
                rayOrigin = hitPos + rayDir * 1e-4f;
                if (currentRayDiff.hasDifferentials) {
                    currentRayDiff.rxOrigin += rayDir * advance;
                    currentRayDiff.ryOrigin += rayDir * advance;
                }
                // Null presence pass-through is not a scattering event. Keep
                // MIS / first-bounce state from the previous real interaction.
                --bounce;
            };

            const float presence = _Clamp01(closure.presence);
            if (presence <= 0.0f) {
                advancePastHit();
                continue;
            }
            // Treat presence as the probability of interaction. The strict
            // u < presence test keeps endpoint-zero sampler values from
            // interacting when the surface is fully absent.
            if (bounceDomain
                    .Fork(HdEmbreeSampleDomainKey::Presence)
                    .Draw1D() >= presence) {
                advancePastHit();
                continue;
            }
            // We chose to interact; clear the stochastic presence term so
            // EvalSurface / SampleSurface don't attenuate a second time.
            closure.presence = 1.0f;
        }

        mxcpp::SurfaceClosure causticPrunedClosure;
        const bool volumeOnlyBoundary =
            hasClosure && _IsVolumeOnlyBoundary(closure) && mesh;
        const mxcpp::SurfaceClosure* bsdfClosure =
            hasClosure ? &closure : nullptr;
        bool hasBsdfClosure = hasClosure && !volumeOnlyBoundary;
        if (hasClosure && hasDiffuseLikeAncestor && !_enableCaustics) {
            causticPrunedClosure =
                mxcpp::Bsdf::PruneCausticClassLobes(closure);
            // A tree-based material can prune down to no remaining BSDF
            // lobes. Keep the material closure for emission/opacity state,
            // but do not fall back to the legacy summary BSDF in that case.
            if (closure.HasBsdfTree() && !causticPrunedClosure.HasBsdfTree()) {
                bsdfClosure = nullptr;
                hasBsdfClosure = false;
            } else {
                bsdfClosure = &causticPrunedClosure;
            }
        }

        if (hasBsdfClosure && bsdfClosure->HasDispersion() && !hero.active) {
            hero.active = true;
            hero.wavelengthNm =
                mxcpp::Spectral::SampleHeroWavelength(
                    bounceDomain
                        .Fork(HdEmbreeSampleDomainKey::Wavelength)
                        .Draw1D());
            hero.pdf = mxcpp::Spectral::HeroWavelengthPdf();
            spectralThroughput = _RgbToSpectralValue(throughput, hero);
        }

        const GfVec3f bsdfNormal = hasClosure
            ? _GetBsdfNormal(closure, normal, geometricNormal, wo)
            : normal;

        mxcpp::AdobeOpenPbrPreparedSurface adobeOpenPbrSurface;
        if (hasBsdfClosure) {
            adobeOpenPbrSurface = mxcpp::PrepareAdobeOpenPbrSurface(
                *bsdfClosure,
                _ToMx(bsdfNormal),
                _ToMx(wo));
        }

        mxcpp::Bsdf::BsdfSample bs;
        bool hasBsdfSample = false;
        if (hasBsdfClosure && bounce <= maxBounces) {
            const GfVec3f bsdfSample =
                bounceDomain
                    .Fork(HdEmbreeSampleDomainKey::BsdfSample)
                    .Draw3D();
            if (adobeOpenPbrSurface.valid) {
                bs = mxcpp::SamplePreparedAdobeOpenPbrSurface(
                    adobeOpenPbrSurface,
                    bsdfSample[0],
                    bsdfSample[1],
                    bsdfSample[2]);
            } else {
                bs = mxcpp::Bsdf::SampleSurface(
                    *bsdfClosure, _ToMx(bsdfNormal), _ToMx(wo),
                    bsdfSample[0], bsdfSample[1], bsdfSample[2],
                    hero.wavelengthNm);
            }
            hasBsdfSample = bs.isSubsurface || bs.pdf > 0.0f;
        }

        if (hasBsdfClosure &&
            hasBsdfSample &&
            bounce < maxBounces &&
            bs.isSubsurface &&
            bsdfClosure->HasSubsurfaceScattering() &&
            mesh) {

            // Entry direction from BSDF (GGX VNDF refraction via specular
            // roughness + IOR).
            //
            // We pass the face-forwarded shading normal (Cycles `sd->N`),
            // not the raw face Ng. This matches Cycles' `bssrdf->N = sd->N`
            // and keeps normal-map perturbations in the refraction basis.
            // Using the unmodified smooth normal here caused the early
            // `cosNI <= 0` rejection to fire on shading-normal terminator
            // hits, which in turn caused the entire path to break with
            // zero radiance (black artifacts at grazing regions).
            mxcpp::Vec3f entryDirMx;
            if (bs.hasSubsurfaceEntryDirection) {
                entryDirMx = bs.wi;
            } else {
                const GfVec2f entrySample =
                    bounceDomain
                        .Fork(HdEmbreeSampleDomainKey::SssEntryDirection)
                        .Draw2D();
                if (!mxcpp::Bsdf::SampleSubsurfaceEntry(
                        *bsdfClosure, _ToMx(normal), _ToMx(wo),
                        entrySample[0], entrySample[1], entryDirMx)) {
                    break;
                }
            }
            const GfVec3f entryDir = _ToGf(entryDirMx);

            // Cycles-parity secondary check: reject if the refracted
            // direction still points outward relative to the true face
            // normal. This can happen when the shading normal (used for
            // the GGX-VNDF + Snell refraction) tilts far enough from the
            // face that an "inward" direction in shading-normal space is
            // actually above the geometric face plane. See
            // `subsurface_bounce()` in
            // Cycles (kernel/integrator/subsurface.h): the corresponding
            // LABEL_NONE also terminates the bounce.
            if (GfDot(faceNg, entryDir) >= 0.0f) {
                break;
            }

            // Entry weight (bs.f = Vec3f(subsurface_weight)).
            if (hero.active) {
                float w = mxcpp::Spectral::RgbToSpectralValue(
                    bs.f, hero.wavelengthNm);
                if (!std::isfinite(w) || w < 0.0f) w = 0.0f;
                spectralThroughput *= w;
            } else {
                GfVec3f w = _ToGf(bs.f);
                for (int i = 0; i < 3; ++i) {
                    if (!std::isfinite(w[i]) || w[i] < 0.0f) w[i] = 0.0f;
                }
                throughput = GfCompMult(throughput, w);
            }

            const GfVec3f rgbTpCheck = hero.active
                ? _SpectralScalarToRgb(spectralThroughput, hero)
                : throughput;
            if (_IsNearlyBlack(rgbTpCheck, _minLuminanceCutoff)) {
                break;
            }

            // Random walk inside the medium. The Dwivedi guide axis is the
            // face-forwarded shading normal (Cycles' `subsurface_state.N =
            // sd->N`); using the raw smooth normal here would leave the
            // guide axis pointing away from `wo` on terminator hits.
            HdEmbreeSssInput sssIn;
            sssIn.entryPos = hitPos;
            sssIn.entryGeomNormal = normal;
            sssIn.entryDir = entryDir;
            sssIn.albedo = _ToGf(bsdfClosure->subsurfaceColor);
            sssIn.radius = GfCompMult(_ToGf(bsdfClosure->subsurfaceRadius),
                                      _ToGf(bsdfClosure->subsurfaceRadiusScale));
            sssIn.anisotropy =
                std::clamp(bsdfClosure->subsurfaceAnisotropy, -0.99f, 0.99f);
            if (bsdfClosure->hasPrecomputedSubsurfaceMedium &&
                !bsdfClosure->precomputedSubsurfaceMedium.IsVacuum()) {
                sssIn.usePrecomputedCoefficients = true;
                sssIn.precomputedSigmaA =
                    _ToGf(bsdfClosure->precomputedSubsurfaceMedium.sigmaA);
                sssIn.precomputedSigmaS =
                    _ToGf(bsdfClosure->precomputedSubsurfaceMedium.sigmaS);
                sssIn.anisotropy = std::clamp(
                    bsdfClosure->precomputedSubsurfaceMedium.anisotropy,
                    -0.99f,
                    0.99f);
            }
            sssIn.ior = std::max(bsdfClosure->specularIor, 1.0f);
            sssIn.ownerInstanceId = rayHit.hit.instID[0];
            sssIn.ownerGeomId = rayHit.hit.geomID;
            sssIn.ownerScene = instanceContext->rootScene;
            sssIn.objectToWorldMatrix = instanceContext->objectToWorldMatrix;
            sssIn.worldToObjectMatrix = instanceContext->worldToObjectMatrix;

            HdEmbreeSssOutput sssOut = HdEmbreeRandomWalkSSS(
                sssIn,
                bounceDomain.Fork(HdEmbreeSampleDomainKey::SssEntry),
                _scene);
            _sssCallCount.fetch_add(1, std::memory_order_relaxed);
            _sssWalkStepCount.fetch_add(
                sssOut.walkSteps, std::memory_order_relaxed);
            _sssIntersectionCount.fetch_add(
                sssOut.intersectionTests, std::memory_order_relaxed);
            if (sssOut.success) {
                _sssSuccessCount.fetch_add(1, std::memory_order_relaxed);
            }

            if (!sssOut.success) {
                break;
            }

            // Apply the random-walk multiplier to throughput.
            if (hero.active) {
                float w = _RgbToSpectralValue(sssOut.throughputWeight, hero);
                if (!std::isfinite(w) || w < 0.0f) w = 0.0f;
                spectralThroughput *= w;
            } else {
                GfVec3f w = sssOut.throughputWeight;
                for (int i = 0; i < 3; ++i) {
                    if (!std::isfinite(w[i]) || w[i] < 0.0f) w[i] = 0.0f;
                }
                throughput = GfCompMult(throughput, w);
            }

            const GfVec3f rgbTpAfter = hero.active
                ? _SpectralScalarToRgb(spectralThroughput, hero)
                : throughput;
            if (_IsNearlyBlack(rgbTpAfter, _minLuminanceCutoff)) {
                break;
            }

            // Process the SSS exit as a synthetic surface hit on the next
            // loop iteration, using the intersection data returned by the
            // random walk. This keeps the Cycles-style Lambertian exit BRDF
            // while avoiding a fragile same-surface re-hit.
            rayOrigin = sssOut.exitPos;
            rayDir = -sssOut.exitDir;
            currentRayDiff.hasDifferentials = false;
            lastBsdfPdf = 0.0f;
            lastScatterWasMedium = false;
            anyNonSpecularBounces = true;
            hasDiffuseLikeAncestor = true;
            isFirstBounce = false;
            syntheticLambertianExit = sssOut;
            useSyntheticLambertian = true;

            // Clear any medium state. The new Phase 1 design has NO
            // subsurface-in-medium state; SSS is fully handled inside
            // HdEmbreeRandomWalkSSS.
            currentMedium = HdEmbreeMediumState();

            // Count the entire SSS closure (entry + random walk + exit
            // Lambertian) as a single bounce, matching how a plain diffuse
            // surface hit costs 1 bounce. Without this, the synthetic
            // Lambertian exit iteration would consume an extra bounce,
            // effectively halving the user's indirect-light budget for
            // paths that traverse SSS.
            --bounce;
            continue;
        }

        // --- Emissive ---
        if (hasClosure) {
            if (hero.active) {
                const float spectralEmissive = mxcpp::Spectral::RgbToSpectralValue(
                    closure.emissiveColor, hero.wavelengthNm);
                addRadiance(
                    _SpectralValueToRgb(
                        spectralThroughput * spectralEmissive,
                        hero));
            } else {
                addRadiance(
                    GfCompMult(throughput, _ToGf(closure.emissiveColor)));
            }
        }

        // --- Direct lighting (NEE) with MIS ---
        GfVec3f direct(0.0f);
        if (hasBsdfClosure) {
            direct = _ComputeDirectLightingMIS(
                hitPos,
                bsdfNormal,
                geometricNormal,
                wo,
                bounceDomain.Fork(HdEmbreeSampleDomainKey::DirectLighting),
                doubleSided,
                bounce <= maxBounces,
                bsdfClosure,
                instanceContext->categories,
                currentMedium,
                hero.active,
                hero.wavelengthNm,
                hero.pdf,
                adobeOpenPbrSurface.valid ? &adobeOpenPbrSurface : nullptr);
        } else if (!hasClosure) {
            GfVec3f matColor = _enableSceneColors
                ? _ToGf(ctx.displayColor) : GfVec3f(0.5f);
            mxcpp::SurfaceClosure fallback;
            fallback.baseColor = _ToMx(matColor);
            fallback.roughness = 1.0f;
            fallback.metallic = 0.0f;
            fallback.specular = 0.0f;
            fallback.specularColor = mxcpp::Vec3f(1.0f);
            fallback.specularIor = 1.5f;
            fallback.opacity = 1.0f;
            direct = _ComputeDirectLightingMIS(
                hitPos,
                normal,
                geometricNormal,
                wo,
                bounceDomain.Fork(HdEmbreeSampleDomainKey::DirectLighting),
                doubleSided,
                false,
                &fallback,
                instanceContext->categories,
                currentMedium,
                hero.active,
                hero.wavelengthNm,
                hero.pdf);
        }
        if (hero.active) {
            addRadiance(direct * spectralThroughput);
        } else {
            addRadiance(GfCompMult(throughput, direct));
        }

        if (volumeOnlyBoundary) {
            const float wiDotNg = GfDot(rayDir, geometricNormal);
            if (currentMedium.active && currentMedium.ownerGeometry == mesh &&
                wiDotNg > 0.0f) {
                currentMedium = HdEmbreeMediumState();
            } else if (!currentMedium.active && wiDotNg < 0.0f) {
                currentMedium.active = true;
                currentMedium.medium = closure.interiorMedium;
                currentMedium.ownerGeometry = mesh;
                currentMedium.categories = &instanceContext->categories;
            }

            const float advance = rayHit.ray.tfar + 1e-4f;
            const float bias = wiDotNg > 0.0f ? 1e-4f : -1e-4f;
            rayOrigin = hitPos + geometricNormal * bias;
            if (currentRayDiff.hasDifferentials) {
                currentRayDiff.rxOrigin += rayDir * advance;
                currentRayDiff.ryOrigin += rayDir * advance;
            }

            // Medium-only boundaries are not scattering events and should not
            // consume the user's surface-bounce budget.
            --bounce;
            continue;
        }

        // --- Stop after last allowed surface bounce ---
        const bool traceEmitterOnlySample =
            bounce >= maxBounces &&
            hasBsdfClosure &&
            hasBsdfSample &&
            !bs.isSubsurface;
        if (bounce >= maxBounces && !traceEmitterOnlySample) {
            break;
        }

        // --- BSDF sampling for next direction ---
        if (!hasBsdfClosure || !hasBsdfSample || bs.isSubsurface) break;

        const GfVec3f wi = _ToGf(bs.wi);
        const float woDotNg = GfDot(wo, geometricNormal);
        const float wiDotNg = GfDot(wi, geometricNormal);
        const bool crossesBoundary =
            (woDotNg > 0.0f && wiDotNg < 0.0f) ||
            (woDotNg < 0.0f && wiDotNg > 0.0f);
        const bool sampledCausticEvent =
            hasDiffuseLikeAncestor && (bs.isSpecular || crossesBoundary);
        // Closure pruning above should keep these events out of the sampling
        // distribution when caustics are disabled. Keep this guard for cases
        // that are only visible after sampling, such as normal-map boundary
        // changes, backend-specific lobe labels, or future medium variants.
        if (sampledCausticEvent && !_enableCaustics) {
            break;
        }
        currentPathIsCaustic = currentPathIsCaustic || sampledCausticEvent;

        if (hero.active) {
            float bsdfContrib = 0.0f;
            if (bs.isSpecular) {
                bsdfContrib = mxcpp::Spectral::RgbToSpectralValue(
                    bs.f, hero.wavelengthNm);
            } else {
                const float cosTheta =
                    std::abs(GfDot(bsdfNormal, _ToGf(bs.wi)));
                bsdfContrib = mxcpp::Spectral::RgbToSpectralValue(
                    bs.f, hero.wavelengthNm) * cosTheta / bs.pdf;
            }

            if (!std::isfinite(bsdfContrib) || bsdfContrib < 0.0f) {
                bsdfContrib = 0.0f;
            }
            spectralThroughput *= bsdfContrib;
        } else {
            GfVec3f bsdfContrib;
            if (bs.isSpecular) {
                // Delta distribution (e.g. thin-surface transmission):
                // f already contains the throughput coefficient; no cosine
                // or pdf division needed.
                bsdfContrib = _ToGf(bs.f);
            } else {
                float cosTheta = std::abs(GfDot(bsdfNormal, _ToGf(bs.wi)));
                bsdfContrib = _ToGf(bs.f) * cosTheta / bs.pdf;
            }

            for (int i = 0; i < 3; ++i) {
                if (!std::isfinite(bsdfContrib[i]) || bsdfContrib[i] < 0.0f)
                    bsdfContrib[i] = 0.0f;
            }

            throughput = GfCompMult(throughput, bsdfContrib);
        }

        lastBsdfPdf = bs.isSpecular ? 0.0f : bs.pdf;
        lastScatterWasMedium = false;
        lastScatterCategories = &instanceContext->categories;
        lastLightSamplingMode =
            (!bs.isSpecular && _IsReflectionOnlyClosure(closure))
                ? HdEmbreeLightSampler::SamplingMode::ReflectionHemisphere
                : HdEmbreeLightSampler::SamplingMode::FullSphere;
        lastLightSamplingNormal = normal;
        if (!bs.isSpecular) {
            anyNonSpecularBounces = true;
        }
        if (bs.isDiffuseLike) {
            hasDiffuseLikeAncestor = true;
        }
        isFirstBounce = false;
        if (crossesBoundary) {
            // Initial implementation keeps only one medium active at a time;
            // nested dielectric stacks are deferred to a later task.
            if (currentMedium.active && currentMedium.ownerGeometry == mesh &&
                wiDotNg > 0.0f) {
                currentMedium = HdEmbreeMediumState();
            } else if (!currentMedium.active &&
                       hasClosure &&
                       closure.hasInteriorMedium &&
                       mesh &&
                       wiDotNg < 0.0f) {
                currentMedium.active = true;
                currentMedium.medium = closure.interiorMedium;
                currentMedium.ownerGeometry = mesh;
                currentMedium.categories = &instanceContext->categories;
            }
        }

        // --- Russian Roulette ---
        if (!traceEmitterOnlySample && bounce >= _minBouncesBeforeRR) {
            float q = hero.active
                ? std::max({
                    _SpectralScalarToRgb(spectralThroughput, hero)[0],
                    _SpectralScalarToRgb(spectralThroughput, hero)[1],
                    _SpectralScalarToRgb(spectralThroughput, hero)[2]})
                : std::max({throughput[0], throughput[1], throughput[2]});
            q = std::min(q, 0.95f);
            if (q <= 0.0f ||
                bounceDomain
                    .Fork(HdEmbreeSampleDomainKey::RussianRoulette)
                    .Draw1D() > q) {
                break;
            }
            if (hero.active) {
                spectralThroughput /= q;
            } else {
                throughput /= q;
            }
        }

        // --- Propagate ray differentials ---
        if (currentRayDiff.hasDifferentials && bs.isSpecular) {
            GfVec3f dndx = lastDndu * lastDudx + lastDndv * lastDvdx;
            GfVec3f dndy = lastDndu * lastDudy + lastDndv * lastDvdy;

            currentRayDiff.rxOrigin = hitPos + lastDpdx;
            currentRayDiff.ryOrigin = hitPos + lastDpdy;

            GfVec3f dwodx = -currentRayDiff.rxDirection - wo;
            GfVec3f dwody = -currentRayDiff.ryDirection - wo;

            // Check if this is a refraction (transmitted ray)
            bool isRefraction = bs.eta != 1.0f;

            if (!isRefraction) {
                // Specular reflection differential
                float dwoDotn_dx = GfDot(dwodx, normal) + GfDot(wo, dndx);
                float dwoDotn_dy = GfDot(dwody, normal) + GfDot(wo, dndy);
                currentRayDiff.rxDirection =
                    wi - dwodx + 2.0f * (GfDot(wo, normal) * dndx +
                                          dwoDotn_dx * normal);
                currentRayDiff.ryDirection =
                    wi - dwody + 2.0f * (GfDot(wo, normal) * dndy +
                                          dwoDotn_dy * normal);
            } else {
                // Specular refraction differential (Snell's law)
                float eta = bs.eta;
                if (eta != 0.0f) {
                    float dwoDotn_dx = GfDot(dwodx, normal) + GfDot(wo, dndx);
                    float dwoDotn_dy = GfDot(dwody, normal) + GfDot(wo, dndy);
                    float mu = GfDot(wo, normal) / eta
                             - std::abs(GfDot(wi, normal));
                    float wiDotN = GfDot(wi, normal);
                    float dmudx = dwoDotn_dx * (1.0f / eta +
                        1.0f / (eta * eta) * GfDot(wo, normal) /
                        (wiDotN != 0.0f ? wiDotN : 1.0f));
                    float dmudy = dwoDotn_dy * (1.0f / eta +
                        1.0f / (eta * eta) * GfDot(wo, normal) /
                        (wiDotN != 0.0f ? wiDotN : 1.0f));
                    currentRayDiff.rxDirection =
                        wi - eta * dwodx +
                        (mu * dndx + dmudx * normal);
                    currentRayDiff.ryDirection =
                        wi - eta * dwody +
                        (mu * dndy + dmudy * normal);
                } else {
                    currentRayDiff.hasDifferentials = false;
                }
            }

            // Squash check: kill differentials if they explode
            if (currentRayDiff.rxDirection.GetLengthSq() > 1e16f ||
                currentRayDiff.ryDirection.GetLengthSq() > 1e16f ||
                currentRayDiff.rxOrigin.GetLengthSq() > 1e16f ||
                currentRayDiff.ryOrigin.GetLengthSq() > 1e16f) {
                currentRayDiff.hasDifferentials = false;
            }
        } else if (!bs.isSpecular) {
            // Non-specular bounce: disable differentials
            currentRayDiff.hasDifferentials = false;
        }

        // --- Next ray ---
        float bias = (GfDot(wi, geometricNormal) > 0.0f) ? 1e-4f : -1e-4f;
        rayOrigin = hitPos + geometricNormal * bias;
        rayDir = wi;
    }

    return radiance;
}

PXR_NAMESPACE_CLOSE_SCOPE
