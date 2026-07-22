//
// Copyright 2018 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
// Single-hit unlit, camera-light, and ambient-occlusion integration.

#include "pxr/imaging/plugin/hdEmbree/renderer/renderer.h"
#include "../rendererImpl.h"

PXR_NAMESPACE_OPEN_SCOPE

HdEmbreeRenderer::_PixelSampleResult
HdEmbreeRenderer::_IntegrateUnlit(
    GfVec3f const& origin,
    GfVec3f const& dir,
    HdEmbreeRayDifferential const& rayDiff,
    HdEmbreeSampleDomain const& domain)
{
    _PixelSampleResult result;
    RTCRayHit& rayHit = result.primaryHit;
    rayHit.ray.flags = 0;
    _PopulateRayHit(
        &rayHit, origin, dir, 0.0f,
        std::numeric_limits<float>::max(),
        HdEmbree_RayMask::Camera);
    rtcIntersect1(_scene, &rayHit);

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

    // Get the instance and prototype context structures for the hit prim.
    // We don't use embree's multi-level instancing; we
    // flatten everything in hydra. So instID[0] should always be correct.
    const HdEmbreeInstanceContext *instanceContext =
        static_cast<HdEmbreeInstanceContext*>(
                rtcGetGeometryUserData(rtcGetGeometry(_scene,
                                                      rayHit.hit.instID[0])));

    const HdEmbreePrototypeContext *prototypeContext =
        static_cast<HdEmbreePrototypeContext*>(
                rtcGetGeometryUserData(
                    rtcGetGeometry(instanceContext->rootScene,
                                   rayHit.hit.geomID)));

    // Compute the worldspace location of the rayHit hit.
    GfVec3f hitPos = _CalculateHitPosition(rayHit);

    // Prefer an authored/smoothed normal primvar; for subdivision hits without
    // one, fall back to a smooth limit-surface normal derived from dP/du,dP/dv.
    GfVec3f displacedDPdu;
    GfVec3f displacedDPdv;
    bool hasDisplacedFrame = false;
    GfVec3f normal = _ResolveObjectSpaceNormal(
        prototypeContext, instanceContext->rootScene, rayHit.hit.geomID,
        rayHit, &displacedDPdu, &displacedDPdv, &hasDisplacedFrame);

    // Transform the normal from object space to world space.
    normal = _TransformNormalToWorld(instanceContext, normal);

    // Build shading context via shared helper (texcoord, displayColor,
    // tangent frame all constructed consistently).
    mxcpp::ShadingContext ctx = _BuildShadingContext(
        rayHit, rayDiff, instanceContext, prototypeContext, hitPos, normal,
        hasDisplacedFrame ? &displacedDPdu : nullptr,
        hasDisplacedFrame ? &displacedDPdv : nullptr);
    HdEmbreePrimvarLookup cbData{
        &prototypeContext->primvarMapByString,
        rayHit.hit.primID, rayHit.hit.u, rayHit.hit.v};
    ctx.geomPropLookup = &HdEmbreeSamplePrimvar;
    ctx.geomPropUserData = &cbData;
    ctx.uniformProps = &prototypeContext->uniformPrimvarMap;

    // Recover tangent frame from context for normal map application.
    GfVec3f tangent = _ToGf(ctx.tangent);
    GfVec3f bitangent = _ToGf(ctx.bitangent);

    // Try to evaluate MaterialXCpp material if one is bound.
    mxcpp::EvalGraph *evalGraph = prototypeContext->material ? prototypeContext->material->evalGraph : nullptr;

    mxcpp::SurfaceClosure closure;
    bool hasMaterialClosure = false;

    if (evalGraph) {
        try {
            mxcpp::EvalOptions evalOptions;
            evalOptions.useAdobeOpenPBR = _useAdobeOpenPBR;
            closure = evalGraph->Evaluate(ctx, evalOptions);
            hasMaterialClosure = true;
        } catch (...) {
            hasMaterialClosure = false;
        }
    }

    if (hasMaterialClosure) {
        mxcpp::Vec3f resolvedNormal;
        if (closure.ResolveNormal(
                _ToMx(tangent),
                _ToMx(bitangent),
                _ToMx(normal),
                &resolvedNormal)) {
            normal = _ToGf(resolvedNormal);
            const GfVec3f wo = -GfVec3f(
                rayHit.ray.dir_x, rayHit.ray.dir_y, rayHit.ray.dir_z);
            if (GfDot(normal, wo) < 0.0f) normal = -normal;
        }
    }

    GfVec3f materialColor;
    if (hasMaterialClosure) {
        materialColor = _ToGf(closure.baseColor);
    } else {
        materialColor = _enableSceneColors
            ? _ToGf(ctx.displayColor) : GfVec3f(0.5f);
    }

    // The unlit integrator uses a camera-facing headlight, optionally
    // attenuated by ambient occlusion, with the resolved material normal.
    const GfVec3f rawDir(
        rayHit.ray.dir_x, rayHit.ray.dir_y, rayHit.ray.dir_z);
    float diffuseLight = fabs(GfDot(-rawDir, normal)) *
        HdEmbreeConfig::GetInstance().cameraLightIntensity;

    float aoLightIntensity =
        _ComputeAmbientOcclusion(
            hitPos,
            normal,
            domain.Fork(HdEmbreeSampleDomainKey::AmbientOcclusion));

    const GfVec3f lightingColor = materialColor * diffuseLight * aoLightIntensity;

    GfVec4f output;
    output[0] = std::max(0.0f, lightingColor[0]);
    output[1] = std::max(0.0f, lightingColor[1]);
    output[2] = std::max(0.0f, lightingColor[2]);
    output[3] = 1.0f;
    result.color = output;
    return result;
}

float
HdEmbreeRenderer::_ComputeAmbientOcclusion(GfVec3f const& position,
                                            GfVec3f const& normal,
                                            HdEmbreeSampleDomain const& domain)
{
    // 0 ambient occlusion samples means disable the ambient occlusion term.
    if (_ambientOcclusionSamples < 1) {
        return 1.0f;
    }

    float occlusionFactor = 0.0f;

    // For hemisphere sampling we need to choose a coordinate frame at this
    // point. For the purposes of _CosineWeightedDirection, the normal needs
    // to map to (0,0,1), but since the distribution is radially symmetric
    // we don't care about the other axes.
    GfMatrix3f basis(1.0f);
    GfVec3f xAxis;
    if (fabsf(GfDot(normal, GfVec3f(0.0f,0.0f,1.0f))) < 0.9f) {
        xAxis = GfCross(normal, GfVec3f(0.0f,0.0f,1.0f));
    } else {
        xAxis = GfCross(normal, GfVec3f(0.0f,1.0f,0.0f));
    }
    GfVec3f yAxis = GfCross(normal, xAxis);
    basis.SetColumn(0, xAxis.GetNormalized());
    basis.SetColumn(1, yAxis.GetNormalized());
    basis.SetColumn(2, normal);

    // Generate random samples, stratified with Latin Hypercube Sampling.
    // https://en.wikipedia.org/wiki/Latin_hypercube_sampling
    // Stratified sampling means we don't get all of our random samples
    // bunched in the far corner of the hemisphere, but instead have some
    // equal spacing guarantees.
    std::vector<GfVec2f> samples;
    samples.resize(_ambientOcclusionSamples);
    std::vector<float> yJitter;
    yJitter.resize(_ambientOcclusionSamples);
    for (int i = 0; i < _ambientOcclusionSamples; ++i) {
        const GfVec2f sample =
            domain
                .Split(
                    HdEmbreeSampleDomainKey::AmbientOcclusionSample,
                    _ambientOcclusionSamples,
                    i)
                .Draw2D();
        samples[i][0] =
            (static_cast<float>(i) + sample[0]) / _ambientOcclusionSamples;
        yJitter[i] = sample[1];
    }
    // Fisher-Yates shuffle using a separate sampler domain.
    for (int i = _ambientOcclusionSamples - 1; i > 0; --i) {
        int j = static_cast<int>(
            domain
                .Chain(HdEmbreeSampleDomainKey::AmbientOcclusionShuffle, i)
                .Draw1D() * (i + 1));
        j = std::min(j, i);
        std::swap(samples[i], samples[j]);
    }
    for (int i = 0; i < _ambientOcclusionSamples; ++i) {
        samples[i][1] =
            (static_cast<float>(i) + yJitter[i]) / _ambientOcclusionSamples;
    }

    // Trace ambient occlusion rays. The occlusion factor is the fraction of
    // the hemisphere that's occluded when rays are traced to infinity,
    // computed by random sampling over the hemisphere.
    const GfVec3f rayOrigin =
        _OffsetRayOrigin(position, normal, normal, 1e-4f);
    for (int i = 0; i < _ambientOcclusionSamples; i++)
    {
        // Sample in the hemisphere centered on the face normal. Use
        // cosine-weighted hemisphere sampling to bias towards samples which
        // will have a bigger effect on the occlusion term.
        GfVec3f shadowDir = basis * _CosineWeightedDirection(samples[i]);

        // Trace shadow ray, using the fast interface (rtcOccluded) since
        // we only care about intersection status, not intersection id.
        RTCRay shadow;
        shadow.flags = 0;
        _PopulateRay(&shadow, rayOrigin, shadowDir, 1e-4f);
        {
          rtcOccluded1(_scene, &shadow);
        }

        // Record this AO ray's contribution to the occlusion factor.
        // Since we use cosine-weighted hemisphere sampling (PDF ∝ cos θ),
        // the Monte Carlo estimator for the Lambertian ambient integral
        // reduces to a simple visibility average: 1 if visible, 0 if
        // occluded.
        // shadow is occluded when shadow.ray.tfar < 0.0f
        if (shadow.tfar > 0.0f)
            occlusionFactor += 1.0f;
    }
    // Compute the average of the occlusion samples.
    occlusionFactor /= _ambientOcclusionSamples;

    return occlusionFactor;
}

PXR_NAMESPACE_CLOSE_SCOPE
