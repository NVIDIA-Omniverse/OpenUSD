//
// Copyright 2018 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
// Hit interpretation and shading-context construction.

#include "pxr/imaging/plugin/hdEmbree/renderer/renderer.h"
#include "pxr/imaging/plugin/hdEmbree/renderer/geometry/normalTransforms.h"
#include "pxr/imaging/plugin/hdEmbree/renderer/geometry/primvarSampling.h"
#include "pxr/imaging/plugin/hdEmbree/renderer/geometry/surfaceDerivatives.h"
#include "pxr/imaging/plugin/hdEmbree/renderer/geometry/wireframe.h"
#include "pxr/imaging/plugin/hdEmbree/renderer/materials/MaterialXCpp/graph.h"
#include "pxr/imaging/plugin/hdEmbree/renderer/materials/MaterialXCpp/shadingContext.h"
#include "pxr/imaging/plugin/hdEmbree/renderer/rayUtil.h"
#include "pxr/imaging/plugin/hdEmbree/renderer/renderBuffer.h"
#include "pxr/imaging/plugin/hdEmbree/renderer/rendererMath.h"

#include "pxr/imaging/hd/perfLog.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/base/work/loops.h"
#include "pxr/base/work/threadLimits.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <optional>
#include <thread>

PXR_NAMESPACE_OPEN_SCOPE

static const TfToken _tokensTangent("tangent");
static const TfToken _tokensBitangent("bitangent");
static const TfToken _tokensComputedTangent("hdEmbreeComputedTangent");
static const TfToken _tokensComputedBitangent("hdEmbreeComputedBitangent");
static const TfToken _tokensSt("st");

static void
_ComputeScreenSpaceDerivatives(HdEmbreeRayDifferential const& rayDifferential,
                               GfVec3f const& positionHitWld,
                               GfVec3f const& normalTangentPlaneWld,
                               GfVec3f const& dPdu, GfVec3f const& dPdv,
                               GfMatrix4d const& viewMatrix,
                               GfMatrix4d const& inverseProjMatrix,
                               float imageWidth, float imageHeight,
                               int samplesPerPixel, mxcpp::ShadingContext& ctx)
{
    GfVec3f dPdx(0.0f);
    GfVec3f dPdy(0.0f);

    if (rayDifferential.hasDifferentials) {
        // Intersect differential rays with the tangent plane at positionHitWld.
        float planeOffset = -GfDot(normalTangentPlaneWld, positionHitWld);
        float rxDotN =
            GfDot(normalTangentPlaneWld, rayDifferential.rxDirection);
        if (std::abs(rxDotN) > 1e-10f) {
            float tx =
                -(GfDot(normalTangentPlaneWld, rayDifferential.rxOrigin) +
                  planeOffset) /
                rxDotN;
            GfVec3f px =
                rayDifferential.rxOrigin + tx * rayDifferential.rxDirection;
            dPdx = px - positionHitWld;
        }
        float ryDotN =
            GfDot(normalTangentPlaneWld, rayDifferential.ryDirection);
        if (std::abs(ryDotN) > 1e-10f) {
            float ty =
                -(GfDot(normalTangentPlaneWld, rayDifferential.ryOrigin) +
                  planeOffset) /
                ryDotN;
            GfVec3f py =
                rayDifferential.ryOrigin + ty * rayDifferential.ryDirection;
            dPdy = py - positionHitWld;
        }
    } else {
        // Fallback: approximate dPdx/dPdy from camera projection.
        GfVec3f hitCamera = GfVec3f(viewMatrix.Transform(positionHitWld));
        float distanceCameraWld = hitCamera.GetLength();
        if (distanceCameraWld > 1e-6f) {
            GfVec3f ndcCenter(0.0f, 0.0f, -1.0f);
            GfVec3f ndcDx(2.0f / imageWidth, 0.0f, -1.0f);
            GfVec3f ndcDy(0.0f, 2.0f / imageHeight, -1.0f);
            GfVec3f camCenter = GfVec3f(inverseProjMatrix.Transform(ndcCenter));
            GfVec3f camDx = GfVec3f(inverseProjMatrix.Transform(ndcDx));
            GfVec3f camDy = GfVec3f(inverseProjMatrix.Transform(ndcDy));
            float pixelScaleX =
                (camDx - camCenter).GetLength() * distanceCameraWld;
            float pixelScaleY =
                (camDy - camCenter).GetLength() * distanceCameraWld;

            float sppScale = (samplesPerPixel > 1)
                ? 1.0f / std::sqrt(static_cast<float>(samplesPerPixel))
                : 1.0f;
            pixelScaleX *= sppScale;
            pixelScaleY *= sppScale;

            GfVec3f tangentWld, bitangentWld;
            GfBuildOrthonormalFrame(normalTangentPlaneWld, &tangentWld,
                                    &bitangentWld);
            dPdx = tangentWld * pixelScaleX;
            dPdy = bitangentWld * pixelScaleY;
        }
    }

    ctx.dPdx = ty::ToMx(dPdx);
    ctx.dPdy = ty::ToMx(dPdy);

    // Solve for UV derivatives: A^T A x = A^T b (least squares)
    float ata00 = GfDot(dPdu, dPdu);
    float ata01 = GfDot(dPdu, dPdv);
    float ata11 = GfDot(dPdv, dPdv);
    float detATA = ty::DifferenceOfProducts(ata00, ata11, ata01, ata01);

    if (std::abs(detATA) > 1e-18f) {
        float invDet = 1.0f / detATA;
        float atb0x = GfDot(dPdu, dPdx);
        float atb1x = GfDot(dPdv, dPdx);
        float atb0y = GfDot(dPdu, dPdy);
        float atb1y = GfDot(dPdv, dPdy);

        ctx.dudx = std::clamp(
            ty::DifferenceOfProducts(ata11, atb0x, ata01, atb1x) * invDet,
            -1e8f, 1e8f);
        ctx.dvdx = std::clamp(
            ty::DifferenceOfProducts(ata00, atb1x, ata01, atb0x) * invDet,
            -1e8f, 1e8f);
        ctx.dudy = std::clamp(
            ty::DifferenceOfProducts(ata11, atb0y, ata01, atb1y) * invDet,
            -1e8f, 1e8f);
        ctx.dvdy = std::clamp(
            ty::DifferenceOfProducts(ata00, atb1y, ata01, atb0y) * invDet,
            -1e8f, 1e8f);
    }
}

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
    if (!ty::IsFinite(worldDPdu) || !ty::IsFinite(worldDPdv) ||
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

    const GfVec3f positionHitWld = ty::CalculateHitPosition(primaryHit);
    HdEmbreeDisplacedSubdivFrame displacedFrame;
    GfVec3f normalSrfWldExt = ty::ResolveObjectSpaceNormal(
        prototypeContext, instanceContext->rootScene, primaryHit.hit.geomID,
        primaryHit, &displacedFrame);
    normalSrfWldExt =
        ty::TransformNormalToWorld(instanceContext, normalSrfWldExt);

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
    _ComputeScreenSpaceDerivatives(rayDiff, positionHitWld, normalSrfWldExt,
                                   parametricFrame->dPdu, parametricFrame->dPdv,
                                   _viewMatrix, _inverseProjMatrix,
                                   static_cast<float>(_dataWindow.GetWidth()),
                                   static_cast<float>(_dataWindow.GetHeight()),
                                   _settings.samplesToConvergence,
                                   wireframeContext);
    const float derivativeScale =
        HdEmbreeComputeWireframeDerivativeScale(_settings.samplesToConvergence);
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
    _SurfaceDifferentials const& surface, GfVec3f const& positionHitWld,
    GfVec3f const& normalShdWldOut, GfVec3f const& omegaOutWld,
    GfVec3f const& omegaInWld, float eta, bool specular,
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

    const bool legalBaseApproximation =
        surface.resolvedNormalProvenance !=
            _ResolvedNormalDerivativeProvenance::BaseApproximation ||
        surface.baseNormalStatus == _BaseNormalDerivativeStatus::Ready;
    TF_VERIFY(legalBaseApproximation);
    const bool hasResolvedNormalDerivatives =
        legalBaseApproximation &&
        surface.resolvedNormalProvenance !=
            _ResolvedNormalDerivativeProvenance::None;
    const GfVec3f dndx = hasResolvedNormalDerivatives
        ? surface.dndu * surface.dudx + surface.dndv * surface.dvdx
        : GfVec3f(0.0f);
    const GfVec3f dndy = hasResolvedNormalDerivatives
        ? surface.dndu * surface.dudy + surface.dndv * surface.dvdy
        : GfVec3f(0.0f);
    rayDifferential->rxOrigin = positionHitWld + surface.dpdx;
    rayDifferential->ryOrigin = positionHitWld + surface.dpdy;

    const GfVec3f dwodx = -rayDifferential->rxDirection - omegaOutWld;
    const GfVec3f dwody = -rayDifferential->ryDirection - omegaOutWld;
    const float dwoDotnDx =
        GfDot(dwodx, normalShdWldOut) + GfDot(omegaOutWld, dndx);
    const float dwoDotnDy =
        GfDot(dwody, normalShdWldOut) + GfDot(omegaOutWld, dndy);

    if (eta == 1.0f) {
        rayDifferential->rxDirection =
            omegaInWld - dwodx +
            2.0f * (GfDot(omegaOutWld, normalShdWldOut) * dndx +
                    dwoDotnDx * normalShdWldOut);
        rayDifferential->ryDirection =
            omegaInWld - dwody +
            2.0f * (GfDot(omegaOutWld, normalShdWldOut) * dndy +
                    dwoDotnDy * normalShdWldOut);
    } else if (eta != 0.0f) {
        const float omegaInDotNormalShd = GfDot(omegaInWld, normalShdWldOut);
        const float omegaInDotNormalShdSafe =
            omegaInDotNormalShd != 0.0f ? omegaInDotNormalShd : 1.0f;
        const float mu = GfDot(omegaOutWld, normalShdWldOut) / eta -
                         std::abs(omegaInDotNormalShd);
        const float derivativeScale =
            1.0f / eta + GfDot(omegaOutWld, normalShdWldOut) /
                             (eta * eta * omegaInDotNormalShdSafe);
        const float dmuDx = dwoDotnDx * derivativeScale;
        const float dmuDy = dwoDotnDy * derivativeScale;
        rayDifferential->rxDirection =
            omegaInWld - eta * dwodx + mu * dndx + dmuDx * normalShdWldOut;
        rayDifferential->ryDirection =
            omegaInWld - eta * dwody + mu * dndy + dmuDy * normalShdWldOut;
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

bool
HdEmbreeRenderer::_TryBuildSurfaceInteraction(
    RTCRayHit const& rayHit, GfVec3f const& omegaOutWld,
    _SurfaceInteraction* outInteraction,
    HdEmbreeInstanceContext const** outInstance,
    HdEmbreePrototypeContext const** outPrototype) const
{
    if (!outInteraction ||
        rayHit.hit.geomID == RTC_INVALID_GEOMETRY_ID ||
        _GetLightGeometryHit(rayHit)) {
        return false;
    }

    HdEmbreeInstanceContext const* instanceContext =
        static_cast<HdEmbreeInstanceContext const*>(
            rtcGetGeometryUserData(
                rtcGetGeometry(_scene, rayHit.hit.instID[0])));
    if (!instanceContext) {
        return false;
    }
    HdEmbreePrototypeContext const* prototypeContext =
        static_cast<HdEmbreePrototypeContext const*>(
            rtcGetGeometryUserData(
                rtcGetGeometry(
                    instanceContext->rootScene, rayHit.hit.geomID)));
    if (!prototypeContext) {
        return false;
    }

    // Preserve Embree's authored-outside normal as immutable boundary state.
    GfVec3f normalGeomWldExt =
        prototypeContext->orientationSign *
        GfVec3f(rayHit.hit.Ng_x, rayHit.hit.Ng_y, rayHit.hit.Ng_z);
    normalGeomWldExt =
        ty::TransformNormalToWorld(instanceContext, normalGeomWldExt);
    if (!ty::TryNormalizeDirection(normalGeomWldExt, &normalGeomWldExt)) {
        return false;
    }

    // The shared resolver remains the only smooth/displaced normal source.
    HdEmbreeDisplacedSubdivFrame displacedFrame;
    GfVec3f normalSrfWldExt =
        ty::ResolveObjectSpaceNormal(
            prototypeContext,
            instanceContext->rootScene,
            rayHit.hit.geomID,
            rayHit,
            &displacedFrame);
    normalSrfWldExt =
        ty::TransformNormalToWorld(instanceContext, normalSrfWldExt);
    if (!ty::TryNormalizeDirection(normalSrfWldExt, &normalSrfWldExt)) {
        return false;
    }
    if (GfDot(normalSrfWldExt, normalGeomWldExt) < 0.0f) {
        normalSrfWldExt = -normalSrfWldExt;
    }

    _SurfaceInteraction interaction;
    interaction.positionHitWld = ty::CalculateHitPosition(rayHit);
    interaction.normalGeomWldExt = normalGeomWldExt;
    interaction.normalSrfWldExt = normalSrfWldExt;
    interaction.displacedFrame = displacedFrame;
    interaction.frontFacing = GfDot(normalGeomWldExt, omegaOutWld) > 0.0f;
    interaction.doubleSided = prototypeContext->doubleSided;

    *outInteraction = interaction;
    if (outInstance) {
        *outInstance = instanceContext;
    }
    if (outPrototype) {
        *outPrototype = prototypeContext;
    }
    return true;
}

mxcpp::ShadingContext
HdEmbreeRenderer::_BuildShadingContext(
    RTCRayHit const& rayHit,
    HdEmbreeRayDifferential const& rayDiff,
    HdEmbreeInstanceContext const* instanceContext,
    HdEmbreePrototypeContext const* prototypeContext,
    _SurfaceInteraction const& interaction,
    GfVec3f* outDndu,
    GfVec3f* outDndv,
    _ShadingContextOptions options) const
{
    const GfVec3f positionHitWld = interaction.positionHitWld;
    const GfVec3f normalSrfWldOut = interaction.GetNormalSrfWldOut();
    const GfVec3f normalSrfWldExt = interaction.normalSrfWldExt;
    HdEmbreeDisplacedSubdivFrame const* displacedFrame =
        interaction.displacedFrame.valid
            ? &interaction.displacedFrame
            : nullptr;
    const float sideSign = interaction.frontFacing ? 1.0f : -1.0f;

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
    // the _settings.enableSceneColors display-only flag.
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
        ty::TransformNormalToObject(instanceContext, normalSrfWldExt);
    if (prototypeContext->refined) {
        ty::ComputeSubdivSurfaceDerivatives(
            prototypeContext,
            instanceContext->rootScene, rayHit.hit.geomID,
            rayHit.hit.primID, rayHit.hit.u, rayHit.hit.v,
            objectNormal, &dPdu, &dPdv, &dndu, &dndv,
            displacedFrame);
    } else {
        ty::ComputeTriangleSurfaceDerivatives(
            prototypeContext,
            rayHit.hit.primID, rayHit.hit.u, rayHit.hit.v, objectNormal,
            &dPdu, &dPdv, &dndu, &dndv);
    }

    const GfVec3f objectHitPos =
        instanceContext->worldToObjectMatrix.Transform(positionHitWld);
    const GfVec3f objectDPdu = dPdu;
    const GfVec3f objectDPdv = dPdv;

    // Object space -> world space
    dPdu = instanceContext->objectToWorldMatrix.TransformDir(dPdu);
    dPdv = instanceContext->objectToWorldMatrix.TransformDir(dPdv);
    dndu = ty::TransformNormalDerivativeToWorld(
        instanceContext, objectNormal, dndu);
    dndv = ty::TransformNormalDerivativeToWorld(
        instanceContext, objectNormal, dndv);
    dndu *= sideSign;
    dndv *= sideSign;

    if (outDndu) *outDndu = dndu;
    if (outDndv) *outDndv = dndv;

    GfVec3f tangent(1.0f, 0.0f, 0.0f);
    GfVec3f bitangent(0.0f, 1.0f, 0.0f);
    bool haveTangentFrame = false;
    {
        const auto sampleFrame = [&](TfToken const& tangentToken,
                                     TfToken const& bitangentToken) {
            auto tangentIt = prototypeContext->primvarMap.find(tangentToken);
            auto bitangentIt =
                prototypeContext->primvarMap.find(bitangentToken);
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
            tangent =
                instanceContext->objectToWorldMatrix.TransformDir(tangent);
            bitangent =
                instanceContext->objectToWorldMatrix.TransformDir(bitangent);
            return true;
        };

        haveTangentFrame =
            sampleFrame(_tokensTangent, _tokensBitangent) ||
            sampleFrame(_tokensComputedTangent, _tokensComputedBitangent);
    }

    // Record handedness in the outward, view-independent frame. The side
    // transform preserves T, flips N/dN on back faces, and reconstructs B.
    float handedness = 1.0f;
    if (haveTangentFrame) {
        GfVec3f tangentOut =
            tangent - normalSrfWldExt * GfDot(normalSrfWldExt, tangent);
        GfVec3f bitangentOut =
            bitangent - normalSrfWldExt * GfDot(normalSrfWldExt, bitangent);
        haveTangentFrame =
            ty::TryNormalizeDirection(tangentOut, &tangentOut) &&
            ty::TryNormalizeDirection(bitangentOut, &bitangentOut);
        if (haveTangentFrame) {
            handedness =
                GfDot(GfCross(tangentOut, bitangentOut), normalSrfWldExt) < 0.0f
                    ? -1.0f
                    : 1.0f;
            tangent = tangentOut;
        }
    }

    if (!haveTangentFrame) {
        tangent = dPdu - normalSrfWldExt * GfDot(normalSrfWldExt, dPdu);
        GfVec3f parameterBitangent =
            dPdv - normalSrfWldExt * GfDot(normalSrfWldExt, dPdv);
        haveTangentFrame =
            ty::TryNormalizeDirection(tangent, &tangent) &&
            ty::TryNormalizeDirection(parameterBitangent, &parameterBitangent);
        if (haveTangentFrame) {
            handedness = GfDot(GfCross(tangent, parameterBitangent),
                               normalSrfWldExt) < 0.0f
                             ? -1.0f
                             : 1.0f;
        }
    }

    tangent -= normalSrfWldOut * GfDot(normalSrfWldOut, tangent);
    if (!ty::TryNormalizeDirection(tangent, &tangent)) {
        GfBuildOrthonormalFrame(normalSrfWldOut, &tangent, &bitangent);
    } else {
        bitangent = handedness * GfCross(normalSrfWldOut, tangent);
        if (!ty::TryNormalizeDirection(bitangent, &bitangent)) {
            GfBuildOrthonormalFrame(normalSrfWldOut, &tangent, &bitangent);
        }
    }

    mxcpp::ShadingContext ctx;
    ctx.position = ty::ToMx(objectHitPos);
    ctx.normal = ty::ToMx(normalSrfWldOut);
    ctx.tangent = ty::ToMx(tangent);
    ctx.bitangent = ty::ToMx(bitangent);
    ctx.viewPosition =
        ty::ToMx(GfVec3f(_inverseViewMatrix.Transform(GfVec3f(0.0f))));
    // Graph-facing texture coordinates preserve authored USD st values, which
    // match MaterialX's lower-left UV convention.  Texture backends convert
    // from that convention to their native image-space convention at lookup
    // time.
    ctx.texcoord = texcoordVal;
    ctx.displayColor = ty::ToMx(displayColor);
    ctx.displayOpacity = displayOpacity;
    HdEmbreeMaterialEvalServices const* materialEvalServices =
        prototypeContext->materialEvalServices
            ? prototypeContext->materialEvalServices
            : &_materialEvalServices;
    ctx.textureSystem = materialEvalServices->textureSystem;
    ctx.frame = materialEvalServices->frame;
    ctx.time = materialEvalServices->time;
    ctx.bypassColorTransforms = HdEmbreeBypassesColorTransforms(
        materialEvalServices->renderColorSpace);
    ctx.luminanceCoefficients =
        ty::ToMx(materialEvalServices->luminanceCoefficients);
    ctx.faceId = rayHit.hit.primID;
    ctx.baryU = rayHit.hit.u;
    ctx.baryV = rayHit.hit.v;
    ctx.dPdu = ty::ToMx(dPdu);
    ctx.dPdv = ty::ToMx(dPdv);
    ctx.dPositiondu = ty::ToMx(objectDPdu);
    ctx.dPositiondv = ty::ToMx(objectDPdv);
    ctx.objectToWorldMatrix = ty::ToMx(instanceContext->objectToWorldMatrix);
    ctx.worldToObjectMatrix = ty::ToMx(instanceContext->worldToObjectMatrix);
    ctx.hasObjectToWorldTransform = true;
    ctx.hasWorldToObjectTransform = true;

    if (options.computeScreenSpaceDerivatives) {
        _ComputeScreenSpaceDerivatives(
            rayDiff, positionHitWld, normalSrfWldOut, dPdu, dPdv, _viewMatrix,
            _inverseProjMatrix, static_cast<float>(_dataWindow.GetWidth()),
            static_cast<float>(_dataWindow.GetHeight()),
            _settings.samplesToConvergence,
            ctx);
    }

    ctx.dPositiondx = ty::ToMx(
        instanceContext->worldToObjectMatrix.TransformDir(ty::ToGf(ctx.dPdx)));
    ctx.dPositiondy = ty::ToMx(
        instanceContext->worldToObjectMatrix.TransformDir(ty::ToGf(ctx.dPdy)));

    return ctx;
}

bool
HdEmbreeRenderer::_TryEvalSurfaceClosureAtHit(
    RTCRayHit const& rayHit, GfVec3f const& omegaOutWld,
    mxcpp::SurfaceClosure* outClosure, GfVec3f* normalShdWldOutOutput,
    GfVec3f* normalGeomWldExtOutput,
    HdEmbreePrototypeContext const** outGeometry) const
{
    _SurfaceInteraction interaction;
    HdEmbreeInstanceContext const* instanceContext = nullptr;
    HdEmbreePrototypeContext const* prototypeContext = nullptr;
    if (!_TryBuildSurfaceInteraction(rayHit, omegaOutWld, &interaction,
                                     &instanceContext, &prototypeContext)) {
        return false;
    }

    if (outGeometry) {
        *outGeometry = prototypeContext;
    }
    if (normalGeomWldExtOutput) {
        *normalGeomWldExtOutput = interaction.normalGeomWldExt;
    }

    mxcpp::EvalGraph* surfaceGraph = prototypeContext->material
        ? prototypeContext->material->surfaceGraph
        : nullptr;
    if (!surfaceGraph || !outClosure) {
        return false;
    }

    const GfVec3f normalSrfWldOut = interaction.GetNormalSrfWldOut();
    GfVec3f normalShdWldOut = normalSrfWldOut;
    HdEmbreeRayDifferential defaultRayDiff;
    const _ShadingContextOptions options(false);
    mxcpp::ShadingContext ctx = _BuildShadingContext(
        rayHit,
        defaultRayDiff,
        instanceContext,
        prototypeContext,
        interaction,
        nullptr,
        nullptr,
        options);
    HdEmbreePrimvarLookup cbData{
        &prototypeContext->primvarMapByString,
        rayHit.hit.primID,
        rayHit.hit.u,
        rayHit.hit.v};
    ctx.geomPropLookup = &HdEmbreeSamplePrimvar;
    ctx.geomPropUserData = &cbData;
    ctx.uniformProps = &prototypeContext->uniformPrimvarMap;
    mxcpp::EvalOptions evalOptions;
    evalOptions.useAdobeOpenPBR = _settings.useAdobeOpenPBR;
    evalOptions.visibilityOnly = true;
    *outClosure = surfaceGraph->Evaluate(ctx, evalOptions);

    mxcpp::Vec3f resolvedNormal;
    if (outClosure->ResolveNormal(
            ctx.tangent,
            ctx.bitangent,
            ctx.normal,
            &resolvedNormal)) {
        const GfVec3f candidate = ty::ToGf(resolvedNormal);
        GfVec3f normalizedCandidate;
        const bool valid =
            ty::TryNormalizeDirection(candidate, &normalizedCandidate) &&
            GfDot(normalizedCandidate, interaction.GetNormalGeomWldOut()) >
                0.0f &&
            GfDot(normalizedCandidate, omegaOutWld) > 0.0f;
        if (valid) {
            normalShdWldOut = normalizedCandidate;
        } else {
            ++_invalidMaterialNormalCount;
        }
    }
    if (normalShdWldOutOutput) {
        *normalShdWldOutOutput = normalShdWldOut;
    }
    return true;
}

PXR_NAMESPACE_CLOSE_SCOPE
