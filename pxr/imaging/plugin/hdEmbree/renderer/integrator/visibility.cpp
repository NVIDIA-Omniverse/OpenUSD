//
// Copyright 2018 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
// Shadow visibility and finite-light intersection handling.

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

GfVec3f
HdEmbreeRenderer::_Visibility(
    GfVec3f const& position,
    GfVec3f const& normal,
    GfVec3f const& direction,
    float dist,
    TfToken const& shadowLink,
    HdEmbreeMediumState const& mediumState) const
{
    constexpr int kMaxTransparentHits = 16;
    constexpr int kMaxIntersections = 256;
    constexpr float kVisThreshold = 1e-4f;
    constexpr float kRayBias = 1e-4f;

    if (_disableShadows) {
        return GfVec3f(1.0f);
    }

    GfVec3f visibility(1.0f);
    HdEmbreeMediumState shadowMedium = mediumState;
    HdEmbreePrototypeContext const* straightTransparentOwner = nullptr;
    GfVec3f rayOrigin = _OffsetRayOrigin(position, normal, direction, kRayBias);
    float remaining = dist;

    const auto evalShadowTransmittance = [&](float distance) {
        const bool useAdobeVolumeTransport =
            _useAdobeOpenPBR &&
            shadowMedium.medium.transportModel ==
                mxcpp::MediumTransportModel::AdobeOpenPBR;
        return _ToGf(useAdobeVolumeTransport
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
        _PopulateRayHit(&rayHit, rayOrigin, direction, kRayBias, remaining,
                        HdEmbree_RayMask::Camera);
        rtcIntersect1(_scene, &rayHit);

        if (rayHit.hit.geomID == RTC_INVALID_GEOMETRY_ID) {
            if (shadowMedium.active && remaining > 0.0f) {
                visibility = GfCompMult(
                    visibility,
                    evalShadowTransmittance(remaining));
            }
            return visibility;
        }

        if (_GetLightGeometryHit(rayHit)) {
            return GfVec3f(0.0f);
        }

        const float hitDist = std::min(rayHit.ray.tfar, remaining);
        if (shadowMedium.active && hitDist > 0.0f) {
            visibility = GfCompMult(
                visibility,
                evalShadowTransmittance(hitDist));
        }
        if (_IsNearlyBlack(visibility, kVisThreshold)) {
            return GfVec3f(0.0f);
        }

        HdEmbreeInstanceContext const* blockerContext = nullptr;
        if (!shadowLink.IsEmpty() &&
            rayHit.hit.instID[0] != RTC_INVALID_GEOMETRY_ID) {
            RTCGeometry instanceGeometry =
                rtcGetGeometry(_scene, rayHit.hit.instID[0]);
            if (instanceGeometry) {
                blockerContext = static_cast<HdEmbreeInstanceContext const*>(
                    rtcGetGeometryUserData(instanceGeometry));
            }
        }

        if (blockerContext &&
            !HdEmbreeMatchesLink(shadowLink, blockerContext->categories)) {
            remaining -= hitDist;
            if (remaining <= 0.001f) {
                return visibility;
            }
            const GfVec3f hitPos = rayOrigin + direction * hitDist;
            rayOrigin = _OffsetRayOrigin(
                hitPos, direction, direction, kRayBias);
            continue;
        }

        if (++materialHits > kMaxTransparentHits) {
            return visibility;
        }

        mxcpp::SurfaceClosure closure;
        GfVec3f hitNg = normal;
        HdEmbreePrototypeContext const* hitMesh = nullptr;
        const bool hasClosure = _TryEvalSurfaceClosureAtHit(
            rayHit,
            -direction,
            &closure,
            nullptr,
            &hitNg,
            &hitMesh);

        const bool exitsCurrentMedium =
            shadowMedium.active && hitMesh == shadowMedium.ownerGeometry;
        const bool exitsStraightTransparent =
            straightTransparentOwner && hitMesh == straightTransparentOwner;
        const bool volumeOnlyBoundary =
            hasClosure && _IsVolumeOnlyBoundary(closure);
        GfVec3f surfaceVisibility(0.0f);
        if (volumeOnlyBoundary) {
            surfaceVisibility = GfVec3f(1.0f);
        } else if (hasClosure) {
            const bool useStraightTransmission =
                closure.thinWalled || _approxTransparentShadows;
            if (useStraightTransmission) {
                GfVec3f transmissionVisibility(0.0f);
                if (closure.thinWalled ||
                    (_approxTransparentShadows &&
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
                        closure, direction, hitNg, includeSurfaceTint);
                }
                surfaceVisibility =
                    _CombinePresenceAndTransmissionVisibility(
                        closure, transmissionVisibility);
            } else {
                float scalarVisibility = 1.0f - _Clamp01(closure.opacity);
                if (exitsCurrentMedium) {
                    scalarVisibility = std::max(
                        scalarVisibility,
                        _Clamp01(closure.transmission));
                }
                surfaceVisibility = GfVec3f(_Clamp01(scalarVisibility));
            }
        }
        visibility = GfCompMult(visibility, surfaceVisibility);

        if (_IsNearlyBlack(visibility, kVisThreshold)) {
            return GfVec3f(0.0f);
        }

        remaining -= hitDist;
        if (remaining <= 0.001f) {
            return visibility;
        }

        if (exitsCurrentMedium) {
            shadowMedium = HdEmbreeMediumState();
        } else if (exitsStraightTransparent) {
            straightTransparentOwner = nullptr;
        } else if (volumeOnlyBoundary && !shadowMedium.active && hitMesh &&
                   GfDot(direction, hitNg) < 0.0f) {
            shadowMedium.active = true;
            shadowMedium.medium = closure.interiorMedium;
            shadowMedium.ownerGeometry = hitMesh;
        } else if (_approxTransparentShadows && !shadowMedium.active &&
                   hasClosure && !closure.thinWalled &&
                   closure.transmission > 0.0f && hitMesh &&
                   GfDot(direction, hitNg) < 0.0f) {
            if (closure.hasInteriorMedium) {
                shadowMedium.active = true;
                shadowMedium.medium = closure.interiorMedium;
                shadowMedium.ownerGeometry = hitMesh;
            } else {
                straightTransparentOwner = hitMesh;
            }
        }

        GfVec3f hitPos = GfVec3f(
            rayHit.ray.org_x + hitDist * rayHit.ray.dir_x,
            rayHit.ray.org_y + hitDist * rayHit.ray.dir_y,
            rayHit.ray.org_z + hitDist * rayHit.ray.dir_z);
        rayOrigin = _OffsetRayOrigin(
            hitPos,
            hitNg,
            direction,
            kRayBias);
    }

    // Reaching the defensive intersection limit indicates malformed or
    // pathologically layered geometry. Block conservatively rather than
    // leaking light past an unexamined linked blocker.
    return GfVec3f(0.0f);
}

bool
HdEmbreeRenderer::_FindNearestFiniteLightHit(
    GfVec3f const& position,
    GfVec3f const& direction,
    float maxDist,
    HdEmbreeLightSampler::LightSample* outSample,
    TfToken* outLightLink) const
{
    if (!outSample || maxDist <= 0.0f) {
        return false;
    }

    bool found = false;
    float closestDist = maxDist;
    HdEmbreeLightSampler::LightSample closestSample{};
    TfToken closestLightLink;

    for (auto const& it : _lights.GetLights()) {
        if (!it.second || !it.second->IsFiniteLight()) {
            continue;
        }

        auto const& light = *it.second;
        if (!light.visible) {
            continue;
        }

        const HdEmbreeLightSampler::LightSample ls =
            HdEmbreeLightSampler::EvaluateLightDirection(
                light, position, direction);
        if (!ls.valid || ls.dist <= 0.0f || !std::isfinite(ls.dist)) {
            continue;
        }
        if (ls.dist >= closestDist) {
            continue;
        }

        closestDist = ls.dist;
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

HdEmbree_LightData const*
HdEmbreeRenderer::_GetLightGeometryHit(RTCRayHit const& rayHit) const
{
    if (rayHit.hit.geomID == RTC_INVALID_GEOMETRY_ID ||
        rayHit.hit.instID[0] != RTC_INVALID_GEOMETRY_ID) {
        return nullptr;
    }

    return _lights.FindGeometry(rayHit.hit.geomID);
}

bool
HdEmbreeRenderer::_EvaluateLightGeometryHit(
    RTCRayHit const& rayHit,
    GfVec3f const& position,
    GfVec3f const& direction,
    HdEmbreeLightSampler::LightSample* outSample,
    TfToken* outLightLink) const
{
    HdEmbree_LightData const* light = _GetLightGeometryHit(rayHit);
    if (!light || !outSample) {
        return false;
    }

    HdEmbreeLightSampler::LightSample sample =
        HdEmbreeLightSampler::EvaluateLightDirection(
            *light, position, direction);
    if (!sample.valid) {
        sample.Li = GfVec3f(0.0f);
        sample.wI = direction;
        sample.dist = rayHit.ray.tfar;
        sample.invPdfW = 0.0f;
        sample.valid = true;
        sample.delta = false;
    } else {
        sample.dist = rayHit.ray.tfar;
    }

    *outSample = sample;
    if (outLightLink) {
        *outLightLink = light->lightLink;
    }
    return true;
}

PXR_NAMESPACE_CLOSE_SCOPE
