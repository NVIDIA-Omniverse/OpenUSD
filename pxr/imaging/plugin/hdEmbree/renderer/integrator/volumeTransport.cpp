//
// Copyright 2018 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
// Path-segment transport through participating media.

#include "pxr/imaging/plugin/hdEmbree/renderer/renderer.h"
#include "../rendererImpl.h"

PXR_NAMESPACE_OPEN_SCOPE

void
HdEmbreeRenderer::_UpdatePathMedium(
    mxcpp::SurfaceClosure const& closure,
    HdEmbreePrototypeContext const* geometry,
    HdEmbreeCategorySet const& categories,
    float directionDotNormal,
    _PathState* state) const
{
    if (!state || !geometry) {
        return;
    }
    if (state->medium.active &&
        state->medium.ownerGeometry == geometry &&
        directionDotNormal > 0.0f) {
        state->medium = HdEmbreeMediumState();
    } else if (!state->medium.active &&
               closure.hasInteriorMedium &&
               directionDotNormal < 0.0f) {
        state->medium.active = true;
        state->medium.medium = closure.interiorMedium;
        state->medium.ownerGeometry = geometry;
        state->medium.categories = &categories;
    }
}

HdEmbreeRenderer::_VolumeTransmissionResult
HdEmbreeRenderer::_TraceVolumeTransmission(
    _VolumeTransmissionInput const& input,
    HdEmbreeSampleDomain const& domain,
    _PathState* state) const
{
    if (!state || !state->medium.active) {
        return _VolumeTransmissionResult::ContinueSurface;
    }

    const HdEmbreeMediumState& mediumState = state->medium;
    const _HeroWavelengthState hero{
        state->hero.active,
        state->hero.wavelengthNm,
        state->hero.pdf};

    const auto throughputIsBlack = [&]() {
        return _IsNearlyBlack(
            _GetPathThroughputRgb(*state), _minLuminanceCutoff);
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

        _AddPathRadiance(
            _WeightPathRadiance(lightContrib, *state), state);

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
                _ToMx(_GetPathThroughputRgb(*state)),
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
                        _ToMx(_GetPathThroughputRgb(*state)),
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
                        _ToMx(_GetPathThroughputRgb(*state)),
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
                  _ToMx(_GetPathThroughputRgb(*state)),
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
            _ApplyPathWeight(scatterWeight, state);

            if (throughputIsBlack()) {
                return _VolumeTransmissionResult::Terminate;
            }

            const GfVec3f scatterPos =
                state->rayOrigin + state->rayDir * scatterDist;
            const GfVec3f wo = -state->rayDir;
            const GfVec3f direct = _ComputeMediumDirectLighting(
                scatterPos,
                wo,
                mediumState,
                domain.Fork(HdEmbreeSampleDomainKey::MediumDirectLighting),
                input.bounce < _maxBounces,
                hero.active,
                hero.wavelengthNm,
                hero.pdf);
            _AddPathRadiance(
                state->hero.active
                    ? direct * state->spectralThroughput
                    : GfCompMult(state->throughput, direct),
                state);

            if (input.bounce >= _maxBounces) {
                return _VolumeTransmissionResult::Terminate;
            }

            if (input.bounce >= _minBouncesBeforeRR) {
                float q = hero.active
                    ? std::max({
                        _SpectralScalarToRgb(
                            state->spectralThroughput,
                            hero,
                            _renderColorSpace)[0],
                        _SpectralScalarToRgb(
                            state->spectralThroughput,
                            hero,
                            _renderColorSpace)[1],
                        _SpectralScalarToRgb(
                            state->spectralThroughput,
                            hero,
                            _renderColorSpace)[2]})
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
            state->hasDiffuseLikeAncestor = true;
            state->isFirstBounce = false;
            return _VolumeTransmissionResult::ContinueRay;
        }

        if (input.hasFiniteLightHit &&
            input.finiteLightDist < input.surfaceDist &&
            input.finiteLightDist > 0.0f) {
            _ApplyPathWeight(
                evalTransmittanceWeight(input.finiteLightDist), state);

            if (throughputIsBlack()) {
                return _VolumeTransmissionResult::Terminate;
            }

            return addFiniteLightHit();
        }

        if (std::isfinite(input.surfaceDist) && input.surfaceDist > 0.0f) {
            _ApplyPathWeight(
                evalTransmittanceWeight(input.surfaceDist), state);
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
        _ApplyPathWeight(transmittance, state);

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
        _ApplyPathWeight(transmittance, state);
    } else {
        return _VolumeTransmissionResult::Terminate;
    }

    if (throughputIsBlack()) {
        return _VolumeTransmissionResult::Terminate;
    }

    return _VolumeTransmissionResult::ContinueSurface;
}

PXR_NAMESPACE_CLOSE_SCOPE
