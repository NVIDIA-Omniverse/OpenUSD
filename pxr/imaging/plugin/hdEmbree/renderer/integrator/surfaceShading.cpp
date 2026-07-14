//
// Copyright 2018 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
// Hit interpretation and shading-context construction.

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

void
HdEmbreeRenderer::_PropagateRayDifferential(
    _SurfaceDifferentials const& surface,
    GfVec3f const& hitPos,
    GfVec3f const& normal,
    GfVec3f const& wo,
    GfVec3f const& wi,
    float eta,
    bool specular,
    HdEmbreeRayDifferential* rayDifferential) const
{
    if (!rayDifferential) {
        return;
    }
    if (!specular) {
        rayDifferential->hasDifferentials = false;
        return;
    }
    if (!rayDifferential->hasDifferentials) {
        return;
    }

    const GfVec3f dndx =
        surface.dndu * surface.dudx + surface.dndv * surface.dvdx;
    const GfVec3f dndy =
        surface.dndu * surface.dudy + surface.dndv * surface.dvdy;
    rayDifferential->rxOrigin = hitPos + surface.dpdx;
    rayDifferential->ryOrigin = hitPos + surface.dpdy;

    const GfVec3f dwodx = -rayDifferential->rxDirection - wo;
    const GfVec3f dwody = -rayDifferential->ryDirection - wo;
    const float dwoDotnDx = GfDot(dwodx, normal) + GfDot(wo, dndx);
    const float dwoDotnDy = GfDot(dwody, normal) + GfDot(wo, dndy);

    if (eta == 1.0f) {
        rayDifferential->rxDirection =
            wi - dwodx +
            2.0f * (GfDot(wo, normal) * dndx + dwoDotnDx * normal);
        rayDifferential->ryDirection =
            wi - dwody +
            2.0f * (GfDot(wo, normal) * dndy + dwoDotnDy * normal);
    } else if (eta != 0.0f) {
        const float wiDotN = GfDot(wi, normal);
        const float safeWiDotN = wiDotN != 0.0f ? wiDotN : 1.0f;
        const float mu =
            GfDot(wo, normal) / eta - std::abs(wiDotN);
        const float derivativeScale =
            1.0f / eta +
            GfDot(wo, normal) / (eta * eta * safeWiDotN);
        const float dmuDx = dwoDotnDx * derivativeScale;
        const float dmuDy = dwoDotnDy * derivativeScale;
        rayDifferential->rxDirection =
            wi - eta * dwodx + mu * dndx + dmuDx * normal;
        rayDifferential->ryDirection =
            wi - eta * dwody + mu * dndy + dmuDy * normal;
    } else {
        rayDifferential->hasDifferentials = false;
        return;
    }

    constexpr float maxDifferentialLengthSquared = 1e16f;
    if (rayDifferential->rxDirection.GetLengthSq() >
            maxDifferentialLengthSquared ||
        rayDifferential->ryDirection.GetLengthSq() >
            maxDifferentialLengthSquared ||
        rayDifferential->rxOrigin.GetLengthSq() >
            maxDifferentialLengthSquared ||
        rayDifferential->ryOrigin.GetLengthSq() >
            maxDifferentialLengthSquared) {
        rayDifferential->hasDifferentials = false;
    }
}

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

PXR_NAMESPACE_CLOSE_SCOPE
