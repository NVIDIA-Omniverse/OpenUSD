//
// Copyright 2018 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
// Shadow visibility and finite-light intersection handling.

#include "closureClassification.h"
#include "transportPolicy.h"

#include <renderer/materials/MaterialXCpp/materials/adobeOpenPbr.h>
#include <renderer/materials/MaterialXCpp/materials/bsdf.h>
#include <renderer/rayUtil.h>
#include <renderer/renderBuffer.h>
#include <renderer/renderer.h>
#include <renderer/rendererMath.h>

#include "pxr/base/work/loops.h"
#include "pxr/base/work/threadLimits.h"
#include "pxr/imaging/hd/perfLog.h"

#include <chrono>
#include <cstdio>
#include <thread>

PXR_NAMESPACE_OPEN_SCOPE

static GfVec3f
_TransparentShadowTransmission(mxcpp::SurfaceClosure const& closure,
                               GfVec3f const& dirShadowWld,
                               GfVec3f const& normalGeomWldExt,
                               bool includeSurfaceTint)
{
    const float transmission = ty::Clamp01(closure.transmission);
    if (transmission <= 0.0f) {
        return GfVec3f(0.0f);
    }

    const float interfaceTransmission =
        mxcpp::Bsdf::StraightShadowDielectricTransmission(
            closure, GfDot(dirShadowWld, normalGeomWldExt));
    GfVec3f attenuation(transmission * interfaceTransmission);
    if (includeSurfaceTint) {
        attenuation = GfCompMult(
            attenuation,
            ty::Clamp01(ty::ToGf(closure.transmissionColor)));
    }
    return ty::Clamp01(attenuation);
}

static GfVec3f
_CombinePresenceAndTransmissionVisibility(
    mxcpp::SurfaceClosure const& closure,
    GfVec3f const& transmissionVisibility)
{
    // `presence` is geometric coverage. `opacity` can be an alpha/transmission
    // control (e.g. UsdPreviewSurface transparent mode), so using it here would
    // bypass the transmissive shadow response for fully transparent glass.
    const float presence = ty::Clamp01(closure.presence);
    const GfVec3f passthroughVisibility(1.0f - presence);
    return ty::Clamp01(
        passthroughVisibility + transmissionVisibility * presence);
}

GfVec3f
ty::Renderer::_Visibility(GfVec3f const& posWld,
                          GfVec3f const& dirOffsetReferenceWld,
                          GfVec3f const& dirShadowWld,
                          float distanceWld, TfToken const& shadowLink,
                          ty::MediumState const& mediumState) const
{
    constexpr int kMaxTransparentHits = 16;
    constexpr int kMaxIntersections = 256;
    constexpr float kVisThreshold = 1e-4f;
    constexpr float kRayBias = 1e-4f;

    if (_settings.disableShadows) {
        return GfVec3f(1.0f);
    }

    GfVec3f visibility(1.0f);
    ty::MediumState shadowMedium = mediumState;
    ty::PrototypeContext const* straightTransparentOwner = nullptr;
    GfVec3f posRayOrgWld = ty::OffsetRayOrigin(
        posWld, dirOffsetReferenceWld, dirShadowWld, kRayBias);
    float distanceRemainingWld = distanceWld;

    const auto evalShadowTransmittance = [&](float distance) {
        const bool useAdobeVolumeTransport =
            _settings.useAdobeOpenPBR &&
            shadowMedium.medium.transportModel ==
                mxcpp::MediumTransportModel::AdobeOpenPBR;
        return ty::ToGf(useAdobeVolumeTransport
            ? mxcpp::AdobeOpenPbrEvalVolumeTransmittance(
                  shadowMedium.medium, distance)
            : mxcpp::EvalBeerTransmittance(shadowMedium.medium, distance));
    };

    int materialHits = 0;
    for (int intersection = 0;
         intersection < kMaxIntersections;
         ++intersection) {
        RTCRayHit rayHit;
        rayHit.ray.flags = 0;
        ty::PopulateRayHit(
            &rayHit,
            posRayOrgWld,
            dirShadowWld,
            kRayBias,
            distanceRemainingWld,
            ty::RayMask::Camera);
        rtcIntersect1(_scene, &rayHit);

        if (rayHit.hit.geomID == RTC_INVALID_GEOMETRY_ID) {
            if (shadowMedium.active && distanceRemainingWld > 0.0f) {
                visibility = GfCompMult(
                    visibility, evalShadowTransmittance(distanceRemainingWld));
            }
            return visibility;
        }

        if (_GetLightGeometryHit(rayHit)) {
            return GfVec3f(0.0f);
        }

        const float distanceHitWld =
            std::min(rayHit.ray.tfar, distanceRemainingWld);
        if (shadowMedium.active && distanceHitWld > 0.0f) {
            visibility =
                GfCompMult(visibility, evalShadowTransmittance(distanceHitWld));
        }
        if (ty::IsNearlyBlack(visibility, kVisThreshold)) {
            return GfVec3f(0.0f);
        }

        ty::InstanceContext const* blockerContext = nullptr;
        if (!shadowLink.IsEmpty() &&
            rayHit.hit.instID[0] != RTC_INVALID_GEOMETRY_ID) {
            RTCGeometry instanceGeometry =
                rtcGetGeometry(_scene, rayHit.hit.instID[0]);
            if (instanceGeometry) {
                blockerContext = static_cast<ty::InstanceContext const*>(
                    rtcGetGeometryUserData(instanceGeometry));
            }
        }

        if (blockerContext &&
            !ty::MatchesLink(shadowLink, blockerContext->categories)) {
            distanceRemainingWld -= distanceHitWld;
            if (distanceRemainingWld <= 0.001f) {
                return visibility;
            }
            const GfVec3f posHitWld =
                posRayOrgWld + dirShadowWld * distanceHitWld;
            posRayOrgWld =
                ty::OffsetRayOrigin(posHitWld, dirShadowWld,
                                    dirShadowWld, kRayBias);
            continue;
        }

        if (++materialHits > kMaxTransparentHits) {
            return visibility;
        }

        mxcpp::SurfaceClosure closure;
        // Stays zero when the blocker has no usable surface frame, because
        // _TryEvalSurfaceClosureAtHit only writes this after it builds an
        // interaction. Every consumer that needs a real normal is guarded by
        // hasClosure; the trailing ty::OffsetRayOrigin is not, and relies on
        // its zero-length case to advance along dirShadowWld instead.
        // Do not seed this from the caller's offset reference: that is the
        // shading point's normal, not this blocker's, and medium callers pass
        // a light direction.
        GfVec3f normalGeomBlockerWldExt(0.0f);
        ty::PrototypeContext const* hitMesh = nullptr;
        const bool hasClosure = _TryEvalSurfaceClosureAtHit(
            rayHit, -dirShadowWld, &closure,
            &normalGeomBlockerWldExt, &hitMesh);

        const bool exitsCurrentMedium =
            shadowMedium.active && hitMesh == shadowMedium.ownerGeometry;
        const bool exitsStraightTransparent =
            straightTransparentOwner && hitMesh == straightTransparentOwner;
        const bool volumeOnlyBoundary =
            hasClosure && ty::IsVolumeOnlyBoundary(closure);
        GfVec3f surfaceVisibility(0.0f);
        if (volumeOnlyBoundary) {
            surfaceVisibility = GfVec3f(1.0f);
        } else if (hasClosure) {
            const bool useStraightTransmission =
                closure.thinWalled || _settings.approxTransparentShadows;
            if (useStraightTransmission) {
                GfVec3f transmissionVisibility(0.0f);
                if (closure.thinWalled ||
                    (_settings.approxTransparentShadows &&
                     (exitsCurrentMedium ||
                      exitsStraightTransparent ||
                      closure.transmission > 0.0f))) {
                    // Thin-walled materials have no refractive path to bend.
                    // Thick transparent surfaces use this straight-through
                    // approximation only when the render setting enables it.
                    const bool includeSurfaceTint =
                        closure.thinWalled ||
                        (!closure.hasInteriorMedium &&
                         !exitsCurrentMedium &&
                         !exitsStraightTransparent);
                    transmissionVisibility = _TransparentShadowTransmission(
                        closure, dirShadowWld, normalGeomBlockerWldExt,
                        includeSurfaceTint);
                }
                surfaceVisibility =
                    _CombinePresenceAndTransmissionVisibility(
                        closure, transmissionVisibility);
            } else {
                float scalarVisibility = 1.0f - ty::Clamp01(closure.opacity);
                if (exitsCurrentMedium) {
                    scalarVisibility = std::max(
                        scalarVisibility,
                        ty::Clamp01(closure.transmission));
                }
                surfaceVisibility = GfVec3f(ty::Clamp01(scalarVisibility));
            }
        }
        visibility = GfCompMult(visibility, surfaceVisibility);

        if (ty::IsNearlyBlack(visibility, kVisThreshold)) {
            return GfVec3f(0.0f);
        }

        distanceRemainingWld -= distanceHitWld;
        if (distanceRemainingWld <= 0.001f) {
            return visibility;
        }

        if (exitsCurrentMedium) {
            shadowMedium = ty::MediumState();
        } else if (exitsStraightTransparent) {
            straightTransparentOwner = nullptr;
        } else if (volumeOnlyBoundary && !shadowMedium.active && hitMesh &&
                   GfDot(dirShadowWld, normalGeomBlockerWldExt) < 0.0f) {
            shadowMedium.active = true;
            shadowMedium.medium = closure.interiorMedium;
            shadowMedium.ownerGeometry = hitMesh;
        } else if (_settings.approxTransparentShadows && !shadowMedium.active &&
                   hasClosure && !closure.thinWalled &&
                   closure.transmission > 0.0f && hitMesh &&
                   GfDot(dirShadowWld, normalGeomBlockerWldExt) < 0.0f) {
            if (closure.hasInteriorMedium) {
                shadowMedium.active = true;
                shadowMedium.medium = closure.interiorMedium;
                shadowMedium.ownerGeometry = hitMesh;
            } else {
                straightTransparentOwner = hitMesh;
            }
        }

        GfVec3f posHitWld =
            GfVec3f(rayHit.ray.org_x + distanceHitWld * rayHit.ray.dir_x,
                    rayHit.ray.org_y + distanceHitWld * rayHit.ray.dir_y,
                    rayHit.ray.org_z + distanceHitWld * rayHit.ray.dir_z);
        posRayOrgWld =
            ty::OffsetRayOrigin(posHitWld, normalGeomBlockerWldExt,
                             dirShadowWld, kRayBias);
    }

    // Reaching the defensive intersection limit indicates malformed or
    // pathologically layered geometry. Block conservatively rather than
    // leaking light past an unexamined linked blocker.
    return GfVec3f(0.0f);
}

bool
ty::Renderer::_FindNearestFiniteLightHit(
    GfVec3f const& position,
    GfVec3f const& direction,
    float maxDist,
    ty::LightSampler::LightSample* outSample,
    TfToken* outLightLink) const
{
    if (!outSample || maxDist <= 0.0f) {
        return false;
    }

    bool found = false;
    float closestDist = maxDist;
    ty::LightSampler::LightSample closestSample{};
    TfToken closestLightLink;

    for (auto const& it : _lights.GetLights()) {
        if (!it.second || !it.second->IsFiniteLight()) {
            continue;
        }

        ty::LightData const& light = *it.second;
        if (!light.visible) {
            continue;
        }

        const ty::LightSampler::LightSample ls =
            ty::LightSampler::EvaluateLightDirection(
                light, position, direction, _renderColorSpace);
        if (!ls.valid || ls.distanceWld <= 0.0f ||
            !std::isfinite(ls.distanceWld)) {
            continue;
        }
        if (ls.distanceWld >= closestDist) {
            continue;
        }

        closestDist = ls.distanceWld;
        closestSample = ls;
        closestLightLink = light.lightLink;
        found = true;
    }

    if (found) {
        *outSample = closestSample;
        if (outLightLink) {
            *outLightLink = closestLightLink;
        }
    }
    return found;
}

ty::LightData const*
ty::Renderer::_GetLightGeometryHit(RTCRayHit const& rayHit) const
{
    if (rayHit.hit.geomID == RTC_INVALID_GEOMETRY_ID ||
        rayHit.hit.instID[0] != RTC_INVALID_GEOMETRY_ID) {
        return nullptr;
    }

    return _lights.FindGeometry(rayHit.hit.geomID);
}

bool
ty::Renderer::_EvaluateLightGeometryHit(
    RTCRayHit const& rayHit,
    GfVec3f const& position,
    GfVec3f const& direction,
    ty::LightSampler::LightSample* outSample,
    TfToken* outLightLink) const
{
    ty::LightData const* light = _GetLightGeometryHit(rayHit);
    if (!light || !outSample) {
        return false;
    }

    ty::LightSampler::LightSample sample =
        ty::LightSampler::EvaluateLightDirection(
            *light, position, direction, _renderColorSpace);
    if (!sample.valid) {
        sample.radianceIn = GfVec3f(0.0f);
        sample.omegaInWld = direction;
        sample.distanceWld = rayHit.ray.tfar;
        sample.pdfSolidAngleInverse = 0.0f;
        sample.valid = true;
        sample.delta = false;
    } else {
        sample.distanceWld = rayHit.ray.tfar;
    }

    *outSample = sample;
    if (outLightLink) {
        *outLightLink = light->lightLink;
    }
    return true;
}

PXR_NAMESPACE_CLOSE_SCOPE
