//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
// One-ray ambient-visibility diagnostic integration.

#include <renderer/rayUtil.h>
#include <renderer/renderer.h>
#include <renderer/rendererMath.h>

#include <cmath>

PXR_NAMESPACE_OPEN_SCOPE

static GfVec3f
_CosineWeightedDirection(GfVec2f const& uniformSample)
{
    const float angleAzimuth = 2.0f * ty::Pi * uniformSample[0];
    const float radiusDisk = std::sqrt(uniformSample[1]);
    return GfVec3f(
        std::cos(angleAzimuth) * radiusDisk,
        std::sin(angleAzimuth) * radiusDisk,
        std::sqrt(1.0f - uniformSample[1]));
}

float
ty::Renderer::_ComputeAmbientOcclusion(
    RTCRayHit const& rayHit,
    ty::SampleDomain const& domain)
{
    if (rayHit.hit.geomID == RTC_INVALID_GEOMETRY_ID ||
        rayHit.hit.instID[0] == RTC_INVALID_GEOMETRY_ID) {
        return 0.0f;
    }

    RTCGeometry const instanceGeometry =
        rtcGetGeometry(_scene, rayHit.hit.instID[0]);
    if (!instanceGeometry) {
        return 0.0f;
    }
    ty::InstanceContext const* instanceContext =
        static_cast<ty::InstanceContext const*>(
            rtcGetGeometryUserData(instanceGeometry));
    if (!instanceContext || !instanceContext->rootScene ||
        !rtcGetGeometry(instanceContext->rootScene, rayHit.hit.geomID)) {
        return 0.0f;
    }

    const GfVec3f omegaOutWld =
        -GfVec3f(rayHit.ray.dir_x, rayHit.ray.dir_y, rayHit.ray.dir_z);
    _SurfaceInteraction interaction;
    if (!_TryBuildSurfaceInteraction(
            rayHit, omegaOutWld, &interaction)) {
        return 0.0f;
    }

    // Use the renderer-resolved smooth/displaced normal, but deliberately do
    // not evaluate a material closure or graph normal for this diagnostic.
    const GfVec3f normalShdWldOut = interaction.GetNormalSrfWldOut();
    GfVec3f tangentWld;
    GfVec3f bitangentWld;
    GfBuildOrthonormalFrame(
        normalShdWldOut, &tangentWld, &bitangentWld);

    // One renderer pixel sample owns exactly one AO sample. Splitting by one
    // preserves the previous estimator's random-domain identity without an
    // inner sample loop or a second sample-count control.
    const GfVec2f uniformSample =
        domain.Split(
            ty::SampleDomainKey::AmbientOcclusionSample, 1, 0).Draw2D();
    const GfVec3f dirTangent =
        _CosineWeightedDirection(uniformSample);
    const GfVec3f dirShadowWld =
        tangentWld * dirTangent[0] +
        bitangentWld * dirTangent[1] +
        normalShdWldOut * dirTangent[2];
    const GfVec3f posRayOrgWld = ty::OffsetRayOrigin(
        interaction.posHitWld,
        interaction.normalGeomWldExt,
        normalShdWldOut,
        1e-4f);

    RTCRay shadow;
    shadow.flags = 0;
    ty::PopulateRay(&shadow, posRayOrgWld, dirShadowWld, 1e-4f);
    _ambientOcclusionRayCount.fetch_add(1, std::memory_order_relaxed);
    ty::Occluded1(_scene, &shadow);

    // With cosine-weighted hemisphere sampling, Lambertian ambient
    // visibility is the binary unoccluded fraction. Embree sets tfar to
    // negative infinity when the ray is occluded.
    return shadow.tfar > 0.0f ? 1.0f : 0.0f;
}

PXR_NAMESPACE_CLOSE_SCOPE
