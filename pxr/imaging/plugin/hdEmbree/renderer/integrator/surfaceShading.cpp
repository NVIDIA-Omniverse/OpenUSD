//
// Copyright 2018 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
// Hit interpretation and shading-context construction.

#include "pxr/imaging/plugin/hdEmbree/renderer/renderer.h"
#include "../rendererImpl.h"
#include "pxr/imaging/plugin/hdEmbree/renderer/geometry/wireframe.h"
#include "pxr/imaging/plugin/hdEmbree/renderer/renderBuffer.h"

#include "pxr/imaging/hd/perfLog.h"
#include "pxr/base/work/loops.h"
#include "pxr/base/work/threadLimits.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <optional>
#include <thread>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

struct _WireframeParametricFrame
{
    GfVec3f dPdu;
    GfVec3f dPdv;
};

std::optional<_WireframeParametricFrame>
_GetWireframeParametricFrame(
    RTCGeometry prototypeGeometry,
    RTCRayHit const& primaryHit,
    HdEmbreeInstanceContext const* instanceContext,
    HdEmbreeDisplacedSubdivFrame const* displacedFrame)
{
    if (!prototypeGeometry || !instanceContext) {
        return std::nullopt;
    }

    GfVec3f objectDPdu;
    GfVec3f objectDPdv;
    if (displacedFrame && displacedFrame->valid) {
        objectDPdu = displacedFrame->dPdu;
        objectDPdv = displacedFrame->dPdv;
    } else {
        // rtcInterpolate1 returns derivatives in the geometry's own hit
        // parameterization. Unlike the material shading frame, these values
        // are deliberately not transformed into authored st space.
        alignas(16) float sampled[4] = {};
        alignas(16) float sampledDu[4] = {};
        alignas(16) float sampledDv[4] = {};
        rtcInterpolate1(
            prototypeGeometry,
            primaryHit.hit.primID,
            primaryHit.hit.u,
            primaryHit.hit.v,
            RTC_BUFFER_TYPE_VERTEX,
            0,
            sampled,
            sampledDu,
            sampledDv,
            3);
        objectDPdu = GfVec3f(
            sampledDu[0], sampledDu[1], sampledDu[2]);
        objectDPdv = GfVec3f(
            sampledDv[0], sampledDv[1], sampledDv[2]);
    }

    const GfVec3f worldDPdu =
        instanceContext->objectToWorldMatrix.TransformDir(objectDPdu);
    const GfVec3f worldDPdv =
        instanceContext->objectToWorldMatrix.TransformDir(objectDPdv);
    if (!_IsFinite(worldDPdu) || !_IsFinite(worldDPdv) ||
        GfCross(worldDPdu, worldDPdv).GetLengthSq() <= 1.0e-18f) {
        return std::nullopt;
    }
    return _WireframeParametricFrame{worldDPdu, worldDPdv};
}

} // anonymous namespace

bool
HdEmbreeRenderer::_IsEdgeOnlyWireframeHit(
    RTCRayHit const& primaryHit) const
{
    if (primaryHit.hit.geomID == RTC_INVALID_GEOMETRY_ID ||
        primaryHit.hit.instID[0] == RTC_INVALID_GEOMETRY_ID ||
        _GetLightGeometryHit(primaryHit)) {
        return false;
    }

    RTCGeometry const instanceGeometry =
        rtcGetGeometry(_scene, primaryHit.hit.instID[0]);
    auto const* const instanceContext = instanceGeometry
        ? static_cast<HdEmbreeInstanceContext const*>(
            rtcGetGeometryUserData(instanceGeometry))
        : nullptr;
    RTCGeometry const prototypeGeometry = instanceContext
        ? rtcGetGeometry(instanceContext->rootScene, primaryHit.hit.geomID)
        : nullptr;
    auto const* const prototypeContext = prototypeGeometry
        ? static_cast<HdEmbreePrototypeContext const*>(
            rtcGetGeometryUserData(prototypeGeometry))
        : nullptr;
    return prototypeContext &&
        prototypeContext->wireframeMode ==
            HdEmbreeWireframeMode::edgeOnly;
}

void
HdEmbreeRenderer::_ApplyWireframe(
    RTCRayHit const& primaryHit,
    HdEmbreeRayDifferential const& rayDiff,
    GfVec4f* color) const
{
    if (!color ||
        primaryHit.hit.geomID == RTC_INVALID_GEOMETRY_ID ||
        primaryHit.hit.instID[0] == RTC_INVALID_GEOMETRY_ID ||
        _GetLightGeometryHit(primaryHit)) {
        return;
    }

    RTCGeometry const instanceGeometry =
        rtcGetGeometry(_scene, primaryHit.hit.instID[0]);
    if (!instanceGeometry) {
        return;
    }
    auto const* const instanceContext =
        static_cast<HdEmbreeInstanceContext const*>(
            rtcGetGeometryUserData(instanceGeometry));
    if (!instanceContext) {
        return;
    }

    RTCGeometry const prototypeGeometry = rtcGetGeometry(
        instanceContext->rootScene, primaryHit.hit.geomID);
    if (!prototypeGeometry) {
        return;
    }
    auto const* const prototypeContext =
        static_cast<HdEmbreePrototypeContext const*>(
            rtcGetGeometryUserData(prototypeGeometry));
    if (!prototypeContext ||
        prototypeContext->wireframeMode ==
            HdEmbreeWireframeMode::disabled) {
        return;
    }

    const GfVec3f hitPos = _CalculateHitPosition(primaryHit);
    HdEmbreeDisplacedSubdivFrame displacedFrame;
    GfVec3f normal = _ResolveObjectSpaceNormal(
        prototypeContext, instanceContext->rootScene,
        primaryHit.hit.geomID, primaryHit,
        &displacedFrame);
    normal = _TransformNormalToWorld(instanceContext, normal);

    const std::optional<_WireframeParametricFrame> parametricFrame =
        _GetWireframeParametricFrame(
            prototypeGeometry,
            primaryHit,
            instanceContext,
            &displacedFrame);
    if (!parametricFrame) {
        return;
    }
    mxcpp::ShadingContext wireframeContext;
    _ComputeScreenSpaceDerivatives(
        rayDiff,
        hitPos,
        normal,
        parametricFrame->dPdu,
        parametricFrame->dPdv,
        _viewMatrix,
        _inverseProjMatrix,
        static_cast<float>(_dataWindow.GetWidth()),
        static_cast<float>(_dataWindow.GetHeight()),
        _samplesToConvergence,
        wireframeContext);
    const float derivativeScale =
        HdEmbreeComputeWireframeDerivativeScale(_samplesToConvergence);
    const HdEmbreeWireframeSample sample{
        primaryHit.hit.u,
        primaryHit.hit.v,
        wireframeContext.dudx * derivativeScale,
        wireframeContext.dvdx * derivativeScale,
        wireframeContext.dudy * derivativeScale,
        wireframeContext.dvdy * derivativeScale};
    const float lineWidth = prototypeContext->wireframeLineWidth > 0.0f
        ? prototypeContext->wireframeLineWidth
        : _wireframeLineWidth;

    float opacity = 0.0f;
    if (prototypeContext->refined) {
        auto const* const levels = prototypeContext->subdivisionLevels;
        HdEmbreeSubdivWireframeTopology const topology{
            prototypeContext->faceVertexCounts.empty()
                ? nullptr
                : prototypeContext->faceVertexCounts.cdata(),
            prototypeContext->faceVertexCounts.size(),
            prototypeContext->faceVertexOffsets.empty()
                ? nullptr
                : prototypeContext->faceVertexOffsets.data(),
            prototypeContext->faceVertexOffsets.size(),
            !levels || levels->empty() ? nullptr : levels->data(),
            levels ? levels->size() : 0};
        opacity = HdEmbreeComputeSubdivisionWireframeOpacity(
            sample, primaryHit.hit.primID, topology, lineWidth);
    } else {
        opacity = HdEmbreeComputeTriangleWireframeOpacity(
            sample, lineWidth);
    }
    opacity = std::clamp(opacity, 0.0f, 1.0f);

    if (prototypeContext->wireframeMode ==
            HdEmbreeWireframeMode::edgeOnly) {
        *color = HdEmbreeCompositeEdgeOnlyWireframe(
            _colorClearValue, opacity);
        return;
    }
    if (!prototypeContext->blendWireframeColor || opacity <= 0.0f) {
        return;
    }

    if (_wireframeColor == GfVec4f(0.0f)) {
        // Match Storm's unset edge-on-surface color: dim the shaded result.
        if (prototypeContext->wireframeMode ==
                HdEmbreeWireframeMode::edgeOnSurface) {
            const float scale = 1.0f - 0.5f * opacity;
            (*color)[0] *= scale;
            (*color)[1] *= scale;
            (*color)[2] *= scale;
        }
        return;
    }

    const float blend =
        opacity * std::clamp(_wireframeColor[3], 0.0f, 1.0f);
    for (int channel = 0; channel < 3; ++channel) {
        (*color)[channel] =
            (1.0f - blend) * (*color)[channel] +
            blend * _wireframeColor[channel];
    }
}

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
    HdEmbreeDisplacedSubdivFrame const* displacedFrame,
    GfVec3f* outDndu,
    GfVec3f* outDndv,
    _ShadingContextOptions options) const
{
    mxcpp::Vec2f texcoordVal(0.0f);
    {
        auto it = prototypeContext->primvarMap.find(_tokensSt);
        if (it != prototypeContext->primvarMap.end()) {
            HdEmbreeSampleTexcoord(
                it->second, rayHit.hit.primID, rayHit.hit.u, rayHit.hit.v,
                &texcoordVal);
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
    const GfVec3f objectNormal =
        _TransformNormalToObject(instanceContext, normal);
    if (_IsSubdivMesh(prototypeContext)) {
        _ComputeSubdivSurfaceDerivatives(
            prototypeContext,
            instanceContext->rootScene, rayHit.hit.geomID,
            rayHit.hit.primID, rayHit.hit.u, rayHit.hit.v,
            objectNormal, &dPdu, &dPdv, &dndu, &dndv,
            displacedFrame);
    } else {
        _ComputeTriangleSurfaceDerivatives(
            prototypeContext,
            rayHit.hit.primID, rayHit.hit.u, rayHit.hit.v, objectNormal,
            &dPdu, &dPdv, &dndu, &dndv);
    }

    const GfVec3f objectHitPos =
        instanceContext->worldToObjectMatrix.Transform(hitPos);
    const GfVec3f objectDPdu = dPdu;
    const GfVec3f objectDPdv = dPdv;

    // Object space -> world space
    dPdu = instanceContext->objectToWorldMatrix.TransformDir(dPdu);
    dPdv = instanceContext->objectToWorldMatrix.TransformDir(dPdv);
    dndu = _TransformNormalDerivativeToWorld(
        instanceContext, objectNormal, dndu);
    dndv = _TransformNormalDerivativeToWorld(
        instanceContext, objectNormal, dndv);

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
        haveTangentFrame =
            _TryNormalizeDirection(tangent, &tangent);
        if (haveTangentFrame) {
            bitangent -= normal * GfDot(normal, bitangent);
            bitangent -= tangent * GfDot(tangent, bitangent);
            haveTangentFrame =
                _TryNormalizeDirection(bitangent, &bitangent);
        }
    }

    if (!haveTangentFrame) {
        tangent = dPdu - normal * GfDot(normal, dPdu);
        if (_TryNormalizeDirection(tangent, &tangent)) {
            bitangent =
                dPdv - normal * GfDot(normal, dPdv) -
                tangent * GfDot(tangent, dPdv);
        }
        if (!_TryNormalizeDirection(tangent, &tangent) ||
            !_TryNormalizeDirection(bitangent, &bitangent)) {
            GfBuildOrthonormalFrame(normal, &tangent, &bitangent);
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
    ctx.texcoord = texcoordVal;
    ctx.displayColor = _ToMx(displayColor);
    ctx.displayOpacity = displayOpacity;
    HdEmbreeMaterialEvalServices const* materialEvalServices =
        prototypeContext->materialEvalServices
            ? prototypeContext->materialEvalServices
            : &_materialEvalServices;
    ctx.textureSystem = materialEvalServices->textureSystem;
    ctx.frame = materialEvalServices->frame;
    ctx.time = materialEvalServices->time;
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
    HdEmbreeDisplacedSubdivFrame displacedFrame;
    GfVec3f normal = _ResolveObjectSpaceNormal(
        prototypeContext, instanceContext->rootScene, rayHit.hit.geomID,
        rayHit, &displacedFrame);
    normal = _TransformNormalToWorld(instanceContext, normal);
    if (outGeometricNormal) {
        *outGeometricNormal = normal;
    }

    try {
        HdEmbreeRayDifferential defaultRayDiff;
        const _ShadingContextOptions options(false);
        mxcpp::ShadingContext ctx = _BuildShadingContext(
            rayHit, defaultRayDiff,
            instanceContext, prototypeContext, hitPos, normal,
            displacedFrame.valid ? &displacedFrame : nullptr,
            nullptr, nullptr, options);
        HdEmbreePrimvarLookup cbData{
            &prototypeContext->primvarMapByString,
            rayHit.hit.primID, rayHit.hit.u, rayHit.hit.v};
        ctx.geomPropLookup = &HdEmbreeSamplePrimvar;
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
