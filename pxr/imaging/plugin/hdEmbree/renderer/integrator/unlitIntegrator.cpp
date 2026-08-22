//
// Copyright 2018 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
// Single-hit Hydra display-color integration.

#include <renderer/rayUtil.h>
#include <renderer/renderer.h>

#include "pxr/imaging/hd/tokens.h"

PXR_NAMESPACE_OPEN_SCOPE

ty::Renderer::_PixelSampleResult
ty::Renderer::_IntegrateUnlit(
    GfVec3f const& posRayOrgWld,
    GfVec3f const& dirRayWld)
{
    _PixelSampleResult result;
    RTCRayHit& rayHit = result.primaryHit;
    rayHit.ray.flags = 0;
    ty::PopulateRayHit(
        &rayHit, posRayOrgWld, dirRayWld, 0.0f,
        std::numeric_limits<float>::max(),
        ty::RayMask::Camera);
    ty::Intersect1(_scene, &rayHit);

    if (_IsEdgeOnlyWireframeHit(rayHit)) {
        // Skip display-color sampling. _ApplyWireframe() will draw solid
        // black coverage.
        return result;
    }

    if (rayHit.hit.geomID == RTC_INVALID_GEOMETRY_ID) {
        result.color = GfVec4f(
            _colorClearValue[0],
            _colorClearValue[1],
            _colorClearValue[2],
            1.0f);
        return result;
    }

    // Finite lights are black in the explicitly unlit integrator.
    if (_GetLightGeometryHit(rayHit)) {
        result.color = GfVec4f(0.0f, 0.0f, 0.0f, 1.0f);
        return result;
    }

    // Resolve only the immutable hit context needed to sample displayColor.
    // Hydra's unlit presentation does not evaluate material closures, normals,
    // lights, derivatives, or any other shading-context input.
    if (rayHit.hit.instID[0] == RTC_INVALID_GEOMETRY_ID) {
        result.color = GfVec4f(0.0f, 0.0f, 0.0f, 1.0f);
        return result;
    }
    RTCGeometry const instanceGeometry =
        rtcGetGeometry(_scene, rayHit.hit.instID[0]);
    ty::InstanceContext const* const instanceContext = instanceGeometry
        ? static_cast<ty::InstanceContext const*>(
            rtcGetGeometryUserData(instanceGeometry))
        : nullptr;
    RTCGeometry const prototypeGeometry =
        instanceContext && instanceContext->rootScene
            ? rtcGetGeometry(instanceContext->rootScene, rayHit.hit.geomID)
            : nullptr;
    ty::PrototypeContext const* const prototypeContext = prototypeGeometry
        ? static_cast<ty::PrototypeContext const*>(
            rtcGetGeometryUserData(prototypeGeometry))
        : nullptr;
    if (!prototypeContext) {
        result.color = GfVec4f(0.0f, 0.0f, 0.0f, 1.0f);
        return result;
    }

    GfVec3f displayColor(0.5f);
    const auto displayColorIt =
        prototypeContext->primvarMap.find(HdTokens->displayColor);
    if (displayColorIt != prototypeContext->primvarMap.end()) {
        displayColorIt->second->Sample(
            rayHit.hit.primID, rayHit.hit.u, rayHit.hit.v,
            &displayColor);
    }

    result.color = GfVec4f(
        displayColor[0], displayColor[1], displayColor[2], 1.0f);
    return result;
}

PXR_NAMESPACE_CLOSE_SCOPE
