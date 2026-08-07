//
// Copyright 2018 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
// Single-hit unlit and camera-light integration.

#include <renderer/geometry/primvarSampling.h>
#include <renderer/integrator/shadingNormal.h>
#include <renderer/materials/MaterialXCpp/graph.h>
#include <renderer/materials/MaterialXCpp/shadingContext.h>
#include <renderer/rayUtil.h>
#include <renderer/renderer.h>
#include <renderer/rendererMath.h>

PXR_NAMESPACE_OPEN_SCOPE

ty::Renderer::_PixelSampleResult
ty::Renderer::_IntegrateUnlit(
    GfVec3f const& posRayOrgWld,
    GfVec3f const& dirRayWld,
    ty::RayDifferential const& diffRay)
{
    _PixelSampleResult result;
    RTCRayHit& rayHit = result.primaryHit;
    rayHit.ray.flags = 0;
    ty::PopulateRayHit(
        &rayHit, posRayOrgWld, dirRayWld, 0.0f,
        std::numeric_limits<float>::max(),
        ty::RayMask::Camera);
    rtcIntersect1(_scene, &rayHit);

    if (_IsEdgeOnlyWireframeHit(rayHit)) {
        // Skip the camera headlight and material evaluation. _ApplyWireframe()
        // will draw solid black coverage.
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

    // Construct the same outward topology and incident material frame used
    // by the path and visibility integrators.
    const GfVec3f omegaOutWld =
        -GfVec3f(rayHit.ray.dir_x, rayHit.ray.dir_y, rayHit.ray.dir_z);
    _SurfaceInteraction interaction;
    ty::InstanceContext const* instanceContext = nullptr;
    ty::PrototypeContext const* prototypeContext = nullptr;
    if (!_TryBuildSurfaceInteraction(rayHit, omegaOutWld, &interaction,
                                     &instanceContext, &prototypeContext)) {
        result.color = GfVec4f(0.0f, 0.0f, 0.0f, 1.0f);
        return result;
    }
    GfVec3f normalShdWldExt = interaction.normalSrfWldExt;
    mxcpp::EvalGraph* surfaceGraph = prototypeContext->material
        ? prototypeContext->material->surfaceGraph
        : nullptr;
    // Build shading context via shared helper (texcoord, displayColor,
    // tangent frame all constructed consistently).
    const _ShadingContextOptions contextOptions(
        true, surfaceGraph && surfaceGraph->RequiresObjectSpacePosition());
    mxcpp::ShadingContext ctx = _BuildShadingContext(
        rayHit, diffRay, instanceContext, prototypeContext, interaction,
        nullptr, nullptr, contextOptions);
    ty::PrimvarLookup cbData{
        &prototypeContext->geomPropSamplers,
        rayHit.hit.primID, rayHit.hit.u, rayHit.hit.v};
    ctx.geomPropLookup = &ty::SamplePrimvar;
    ctx.geomPropUserData = &cbData;
    ctx.uniformProps = &prototypeContext->geomPropUniformValues;

    // Try to evaluate MaterialXCpp material if one is bound.
    mxcpp::SurfaceClosure closure;
    bool hasMaterialClosure = false;

    if (surfaceGraph) {
        mxcpp::EvalOptions evalOptions;
        evalOptions.useAdobeOpenPBR = _settings.useAdobeOpenPBR;
        closure = surfaceGraph->Evaluate(ctx, evalOptions);
        hasMaterialClosure = true;
    }

    if (hasMaterialClosure) {
        normalShdWldExt = ty::ToGf(
            mxcpp::ResolveGraphNormal(closure, ctx));
    }
    const GfVec3f normalShdWldOut = ty::FaceNormalShdWldOut(
        normalShdWldExt, interaction.frontFacing);

    // Hydra's unlit presentation color is the geometry display color, not a
    // material-closure summary. Material evaluation above currently remains
    // responsible only for the shading normal used by the camera headlight.
    const GfVec3f displayColor = ty::ToGf(ctx.displayColor);

    // The unlit integrator uses a camera-facing headlight with the resolved
    // material normal. Ambient occlusion is an independent AOV diagnostic.
    const GfVec3f dirCameraRayWld(
        rayHit.ray.dir_x, rayHit.ray.dir_y, rayHit.ray.dir_z);
    float diffuseLight = fabs(GfDot(-dirCameraRayWld, normalShdWldOut)) *
                         ty::CameraLightIntensity;

    const GfVec3f lightingColor =
        displayColor * diffuseLight;

    GfVec4f output;
    output[0] = std::max(0.0f, lightingColor[0]);
    output[1] = std::max(0.0f, lightingColor[1]);
    output[2] = std::max(0.0f, lightingColor[2]);
    output[3] = 1.0f;
    result.color = output;
    return result;
}

PXR_NAMESPACE_CLOSE_SCOPE
