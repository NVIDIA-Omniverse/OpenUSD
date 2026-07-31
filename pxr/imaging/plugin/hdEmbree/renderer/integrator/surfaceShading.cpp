//
// Copyright 2018 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
// Hit interpretation and shading-context construction.

#include <renderer/geometry/meshSamplers.h>
#include <renderer/geometry/normalTransforms.h>
#include <renderer/geometry/primvarSampling.h>
#include <renderer/geometry/surfaceDerivatives.h>
#include <renderer/geometry/triangleMesh.h>
#include <renderer/geometry/wireframe.h>
#include <renderer/integrator/shadingNormal.h>
#include <renderer/materials/MaterialXCpp/graph.h>
#include <renderer/materials/MaterialXCpp/materials/bsdf/closureTraversal.h>
#include <renderer/materials/MaterialXCpp/shadingContext.h>
#include <renderer/rayUtil.h>
#include <renderer/renderBuffer.h>
#include <renderer/renderer.h>
#include <renderer/rendererMath.h>

#include "pxr/base/work/loops.h"
#include "pxr/base/work/threadLimits.h"
#include "pxr/imaging/hd/perfLog.h"
#include "pxr/imaging/hd/tokens.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <optional>
#include <thread>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

static const TfToken _tokensSt("st");

static bool
_SampleTriangleNormalCorners(
    ty::PrototypeContext const* prototypeContext,
    unsigned int primitiveId,
    GfVec3f* outN0,
    GfVec3f* outN1,
    GfVec3f* outN2)
{
    if (!prototypeContext || !prototypeContext->triangleNormalSampler ||
        !outN0 || !outN1 || !outN2) {
        return false;
    }

    switch (prototypeContext->triangleNormalSamplerKind) {
    case ty::TriangleCornerSamplerKind::vertex:
        return static_cast<ty::TriangleVertexSampler const*>(
                   prototypeContext->triangleNormalSampler)
            ->SampleVertices(primitiveId, outN0, outN1, outN2);
    case ty::TriangleCornerSamplerKind::faceVarying:
        return static_cast<ty::TriangleFaceVaryingSampler const*>(
                   prototypeContext->triangleNormalSampler)
            ->SampleVertices(primitiveId, outN0, outN1, outN2);
    case ty::TriangleCornerSamplerKind::none:
        return false;
    }
    return false;
}

static void
_ComputeScreenSpaceDerivatives(ty::RayDifferential const& diffRay,
                               GfVec3f const& posHitWld,
                               GfVec3f const& normalTangentPlaneWld,
                               GfVec3f const& dPdu, GfVec3f const& dPdv,
                               GfMatrix4d const& viewMatrix,
                               GfMatrix4d const& inverseProjMatrix,
                               float imageWidth, float imageHeight,
                               int samplesPerPixel, mxcpp::ShadingContext& ctx)
{
    GfVec3f dPdx(0.0f);
    GfVec3f dPdy(0.0f);

    if (diffRay.hasDifferentials) {
        // Intersect differential rays with the tangent plane at posHitWld.
        float planeOffset = -GfDot(normalTangentPlaneWld, posHitWld);
        float rxDotN =
            GfDot(normalTangentPlaneWld, diffRay.rxDirection);
        if (std::abs(rxDotN) > 1e-10f) {
            const float tRayX =
                -(GfDot(normalTangentPlaneWld, diffRay.rxOrigin) +
                  planeOffset) /
                rxDotN;
            const GfVec3f posRayXWld =
                diffRay.rxOrigin + tRayX * diffRay.rxDirection;
            dPdx = posRayXWld - posHitWld;
        }
        float ryDotN =
            GfDot(normalTangentPlaneWld, diffRay.ryDirection);
        if (std::abs(ryDotN) > 1e-10f) {
            const float tRayY =
                -(GfDot(normalTangentPlaneWld, diffRay.ryOrigin) +
                  planeOffset) /
                ryDotN;
            const GfVec3f posRayYWld =
                diffRay.ryOrigin + tRayY * diffRay.ryDirection;
            dPdy = posRayYWld - posHitWld;
        }
    } else {
        // Fallback: approximate dPdx/dPdy from camera projection.
        GfVec3f hitCamera = GfVec3f(viewMatrix.Transform(posHitWld));
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
    ty::InstanceContext const* instanceContext,
    ty::DisplacedSubdivFrame const* displacedFrame)
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
ty::Renderer::_IsEdgeOnlyWireframeHit(
    RTCRayHit const& primaryHit) const
{
    if (primaryHit.hit.geomID == RTC_INVALID_GEOMETRY_ID ||
        primaryHit.hit.instID[0] == RTC_INVALID_GEOMETRY_ID ||
        _GetLightGeometryHit(primaryHit)) {
        return false;
    }

    RTCGeometry const instanceGeometry =
        rtcGetGeometry(_scene, primaryHit.hit.instID[0]);
    ty::InstanceContext const* const instanceContext = instanceGeometry
        ? static_cast<ty::InstanceContext const*>(
            rtcGetGeometryUserData(instanceGeometry))
        : nullptr;
    RTCGeometry const prototypeGeometry = instanceContext
        ? rtcGetGeometry(instanceContext->rootScene, primaryHit.hit.geomID)
        : nullptr;
    ty::PrototypeContext const* const prototypeContext = prototypeGeometry
        ? static_cast<ty::PrototypeContext const*>(
            rtcGetGeometryUserData(prototypeGeometry))
        : nullptr;
    return prototypeContext &&
        prototypeContext->wireframeMode ==
            ty::WireframeMode::edgeOnly;
}

void
ty::Renderer::_ApplyWireframe(
    RTCRayHit const& primaryHit,
    ty::RayDifferential const& diffRay,
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
    ty::InstanceContext const* const instanceContext =
        static_cast<ty::InstanceContext const*>(
            rtcGetGeometryUserData(instanceGeometry));
    if (!instanceContext) {
        return;
    }

    RTCGeometry const prototypeGeometry = rtcGetGeometry(
        instanceContext->rootScene, primaryHit.hit.geomID);
    if (!prototypeGeometry) {
        return;
    }
    ty::PrototypeContext const* const prototypeContext =
        static_cast<ty::PrototypeContext const*>(
            rtcGetGeometryUserData(prototypeGeometry));
    if (!prototypeContext ||
        prototypeContext->wireframeMode ==
            ty::WireframeMode::disabled) {
        return;
    }

    const GfVec3f posHitWld = ty::CalculateHitPosition(primaryHit);
    ty::DisplacedSubdivFrame displacedFrame;
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
    _ComputeScreenSpaceDerivatives(diffRay, posHitWld, normalSrfWldExt,
                                   parametricFrame->dPdu, parametricFrame->dPdv,
                                   _viewMatrix, _inverseProjMatrix,
                                   static_cast<float>(_dataWindow.GetWidth()),
                                   static_cast<float>(_dataWindow.GetHeight()),
                                   _settings.samplesToConvergence,
                                   wireframeContext);
    const float derivativeScale =
        ty::ComputeWireframeDerivativeScale(_settings.samplesToConvergence);
    const ty::WireframeSample sample{
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
        std::vector<float> const* const levels =
            prototypeContext->subdivisionLevels;
        ty::SubdivWireframeTopology const topology{
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
        opacity = ty::ComputeSubdivisionWireframeOpacity(
            sample, primaryHit.hit.primID, topology, lineWidth);
    } else {
        opacity = ty::ComputeTriangleWireframeOpacity(
            sample, lineWidth);
    }
    opacity = std::clamp(opacity, 0.0f, 1.0f);

    if (prototypeContext->wireframeMode ==
            ty::WireframeMode::edgeOnly) {
        *color = ty::CompositeEdgeOnlyWireframe(
            _colorClearValue, opacity);
        return;
    }
    if (!prototypeContext->blendWireframeColor || opacity <= 0.0f) {
        return;
    }

    if (_wireframeColor == GfVec4f(0.0f)) {
        // Match Storm's unset edge-on-surface color: dim the shaded result.
        if (prototypeContext->wireframeMode ==
                ty::WireframeMode::edgeOnSurface) {
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
ty::Renderer::_PropagateRayDifferential(
    _SurfaceDifferentials const& surface, GfVec3f const& posHitWld,
    GfVec3f const& normalShdWldOut, GfVec3f const& omegaOutWld,
    GfVec3f const& omegaInWld, float eta, bool specular,
    ty::RayDifferential* diffRay) const
{
    if (!diffRay) {
        return;
    }
    if (!specular) {
        diffRay->hasDifferentials = false;
        return;
    }
    if (!diffRay->hasDifferentials) {
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
    diffRay->rxOrigin = posHitWld + surface.dpdx;
    diffRay->ryOrigin = posHitWld + surface.dpdy;

    const GfVec3f dwodx = -diffRay->rxDirection - omegaOutWld;
    const GfVec3f dwody = -diffRay->ryDirection - omegaOutWld;
    const float dwoDotnDx =
        GfDot(dwodx, normalShdWldOut) + GfDot(omegaOutWld, dndx);
    const float dwoDotnDy =
        GfDot(dwody, normalShdWldOut) + GfDot(omegaOutWld, dndy);

    if (eta == 1.0f) {
        diffRay->rxDirection =
            omegaInWld - dwodx +
            2.0f * (GfDot(omegaOutWld, normalShdWldOut) * dndx +
                    dwoDotnDx * normalShdWldOut);
        diffRay->ryDirection =
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
        diffRay->rxDirection =
            omegaInWld - eta * dwodx + mu * dndx + dmuDx * normalShdWldOut;
        diffRay->ryDirection =
            omegaInWld - eta * dwody + mu * dndy + dmuDy * normalShdWldOut;
    } else {
        diffRay->hasDifferentials = false;
        return;
    }

    constexpr float maxDifferentialLengthSquared = 1e16f;
    if (diffRay->rxDirection.GetLengthSq() > maxDifferentialLengthSquared ||
        diffRay->ryDirection.GetLengthSq() > maxDifferentialLengthSquared ||
        diffRay->rxOrigin.GetLengthSq() > maxDifferentialLengthSquared ||
        diffRay->ryOrigin.GetLengthSq() > maxDifferentialLengthSquared) {
        diffRay->hasDifferentials = false;
    }
}

bool
ty::Renderer::_TryBuildSurfaceInteraction(
    RTCRayHit const& rayHit, GfVec3f const& omegaOutWld,
    _SurfaceInteraction* outInteraction,
    ty::InstanceContext const** outInstance,
    ty::PrototypeContext const** outPrototype) const
{
    if (!outInteraction ||
        rayHit.hit.geomID == RTC_INVALID_GEOMETRY_ID ||
        _GetLightGeometryHit(rayHit)) {
        return false;
    }

    ty::InstanceContext const* instanceContext =
        static_cast<ty::InstanceContext const*>(
            rtcGetGeometryUserData(
                rtcGetGeometry(_scene, rayHit.hit.instID[0])));
    if (!instanceContext) {
        return false;
    }
    RTCGeometry const prototypeGeometry =
        rtcGetGeometry(instanceContext->rootScene, rayHit.hit.geomID);
    ty::PrototypeContext const* prototypeContext =
        static_cast<ty::PrototypeContext const*>(
            rtcGetGeometryUserData(prototypeGeometry));
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
    ty::DisplacedSubdivFrame displacedFrame;
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
    interaction.posHitWld = ty::CalculateHitPosition(rayHit);
    interaction.normalGeomWldExt = normalGeomWldExt;
    interaction.normalSrfWldExt = normalSrfWldExt;
    interaction.instanceContext = instanceContext;
    interaction.prototypeContext = prototypeContext;
    interaction.primitiveId = rayHit.hit.primID;
    interaction.baryU = rayHit.hit.u;
    interaction.baryV = rayHit.hit.v;
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

GfVec3f
ty::Renderer::_ComputeSmoothShadowOffsetOut(
    _SurfaceInteraction const& interaction) const
{
    ty::PrototypeContext const* const prototypeContext =
        interaction.prototypeContext;
    ty::InstanceContext const* const instanceContext =
        interaction.instanceContext;
    // Refined/displaced prototypes already shade a finely tessellated surface;
    // lifting them from their control cage would over-correct the origin.
    if (!prototypeContext || !instanceContext ||
        prototypeContext->refined || prototypeContext->displaced ||
        !prototypeContext->triangleNormalSampler) {
        return GfVec3f(0.0f);
    }

    GfVec3f n0;
    GfVec3f n1;
    GfVec3f n2;
    if (!_SampleTriangleNormalCorners(
            prototypeContext, interaction.primitiveId, &n0, &n1, &n2)) {
        return GfVec3f(0.0f);
    }

    const GfVec3f offsetExt =
        ty::ComputeSmoothTriangleShadowOffsetFromContext(
            prototypeContext, instanceContext, interaction.primitiveId,
            n0, n1, n2, interaction.baryU, interaction.baryV,
            interaction.normalGeomWldExt);
    return interaction.frontFacing ? offsetExt : -offsetExt;
}

mxcpp::ShadingContext
ty::Renderer::_BuildShadingContext(
    RTCRayHit const& rayHit,
    ty::RayDifferential const& diffRay,
    ty::InstanceContext const* instanceContext,
    ty::PrototypeContext const* prototypeContext,
    _SurfaceInteraction const& interaction,
    GfVec3f* outDndu,
    GfVec3f* outDndv,
    _ShadingContextOptions options) const
{
    const GfVec3f posHitWld = interaction.posHitWld;
    const GfVec3f normalSrfWldOut = interaction.GetNormalSrfWldOut();
    const GfVec3f normalSrfWldExt = interaction.normalSrfWldExt;
    ty::DisplacedSubdivFrame const* displacedFrame =
        interaction.displacedFrame.valid
            ? &interaction.displacedFrame
            : nullptr;
    const float sideSign = interaction.frontFacing ? 1.0f : -1.0f;

    mxcpp::Vec2f texcoordVal(0.0f);
    {
        auto it = prototypeContext->primvarMap.find(_tokensSt);
        if (it != prototypeContext->primvarMap.end()) {
            ty::SampleTexcoord(
                it->second.get(), rayHit.hit.primID,
                rayHit.hit.u, rayHit.hit.v,
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

    GfVec3f posHitObj =
        instanceContext->worldToObjectMatrix.Transform(posHitWld);
    if (displacedFrame) {
        posHitObj = displacedFrame->posObj;
    } else if (options.computeObjectSpacePosition) {
        // Exact primitive interpolation avoids crossing a discontinuous
        // procedural cell boundary through transform cancellation. Coarse
        // triangles reuse the genuine corner cache needed by smooth shadows.
        GfVec3f p0;
        GfVec3f p1;
        GfVec3f p2;
        if (!prototypeContext->refined &&
            ty::SampleTrianglePositions(
                prototypeContext, rayHit.hit.primID, &p0, &p1, &p2)) {
            const GfVec3f interpolatedPos =
                ty::InterpolateTrianglePosition(
                    p0, p1, p2, rayHit.hit.u, rayHit.hit.v);
            if (ty::IsFinite(interpolatedPos)) {
                posHitObj = interpolatedPos;
            }
        } else {
            RTCGeometry const prototypeGeometry = rtcGetGeometry(
                instanceContext->rootScene, rayHit.hit.geomID);
            if (prototypeGeometry) {
                // rtcInterpolate1 writes through SIMD-width arrays.
                alignas(16) float sampled[4] = {};
                rtcInterpolate1(
                    prototypeGeometry,
                    rayHit.hit.primID,
                    rayHit.hit.u,
                    rayHit.hit.v,
                    RTC_BUFFER_TYPE_VERTEX,
                    0,
                    sampled,
                    nullptr,
                    nullptr,
                    3);
                const GfVec3f interpolatedPos(
                    sampled[0], sampled[1], sampled[2]);
                if (ty::IsFinite(interpolatedPos)) {
                    posHitObj = interpolatedPos;
                }
            }
        }
    }
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
        const auto sampleFrame = [&](
            ty::PrimvarSampler const* tangentSampler,
            ty::PrimvarSampler const* bitangentSampler) {
            if (!tangentSampler || !bitangentSampler) {
                return false;
            }
            if (!tangentSampler->Sample(
                    rayHit.hit.primID, rayHit.hit.u, rayHit.hit.v, &tangent) ||
                !bitangentSampler->Sample(
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
            sampleFrame(
                prototypeContext->tangentSampler,
                prototypeContext->bitangentSampler) ||
            sampleFrame(
                prototypeContext->computedTangentSampler,
                prototypeContext->computedBitangentSampler);
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
    ctx.position = ty::ToMx(posHitObj);
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
    ty::MaterialEvalServices const* materialEvalServices =
        prototypeContext->materialEvalServices
            ? prototypeContext->materialEvalServices
            : &_materialEvalServices;
    ctx.textureSystem = materialEvalServices->textureSystem;
    ctx.frame = materialEvalServices->frame;
    ctx.time = materialEvalServices->time;
    ctx.bypassColorTransforms = ty::BypassesColorTransforms(
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
            diffRay, posHitWld, normalSrfWldOut, dPdu, dPdv, _viewMatrix,
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
ty::Renderer::_TryEvalSurfaceClosureAtHit(
    RTCRayHit const& rayHit, GfVec3f const& omegaOutWld,
    mxcpp::SurfaceClosure* outClosure, GfVec3f* normalShdWldOutOutput,
    GfVec3f* normalGeomWldExtOutput,
    ty::PrototypeContext const** outGeometry) const
{
    _SurfaceInteraction interaction;
    ty::InstanceContext const* instanceContext = nullptr;
    ty::PrototypeContext const* prototypeContext = nullptr;
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
    ty::RayDifferential defaultRayDiff;
    const _ShadingContextOptions options(
        false, surfaceGraph->RequiresObjectSpacePosition());
    mxcpp::ShadingContext ctx = _BuildShadingContext(
        rayHit,
        defaultRayDiff,
        instanceContext,
        prototypeContext,
        interaction,
        nullptr,
        nullptr,
        options);
    ty::PrimvarLookup cbData{
        &prototypeContext->geomPropSamplers,
        rayHit.hit.primID,
        rayHit.hit.u,
        rayHit.hit.v};
    ctx.geomPropLookup = &ty::SamplePrimvar;
    ctx.geomPropUserData = &cbData;
    ctx.uniformProps = &prototypeContext->geomPropUniformValues;
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
        if (ty::TryResolveNormalShdWldOut(
                candidate, interaction.GetNormalSrfWldOut(),
                &normalizedCandidate)) {
            normalShdWldOut = normalizedCandidate;
        } else {
            ++_invalidMaterialNormalCount;
        }
    }
    _invalidMaterialNormalCount.fetch_add(
        mxcpp::Bsdf::detail::PrepareShadingNormals(
            &outClosure->bsdfTree,
            ty::ToMx(normalShdWldOut),
            ty::ToMx(interaction.GetNormalGeomWldOut()),
            ty::ToMx(omegaOutWld)),
        std::memory_order_relaxed);
    if (normalShdWldOutOutput) {
        *normalShdWldOutOutput = normalShdWldOut;
    }
    return true;
}

PXR_NAMESPACE_CLOSE_SCOPE
