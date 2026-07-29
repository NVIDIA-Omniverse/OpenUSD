//
// Copyright 2018 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
// Path-segment transport through participating media.

#include "transportPolicy.h"

#include <renderer/heroWavelength.h>
#include <renderer/materials/MaterialXCpp/materials/adobeOpenPbr.h>
#include <renderer/materials/MaterialXCpp/materials/bsdf.h>
#include <renderer/rayUtil.h>
#include <renderer/renderer.h>
#include <renderer/rendererMath.h>

PXR_NAMESPACE_OPEN_SCOPE

static constexpr float _volumePdfEps = 1.0e-20f;

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
        return ty::IsNearlyBlack(
            _GetPathThroughputRgb(*state), ty::MinLuminanceCutoff);
    };

    const auto addFiniteLightHit = [&]() {
        if (!input.finiteLightLink.IsEmpty() &&
            (!state->lastScatterCategories ||
             !HdEmbreeMatchesLink(
                 input.finiteLightLink, *state->lastScatterCategories))) {
            return _VolumeTransmissionResult::Terminate;
        }
        GfVec3f radianceLight = input.finiteLightHit.radianceIn;
        if (state->lastBsdfPdf > 0.0f &&
            input.finiteLightHit.pdfSolidAngleInverse > 0.0f) {
            const float lightPdf =
                1.0f / input.finiteLightHit.pdfSolidAngleInverse;
            const float effectiveLightPdf = ty::GetMultiSampleMisLightPdf(
                lightPdf,
                _settings.lightSamplesPerHit);
            if (effectiveLightPdf > 0.0f) {
                radianceLight *= mxcpp::Bsdf::PowerHeuristic(state->lastBsdfPdf,
                                                             effectiveLightPdf);
            }
        }

        _AddPathRadiance(_WeightPathRadiance(radianceLight, *state), state);

        return _VolumeTransmissionResult::Terminate;
    };

    const mxcpp::MediumProperties& medium = mediumState.medium;
    const bool useAdobeVolumeTransport =
        _settings.useAdobeOpenPBR &&
        medium.transportModel == mxcpp::MediumTransportModel::AdobeOpenPBR;

    if (!medium.IsAbsorbingOnly()) {
        GfVec3f extinction(0.0f);
        GfVec3f scattering(0.0f);
        GfVec3f channelPdf(0.0f);
        int channel = 0;

        if (!useAdobeVolumeTransport) {
            extinction = ty::ToGf(medium.Extinction());
            scattering = ty::ToGf(medium.scattering);
            GfVec3f albedo(0.0f);
            for (int i = 0; i < 3; ++i) {
                if (extinction[i] > 1.0e-6f) {
                    albedo[i] =
                        std::clamp(scattering[i] / extinction[i], 0.0f, 1.0f);
                }
            }

            mxcpp::Vec3f channelPdfMx;
            channel = mxcpp::ChannelMIS(
                ty::ToMx(_GetPathThroughputRgb(*state)),
                ty::ToMx(albedo),
                domain.Fork(HdEmbreeSampleDomainKey::MediumChannel).Draw1D(),
                &channelPdfMx);
            channelPdf =
                GfVec3f(channelPdfMx[0], channelPdfMx[1], channelPdfMx[2]);
        }

        // Chiang channel MIS: sample a single RGB tracking channel, but
        // evaluate all RGB channels against the mixture pdf.
        const auto evalTransmittance = [&](float distance) {
            return ty::ToGf(useAdobeVolumeTransport
                ? mxcpp::AdobeOpenPbrEvalVolumeTransmittance(
                      medium, distance)
                : mxcpp::EvalBeerTransmittance(medium, distance));
        };

        const auto evalScatterWeight = [&](float distance) {
            if (useAdobeVolumeTransport) {
                return ty::ToGf(
                    mxcpp::AdobeOpenPbrCalculateVolumeEventWeight(
                        medium,
                        ty::ToMx(_GetPathThroughputRgb(*state)),
                        distance));
            }
            const GfVec3f transmittance = evalTransmittance(distance);
            const GfVec3f pdf = GfCompMult(extinction, transmittance);
            const GfVec3f sampleContrib = GfCompMult(scattering, transmittance);
            const float denom = GfDot(channelPdf, pdf);
            if (!std::isfinite(denom) || denom <= _volumePdfEps) {
                return GfVec3f(0.0f);
            }
            return sampleContrib * (1.0f / denom);
        };

        const auto evalTransmittanceWeight = [&](float distance) {
            if (useAdobeVolumeTransport) {
                return ty::ToGf(
                    mxcpp::AdobeOpenPbrCalculateVolumeSurfaceWeight(
                        medium,
                        ty::ToMx(_GetPathThroughputRgb(*state)),
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
                  ty::ToMx(_GetPathThroughputRgb(*state)),
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

            const GfVec3f scatterPos = state->positionRayOriginWld +
                                       state->directionRayWld * scatterDist;
            const GfVec3f omegaOutWld = -state->directionRayWld;
            const GfVec3f direct = _ComputeMediumDirectLighting(
                scatterPos, omegaOutWld, mediumState,
                domain.Fork(HdEmbreeSampleDomainKey::MediumDirectLighting),
                input.bounce < _settings.maxBounces,
                hero.active,
                hero.wavelengthNm,
                hero.pdf);
            _AddPathRadiance(state->hero.active
                                 ? direct * state->throughputSpectral
                                 : GfCompMult(state->throughputRgb, direct),
                             state);

            if (input.bounce >= _settings.maxBounces) {
                return _VolumeTransmissionResult::Terminate;
            }

            if (input.bounce >= _settings.minBouncesBeforeRR) {
                float q =
                    hero.active
                        ? std::max({
                            ty::SpectralScalarToRgb(
                                state->throughputSpectral,
                                hero,
                                _renderColorSpace)[0],
                            ty::SpectralScalarToRgb(
                                state->throughputSpectral,
                                hero,
                                _renderColorSpace)[1],
                            ty::SpectralScalarToRgb(
                                state->throughputSpectral,
                                hero,
                                _renderColorSpace)[2]})
                        : std::max({state->throughputRgb[0],
                                    state->throughputRgb[1],
                                    state->throughputRgb[2]});
                q = std::min(q, 0.95f);
                if (q <= 0.0f ||
                    domain
                        .Fork(HdEmbreeSampleDomainKey::MediumRussianRoulette)
                        .Draw1D() > q) {
                    return _VolumeTransmissionResult::Terminate;
                }
                if (hero.active) {
                    state->throughputSpectral /= q;
                } else {
                    state->throughputRgb /= q;
                }
            }

            const GfVec2f phaseSample =
                domain.Fork(HdEmbreeSampleDomainKey::MediumPhase).Draw2D();
            const GfVec3f omegaInWld =
                ty::ToGf(useAdobeVolumeTransport
                          ? mxcpp::AdobeOpenPbrSampleVolumePhase(
                                medium, ty::ToMx(omegaOutWld), phaseSample[0],
                                phaseSample[1])
                          : mxcpp::SampleHenyeyGreenstein(
                                ty::ToMx(omegaOutWld), medium.anisotropy,
                                phaseSample[0], phaseSample[1]));
            const float phasePdf =
                useAdobeVolumeTransport
                    ? mxcpp::AdobeOpenPbrEvalVolumePhasePdf(
                          medium, ty::ToMx(omegaInWld), ty::ToMx(omegaOutWld))
                    : mxcpp::PdfHenyeyGreenstein(ty::ToMx(omegaInWld),
                                                 ty::ToMx(omegaOutWld),
                                                 medium.anisotropy);
            if (phasePdf <= 0.0f || !std::isfinite(phasePdf)) {
                return _VolumeTransmissionResult::Terminate;
            }

            state->positionRayOriginWld =
                ty::OffsetRayOrigin(scatterPos, omegaInWld, omegaInWld, 1e-4f);
            state->directionRayWld = omegaInWld;
            state->rayDifferential.hasDifferentials = false;
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
        const GfVec3f transmittance = ty::ToGf(useAdobeVolumeTransport
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
        const GfVec3f transmittance = ty::ToGf(useAdobeVolumeTransport
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
