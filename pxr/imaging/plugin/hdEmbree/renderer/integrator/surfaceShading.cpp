//
// Copyright 2018 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
// Hit interpretation, shading contexts, and camera color.

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

mxcpp::ShadingContext
HdEmbreeRenderer::_BuildShadingContext(
    RTCRayHit const& rayHit,
    HdEmbreeRayDifferential const& rayDiff,
    HdEmbreeInstanceContext const* instanceContext,
    HdEmbreePrototypeContext const* prototypeContext,
    GfVec3f const& hitPos,
    GfVec3f const& normal,
    GfVec3f* outDndu,
    GfVec3f* outDndv,
    _ShadingContextOptions options) const
{
    // Texcoord (try GfVec2f first, then GfVec3f)
    GfVec2f texcoordVal(0.0f);
    {
        auto it = prototypeContext->primvarMap.find(_tokensSt);
        if (it != prototypeContext->primvarMap.end()) {
            if (!it->second->Sample(
                    rayHit.hit.primID, rayHit.hit.u, rayHit.hit.v,
                    &texcoordVal)) {
                GfVec3f tc3;
                if (it->second->Sample(
                        rayHit.hit.primID, rayHit.hit.u, rayHit.hit.v,
                        &tc3)) {
                    texcoordVal = GfVec2f(tc3[0], tc3[1]);
                }
            }
        }
    }

    // Display color — always sample the authored primvar so that material
    // evaluation (including opacity) sees the correct value regardless of
    // the _enableSceneColors display-only flag.
    GfVec3f displayColor(0.8f);
    {
        auto it = prototypeContext->primvarMap.find(HdTokens->displayColor);
        if (it != prototypeContext->primvarMap.end()) {
            it->second->Sample(
                rayHit.hit.primID, rayHit.hit.u, rayHit.hit.v,
                &displayColor);
        }
    }

    float displayOpacity = 1.0f;
    {
        auto it = prototypeContext->primvarMap.find(HdTokens->displayOpacity);
        if (it != prototypeContext->primvarMap.end()) {
            it->second->Sample(
                rayHit.hit.primID, rayHit.hit.u, rayHit.hit.v,
                &displayOpacity);
        }
    }

    // Surface derivatives (dPdu, dPdv) and normal derivatives (dndu, dndv)
    // start in object space and are transformed to world space below.
    GfVec3f dPdu, dPdv, dndu, dndv;
    if (_IsSubdivMesh(prototypeContext)) {
        _ComputeSubdivSurfaceDerivatives(
            prototypeContext,
            instanceContext->rootScene, rayHit.hit.geomID,
            rayHit.hit.primID, rayHit.hit.u, rayHit.hit.v,
            normal, &dPdu, &dPdv, &dndu, &dndv);
    } else {
        _ComputeTriangleSurfaceDerivatives(
            prototypeContext,
            rayHit.hit.primID, normal,
            &dPdu, &dPdv, &dndu, &dndv);
    }

    const GfVec3f objectHitPos =
        instanceContext->worldToObjectMatrix.Transform(hitPos);
    const GfVec3f objectDPdu = dPdu;
    const GfVec3f objectDPdv = dPdv;

    // Object space -> world space
    dPdu = instanceContext->objectToWorldMatrix.TransformDir(dPdu);
    dPdv = instanceContext->objectToWorldMatrix.TransformDir(dPdv);
    dndu = instanceContext->objectToWorldMatrix.TransformDir(dndu);
    dndv = instanceContext->objectToWorldMatrix.TransformDir(dndv);

    if (outDndu) *outDndu = dndu;
    if (outDndv) *outDndv = dndv;

    GfVec3f tangent(1.0f, 0.0f, 0.0f);
    GfVec3f bitangent(0.0f, 1.0f, 0.0f);
    bool haveTangentFrame = false;
    {
        const auto sampleFrame = [&](TfToken const& tangentToken,
                                     TfToken const& bitangentToken) {
            auto tangentIt = prototypeContext->primvarMap.find(tangentToken);
            auto bitangentIt = prototypeContext->primvarMap.find(bitangentToken);
            if (tangentIt == prototypeContext->primvarMap.end() ||
                bitangentIt == prototypeContext->primvarMap.end()) {
                return false;
            }
            if (!tangentIt->second->Sample(
                    rayHit.hit.primID, rayHit.hit.u, rayHit.hit.v, &tangent) ||
                !bitangentIt->second->Sample(
                    rayHit.hit.primID, rayHit.hit.u, rayHit.hit.v,
                    &bitangent)) {
                return false;
            }
            tangent = instanceContext->objectToWorldMatrix.TransformDir(tangent);
            bitangent =
                instanceContext->objectToWorldMatrix.TransformDir(bitangent);
            return true;
        };

        haveTangentFrame =
            sampleFrame(_tokensTangent, _tokensBitangent) ||
            sampleFrame(_tokensComputedTangent, _tokensComputedBitangent);
    }

    if (haveTangentFrame) {
        tangent -= normal * GfDot(normal, tangent);
        if (tangent.GetLengthSq() > 1e-18f) {
            tangent.Normalize();
        } else {
            haveTangentFrame = false;
        }

        bitangent -= normal * GfDot(normal, bitangent);
        bitangent -= tangent * GfDot(tangent, bitangent);
        if (haveTangentFrame && bitangent.GetLengthSq() > 1e-18f) {
            bitangent.Normalize();
        } else {
            haveTangentFrame = false;
        }
    }

    if (!haveTangentFrame) {
        bitangent = GfCross(normal, dPdu);
        if (bitangent.GetLengthSq() < 1e-18f) {
            GfBuildOrthonormalFrame(normal, &tangent, &bitangent);
        } else {
            bitangent.Normalize();
            tangent = GfCross(bitangent, normal).GetNormalized();
        }
    }

    mxcpp::ShadingContext ctx;
    ctx.position = _ToMx(objectHitPos);
    ctx.normal = _ToMx(normal);
    ctx.tangent = _ToMx(tangent);
    ctx.bitangent = _ToMx(bitangent);
    ctx.viewPosition = _ToMx(GfVec3f(_inverseViewMatrix.Transform(GfVec3f(0.0f))));
    // Graph-facing texture coordinates preserve authored USD st values, which
    // match MaterialX's lower-left UV convention.  Texture backends convert
    // from that convention to their native image-space convention at lookup
    // time.
    ctx.texcoord = mxcpp::Vec2f(texcoordVal[0], texcoordVal[1]);
    ctx.displayColor = _ToMx(displayColor);
    ctx.displayOpacity = displayOpacity;
    ctx.textureSystem = _textureSystem.get();
    ctx.frame = _sceneFrame;
    ctx.time = _sceneTime;
    ctx.faceId = rayHit.hit.primID;
    ctx.baryU = rayHit.hit.u;
    ctx.baryV = rayHit.hit.v;
    ctx.dPdu = _ToMx(dPdu);
    ctx.dPdv = _ToMx(dPdv);
    ctx.dPositiondu = _ToMx(objectDPdu);
    ctx.dPositiondv = _ToMx(objectDPdv);
    ctx.objectToWorldMatrix = _ToMx(instanceContext->objectToWorldMatrix);
    ctx.worldToObjectMatrix = _ToMx(instanceContext->worldToObjectMatrix);
    ctx.hasObjectToWorldTransform = true;
    ctx.hasWorldToObjectTransform = true;

    if (options.computeScreenSpaceDerivatives) {
        _ComputeScreenSpaceDerivatives(
            rayDiff, hitPos, normal, dPdu, dPdv,
            _viewMatrix, _inverseProjMatrix,
            static_cast<float>(_dataWindow.GetWidth()),
            static_cast<float>(_dataWindow.GetHeight()),
            _samplesToConvergence,
            ctx);

    }

    ctx.dPositiondx = _ToMx(
        instanceContext->worldToObjectMatrix.TransformDir(_ToGf(ctx.dPdx)));
    ctx.dPositiondy = _ToMx(
        instanceContext->worldToObjectMatrix.TransformDir(_ToGf(ctx.dPdy)));

    return ctx;
}

bool
HdEmbreeRenderer::_TryEvalSurfaceClosureAtHit(
    RTCRayHit const& rayHit,
    mxcpp::SurfaceClosure* outClosure,
    GfVec3f* outGeometricNormal,
    HdEmbreePrototypeContext const** outGeometry) const
{
    if (rayHit.hit.geomID == RTC_INVALID_GEOMETRY_ID ||
        _GetLightGeometryHit(rayHit)) {
        return false;
    }

    const HdEmbreeInstanceContext *instanceContext =
        static_cast<HdEmbreeInstanceContext*>(
            rtcGetGeometryUserData(
                rtcGetGeometry(_scene, rayHit.hit.instID[0])));
    const HdEmbreePrototypeContext *prototypeContext =
        static_cast<HdEmbreePrototypeContext*>(
            rtcGetGeometryUserData(
                rtcGetGeometry(instanceContext->rootScene,
                               rayHit.hit.geomID)));

    mxcpp::EvalGraph *evalGraph = prototypeContext->material ? prototypeContext->material->evalGraph : nullptr;
    if (outGeometry) {
        *outGeometry = prototypeContext;
    }
    if (!evalGraph || !outClosure) return false;

    GfVec3f hitPos = _CalculateHitPosition(rayHit);
    GfVec3f normal = _ResolveObjectSpaceNormal(
        prototypeContext, instanceContext->rootScene, rayHit.hit.geomID,
        rayHit);
    normal = instanceContext->objectToWorldMatrix.TransformDir(normal);
    normal.Normalize();
    if (outGeometricNormal) {
        *outGeometricNormal = normal;
    }

    try {
        HdEmbreeRayDifferential defaultRayDiff;
        const _ShadingContextOptions options(false);
        mxcpp::ShadingContext ctx = _BuildShadingContext(
            rayHit, defaultRayDiff,
            instanceContext, prototypeContext, hitPos, normal,
            nullptr, nullptr, options);
        _GeomPropCallbackData cbData{
            &prototypeContext->primvarMapByString,
            rayHit.hit.primID, rayHit.hit.u, rayHit.hit.v};
        ctx.geomPropLookup = &_SampleGeomProp;
        ctx.geomPropUserData = &cbData;
        ctx.uniformProps = &prototypeContext->uniformPrimvarMap;
        mxcpp::EvalOptions evalOptions;
        evalOptions.useAdobeOpenPBR = _useAdobeOpenPBR;
        evalOptions.visibilityOnly = true;
        *outClosure = evalGraph->Evaluate(ctx, evalOptions);
        return true;
    } catch (...) {
        return false;
    }
}

GfVec4f
HdEmbreeRenderer::_ComputeColor(RTCRayHit const& rayHit,
                                HdEmbreeRayDifferential const& rayDiff,
                                HdEmbreeSampler const& sampler,
                                GfVec4f const& clearColor)
{
    if (rayHit.hit.geomID == RTC_INVALID_GEOMETRY_ID) {
        if (_lights.GetDomes().empty() || !_enableLighting || !_domeLightCameraVisibility) {
            return GfVec4f(
                clearColor[0],
                clearColor[1],
                clearColor[2],
                1.0f);
        }

        // if we missed all geometry in the scene, evaluate the infinite lights
        // directly
        GfVec4f domeColor(0.0f, 0.0f, 0.0f, 1.0f);
        for (auto* dome : _lights.GetDomes()) {
            if (!dome->visible) {
                continue;
            }
            // Direct visibility: sample the dome lights. Since we know
            // we're only evaluating domes along the camera ray direction.
            HdEmbreeLightSampler::LightSample ls =
                HdEmbreeLightSampler::EvaluateDomeLightDirection(
                    *dome,
                    GfVec3f(
                        rayHit.ray.dir_x,
                        rayHit.ray.dir_y,
                        rayHit.ray.dir_z));
            domeColor += GfVec4f(ls.Li[0], ls.Li[1], ls.Li[2], 0);
        }
        return domeColor;
    }

    const GfVec3f origin(
        rayHit.ray.org_x, rayHit.ray.org_y, rayHit.ray.org_z);
    const GfVec3f dir = GfVec3f(
        rayHit.ray.dir_x, rayHit.ray.dir_y, rayHit.ray.dir_z).GetNormalized();
    HdEmbreeLightSampler::LightSample lightHit{};
    if (_EvaluateLightGeometryHit(rayHit, origin, dir, &lightHit)) {
        const GfVec3f Li = _enableLighting ? lightHit.Li : GfVec3f(0.0f);
        return GfVec4f(
            std::max(0.0f, Li[0]),
            std::max(0.0f, Li[1]),
            std::max(0.0f, Li[2]),
            1.0f);
    }

    if (_enableLighting) {
        // Path trace from the camera ray origin.  _TracePath re-intersects
        // the ray and derives all surface data (shading context, material
        // closure, shading normal) at the hit itself, so evaluating the
        // material here as well would be pure duplicate work -- it used to
        // roughly double the per-sample material-evaluation cost on
        // material-heavy scenes.
        const GfVec3f lightingColor =
            _TracePath(origin, dir, rayDiff, sampler.RootDomain());
        return GfVec4f(
            std::max(0.0f, lightingColor[0]),
            std::max(0.0f, lightingColor[1]),
            std::max(0.0f, lightingColor[2]),
            1.0f);
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
    GfVec3f normal = _ResolveObjectSpaceNormal(
        prototypeContext, instanceContext->rootScene, rayHit.hit.geomID,
        rayHit);

    // Transform the normal from object space to world space.
    normal = instanceContext->objectToWorldMatrix.TransformDir(normal);
    normal.Normalize();

    // Build shading context via shared helper (texcoord, displayColor,
    // tangent frame all constructed consistently).
    mxcpp::ShadingContext ctx = _BuildShadingContext(
        rayHit, rayDiff, instanceContext, prototypeContext, hitPos, normal);
    _GeomPropCallbackData cbData{
        &prototypeContext->primvarMapByString,
        rayHit.hit.primID, rayHit.hit.u, rayHit.hit.v};
    ctx.geomPropLookup = &_SampleGeomProp;
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

    // The lighting-enabled path returned above; this is the camera-light
    // shading fallback, which is what actually consumes the closure and the
    // resolved shading normal computed in this function.
    const GfVec3f rawDir(
        rayHit.ray.dir_x, rayHit.ray.dir_y, rayHit.ray.dir_z);
    float diffuseLight = fabs(GfDot(-rawDir, normal)) *
        HdEmbreeConfig::GetInstance().cameraLightIntensity;

    float aoLightIntensity =
        _ComputeAmbientOcclusion(
            hitPos,
            normal,
            sampler.RootDomain()
                .Fork(HdEmbreeSampleDomainKey::AmbientOcclusion));

    const GfVec3f lightingColor = materialColor * diffuseLight * aoLightIntensity;

    GfVec4f output;
    output[0] = std::max(0.0f, lightingColor[0]);
    output[1] = std::max(0.0f, lightingColor[1]);
    output[2] = std::max(0.0f, lightingColor[2]);
    output[3] = 1.0f;
    return output;
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
