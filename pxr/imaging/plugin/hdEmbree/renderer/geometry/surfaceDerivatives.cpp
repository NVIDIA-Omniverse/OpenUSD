//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
// Subdivision and triangle surface-frame resolution.
//
#include "surfaceDerivatives.h"
#include "context.h"
#include "displacementEvaluation.h"
#include "meshSamplers.h"
#include "normalTransforms.h"
#include "primvarSampling.h"

#include <renderer/rendererMath.h>

#include "pxr/base/tf/token.h"
#include "pxr/imaging/hd/meshUtil.h"
#include "pxr/imaging/hd/tokens.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <unordered_map>

PXR_NAMESPACE_OPEN_SCOPE

static const TfToken _tokensSt("st");

static bool
_TryBuildSurfaceNormal(
    GfVec3f const& dPdu,
    GfVec3f const& dPdv,
    GfVec3f* normal)
{
    GfVec3f normalizedU;
    GfVec3f normalizedV;
    if (!normal ||
        !ty::TryNormalizeDirection(dPdu, &normalizedU) ||
        !ty::TryNormalizeDirection(dPdv, &normalizedV)) {
        return false;
    }

    const GfVec3f relativeArea = GfCross(normalizedU, normalizedV);
    const float relativeAreaSquared = relativeArea.GetLengthSq();
    return std::isfinite(relativeAreaSquared) &&
        relativeAreaSquared > 1.0e-18f &&
        ty::TryNormalizeDirection(relativeArea, normal);
}

static void
_InterpolateSubdivPosition(
    RTCGeometry geometry,
    unsigned int primID,
    float u,
    float v,
    GfVec3f* position,
    GfVec3f* dPdu,
    GfVec3f* dPdv)
{
    // rtcInterpolate1 writes through SIMD-width arrays. Compact GfVec3f
    // objects are only 12 bytes, so write to padded temporaries first.
    alignas(16) float sampled[4] = {};
    alignas(16) float sampledDu[4] = {};
    alignas(16) float sampledDv[4] = {};
    rtcInterpolate1(
        geometry, primID, u, v, RTC_BUFFER_TYPE_VERTEX, 0,
        sampled, dPdu ? sampledDu : nullptr, dPdv ? sampledDv : nullptr, 3);
    if (position) {
        *position = GfVec3f(sampled[0], sampled[1], sampled[2]);
    }
    if (dPdu) {
        *dPdu = GfVec3f(sampledDu[0], sampledDu[1], sampledDu[2]);
    }
    if (dPdv) {
        *dPdv = GfVec3f(sampledDv[0], sampledDv[1], sampledDv[2]);
    }
}

/// Try to compute a smooth limit-surface normal for a subdivision hit.
static bool
_TryComputeSubdivLimitNormal(
    RTCScene rootScene,
    unsigned int geomID,
    unsigned int primID,
    float u,
    float v,
    GfVec3f* outNormal)
{
    GfVec3f posValue;
    GfVec3f dPdu(0.0f);
    GfVec3f dPdv(0.0f);
    _InterpolateSubdivPosition(
        rtcGetGeometry(rootScene, geomID),
        primID, u, v, &posValue, &dPdu, &dPdv);
    (void)posValue;

    GfVec3f limitNormal;
    if (!_TryBuildSurfaceNormal(dPdu, dPdv, &limitNormal)) {
        return false;
    }

    *outNormal = limitNormal;
    return true;
}

GfVec3f
ty::ResolveObjectSpaceNormal(
    ty::PrototypeContext const* prototypeContext,
    RTCScene rootScene,
    unsigned int geomID,
    RTCRayHit const& rayHit,
    ty::DisplacedSubdivFrame* outDisplacedFrame)
{
    if (outDisplacedFrame) {
        *outDisplacedFrame = ty::DisplacedSubdivFrame{};
    }
    GfVec3f normal = prototypeContext->orientationSign * GfVec3f(
        rayHit.hit.Ng_x, rayHit.hit.Ng_y, rayHit.hit.Ng_z);

    // rtcInterpolate ignores displacement, so reconstruct the differential
    // frame of P + D*N explicitly. Embree's hit Ng remains the true facet
    // orientation and is the fallback for invalid graph/patch evaluations.
    if (prototypeContext->displaced) {
        ty::DisplacedSubdivFrame displacedFrame;
        if (ty::ComputeDisplacedSubdivFrame(
                rtcGetGeometry(rootScene, geomID),
                prototypeContext,
                rayHit.hit.primID,
                rayHit.hit.u,
                rayHit.hit.v,
                &displacedFrame)) {
            if (GfDot(displacedFrame.normal, normal) < 0.0f) {
                displacedFrame.normal = -displacedFrame.normal;
            }
            if (outDisplacedFrame) {
                *outDisplacedFrame = displacedFrame;
            }
            return displacedFrame.normal;
        }
        return normal;
    }

    auto it = prototypeContext->primvarMap.find(HdTokens->normals);
    if (it != prototypeContext->primvarMap.end()) {
        GfVec3f authoredNormal;
        if (it->second->Sample(
                rayHit.hit.primID, rayHit.hit.u, rayHit.hit.v,
                &authoredNormal) &&
            ty::TryNormalizeDirection(authoredNormal, &normal)) {
            return normal;
        }
    }

    if (prototypeContext->refined) {
        GfVec3f limitNormal;
        if (_TryComputeSubdivLimitNormal(
                rootScene,
                geomID,
                rayHit.hit.primID,
                rayHit.hit.u,
                rayHit.hit.v,
                &limitNormal)) {
            return prototypeContext->orientationSign * limitNormal;
        }
    }

    return normal;
}

void
ty::ComputeTriangleSurfaceDerivatives(
    ty::PrototypeContext const* prototypeContext,
    unsigned int primID,
    float u,
    float v,
    GfVec3f const& normal,
    GfVec3f* outDPdu, GfVec3f* outDPdv,
    GfVec3f* outDndu, GfVec3f* outDndv)
{
    bool haveCachedDerivatives = false;
    {
        if (prototypeContext->triangleDPdu && prototypeContext->triangleDPdv) {
            const VtVec3fArray& cachedDPdu = *prototypeContext->triangleDPdu;
            const VtVec3fArray& cachedDPdv = *prototypeContext->triangleDPdv;
            if (primID < cachedDPdu.size() && primID < cachedDPdv.size()) {
                *outDPdu = cachedDPdu[primID];
                *outDPdv = cachedDPdv[primID];
                haveCachedDerivatives = true;
            }
        }
    }

    if (!haveCachedDerivatives) {
        GfBuildOrthonormalFrame(normal, outDPdu, outDPdv);
    }

    // Try to get st vertices for normal derivatives.
    GfVec2f st[3];
    bool haveSt = false;
    {
        auto it = prototypeContext->primvarMap.find(_tokensSt);
        if (it != prototypeContext->primvarMap.end()) {
            auto* vtxSampler =
                dynamic_cast<ty::TriangleVertexSampler*>(
                    it->second.get());
            if (vtxSampler) {
                haveSt = vtxSampler->SampleVertices(
                    primID, &st[0], &st[1], &st[2]);
            }
            if (!haveSt) {
                auto* fvSampler =
                    dynamic_cast<ty::TriangleFaceVaryingSampler*>(
                        it->second.get());
                if (fvSampler) {
                    haveSt = fvSampler->SampleVertices(
                        primID, &st[0], &st[1], &st[2]);
                }
            }
        }
    }

    // Normal derivatives let specular continuations propagate ray-direction
    // differentials.
    GfVec3f N[3];
    bool haveNormals = false;
    {
        auto it = prototypeContext->primvarMap.find(HdTokens->normals);
        if (it != prototypeContext->primvarMap.end()) {
            auto* vtxSampler =
                dynamic_cast<ty::TriangleVertexSampler*>(
                    it->second.get());
            if (vtxSampler) {
                haveNormals = vtxSampler->SampleVertices(
                    primID, &N[0], &N[1], &N[2]);
            }
            if (!haveNormals) {
                auto* fvSampler =
                    dynamic_cast<ty::TriangleFaceVaryingSampler*>(
                        it->second.get());
                if (fvSampler) {
                    haveNormals = fvSampler->SampleVertices(
                        primID, &N[0], &N[1], &N[2]);
                }
            }
        }
    }

    if (haveNormals) {
        const GfVec3f dN1 = N[1] - N[0];
        const GfVec3f dN2 = N[2] - N[0];
        const GfVec3f sampledNormal = N[0] + u * dN1 + v * dN2;
        if (haveSt) {
            GfVec2f dst1 = st[1] - st[0];
            GfVec2f dst2 = st[2] - st[0];
            float det = ty::DifferenceOfProducts(
                dst1[0], dst2[1], dst1[1], dst2[0]);
            if (std::abs(det) > 1e-9f) {
                float invDet = 1.0f / det;
                *outDndu = ( dst2[1] * dN1 - dst1[1] * dN2) * invDet;
                *outDndv = (-dst2[0] * dN1 + dst1[0] * dN2) * invDet;
            } else {
                *outDndu = dN1;
                *outDndv = dN2;
            }
        } else {
            *outDndu = dN1;
            *outDndv = dN2;
        }

        // Primvar interpolation produces an unnormalized vector. Differentiate
        // its normalization and preserve any face-forward orientation applied
        // to the shading normal by the caller.
        const float sampledLengthSquared = sampledNormal.GetLengthSq();
        if (ty::IsFinite(sampledNormal) &&
            std::isfinite(sampledLengthSquared) &&
            sampledLengthSquared > 1.0e-18f) {
            const float sampledLength = std::sqrt(sampledLengthSquared);
            const GfVec3f sampledUnitNormal =
                sampledNormal / sampledLength;
            *outDndu =
                (*outDndu - sampledUnitNormal *
                    GfDot(sampledUnitNormal, *outDndu)) /
                sampledLength;
            *outDndv =
                (*outDndv - sampledUnitNormal *
                    GfDot(sampledUnitNormal, *outDndv)) /
                sampledLength;
            if (GfDot(sampledUnitNormal, normal) < 0.0f) {
                *outDndu = -*outDndu;
                *outDndv = -*outDndv;
            }
        } else {
            *outDndu = GfVec3f(0.0f);
            *outDndv = GfVec3f(0.0f);
        }
    } else {
        *outDndu = GfVec3f(0.0f);
        *outDndv = GfVec3f(0.0f);
    }
}

void
ty::ComputeSubdivSurfaceDerivatives(
    ty::PrototypeContext const* prototypeContext,
    RTCScene rootScene,
    unsigned int geomID,
    unsigned int primID, float u, float v,
    GfVec3f const& normal,
    GfVec3f* outDPdu, GfVec3f* outDPdv,
    GfVec3f* outDndu, GfVec3f* outDndv,
    ty::DisplacedSubdivFrame const* displacedFrame)
{
    RTCGeometry const geometry = rtcGetGeometry(rootScene, geomID);

    // Position derivatives. Displaced subdivision needs the derivatives
    // of P + D*N; rtcInterpolate itself intentionally returns only the
    // undisplaced limit surface.
    bool havePositionDerivs = false;
    if (prototypeContext->displaced) {
        if (displacedFrame && displacedFrame->valid) {
            *outDPdu = displacedFrame->dPdu;
            *outDPdv = displacedFrame->dPdv;
            havePositionDerivs = true;
        } else {
            GfVec3f displacedNormal;
            havePositionDerivs = ty::ComputeDisplacedSubdivFrame(
                geometry,
                prototypeContext,
                primID,
                u,
                v,
                &displacedNormal,
                outDPdu,
                outDPdv);
        }
    }
    if (!havePositionDerivs) {
        GfVec3f posVal;
        _InterpolateSubdivPosition(
            geometry,
            primID, u, v, &posVal, outDPdu, outDPdv);
        havePositionDerivs = true;
    }

    if (!havePositionDerivs) {
        GfBuildOrthonormalFrame(normal, outDPdu, outDPdv);
        *outDndu = GfVec3f(0.0f);
        *outDndv = GfVec3f(0.0f);
        return;
    }

    // If st available, transform from parametric to st space.
    ty::SubdivTexcoordJacobian stJacobian;
    auto const stIt = prototypeContext->primvarMap.find(_tokensSt);
    if (stIt != prototypeContext->primvarMap.end()) {
        stJacobian = ty::ComputeSubdivTexcoordJacobian(
            stIt->second.get(), primID, u, v);
    }
    if (stJacobian.valid) {
        const GfVec3f dPduParam = *outDPdu;
        const GfVec3f dPdvParam = *outDPdv;
        // Chain rule through the inverse st Jacobian.
        *outDPdu =
            stJacobian.duDs * dPduParam +
            stJacobian.dvDs * dPdvParam;
        *outDPdv =
            stJacobian.duDt * dPduParam +
            stJacobian.dvDt * dPdvParam;
    }

    // Degenerate check
    GfVec3f derivativeNormal;
    if (!_TryBuildSurfaceNormal(
            *outDPdu, *outDPdv, &derivativeNormal)) {
        GfBuildOrthonormalFrame(normal, outDPdu, outDPdv);
    }

    // Normal derivatives let specular continuations propagate ray-direction
    // differentials.
    *outDndu = GfVec3f(0.0f);
    *outDndv = GfVec3f(0.0f);
    // Computing derivatives of the displaced normal itself requires another
    // finite-difference ring around the three displacement probes. Zero is a
    // conservative value for ray differentials and avoids reusing unrelated
    // authored base-normal derivatives on displaced geometry.
    if (prototypeContext->displaced) {
        return;
    }
    {
        auto it = prototypeContext->primvarMap.find(HdTokens->normals);
        if (it != prototypeContext->primvarMap.end()) {
            GfVec3f sampledNormal(0.0f);
            bool haveNormalDerivatives = false;
            auto* vertexSampler =
                dynamic_cast<ty::SubdivVertexSampler*>(
                    it->second.get());
            auto* varyingSampler =
                dynamic_cast<ty::SubdivVaryingSampler*>(
                    it->second.get());
            auto* fvarSampler =
                dynamic_cast<ty::SubdivFaceVaryingSampler*>(
                    it->second.get());
            if (vertexSampler) {
                haveNormalDerivatives =
                    vertexSampler->SampleWithDerivatives(
                        primID, u, v, &sampledNormal,
                        outDndu, outDndv);
            } else if (varyingSampler) {
                haveNormalDerivatives =
                    varyingSampler->SampleWithDerivatives(
                        primID, u, v, &sampledNormal,
                        outDndu, outDndv);
            } else if (fvarSampler) {
                haveNormalDerivatives =
                    fvarSampler->SampleWithDerivatives(
                        primID, u, v, &sampledNormal,
                        outDndu, outDndv);
            }

            if (!haveNormalDerivatives) {
                *outDndu = GfVec3f(0.0f);
                *outDndv = GfVec3f(0.0f);
                return;
            }

            if (stJacobian.valid) {
                const GfVec3f dNduParam = *outDndu;
                const GfVec3f dNdvParam = *outDndv;
                *outDndu =
                    stJacobian.duDs * dNduParam +
                    stJacobian.dvDs * dNdvParam;
                *outDndv =
                    stJacobian.duDt * dNduParam +
                    stJacobian.dvDt * dNdvParam;
            }

            const float sampledLengthSquared = sampledNormal.GetLengthSq();
            if (ty::IsFinite(sampledNormal) &&
                std::isfinite(sampledLengthSquared) &&
                sampledLengthSquared > 1.0e-18f) {
                const float sampledLength = std::sqrt(sampledLengthSquared);
                const GfVec3f sampledUnitNormal =
                    sampledNormal / sampledLength;
                *outDndu =
                    (*outDndu - sampledUnitNormal *
                        GfDot(sampledUnitNormal, *outDndu)) /
                    sampledLength;
                *outDndv =
                    (*outDndv - sampledUnitNormal *
                        GfDot(sampledUnitNormal, *outDndv)) /
                    sampledLength;
                if (GfDot(sampledUnitNormal, normal) < 0.0f) {
                    *outDndu = -*outDndu;
                    *outDndv = -*outDndv;
                }
            } else {
                *outDndu = GfVec3f(0.0f);
                *outDndv = GfVec3f(0.0f);
            }
        }
    }
}

bool
ty::TryComputeDisplacedSubdivNormalDerivativesToWorld(
    ty::PrototypeContext const* prototypeContext,
    ty::InstanceContext const* instanceContext,
    RTCScene rootScene,
    unsigned int geomID,
    ty::DisplacedSubdivFrame const& frame,
    GfVec3f const& faceForwardedWorldNormal,
    GfVec3f* outDndu,
    GfVec3f* outDndv)
{
    if (!prototypeContext || !rootScene || !frame.valid ||
        !outDndu || !outDndv) {
        return false;
    }

    GfVec3f objectDndu;
    GfVec3f objectDndv;
    if (!ty::ComputeDisplacedSubdivNormalDerivatives(
            rtcGetGeometry(rootScene, geomID),
            prototypeContext,
            frame,
            &objectDndu,
            &objectDndv)) {
        return false;
    }

    auto const stIt = prototypeContext->primvarMap.find(_tokensSt);
    if (stIt != prototypeContext->primvarMap.end()) {
        const ty::SubdivTexcoordJacobian stJacobian =
            ty::ComputeSubdivTexcoordJacobian(
                stIt->second.get(), frame.primID, frame.u, frame.v);
        if (stJacobian.valid) {
            const GfVec3f dNduParam = objectDndu;
            const GfVec3f dNdvParam = objectDndv;
            objectDndu =
                stJacobian.duDs * dNduParam +
                stJacobian.dvDs * dNdvParam;
            objectDndv =
                stJacobian.duDt * dNduParam +
                stJacobian.dvDt * dNdvParam;
        }
    }

    GfVec3f objectNormal = frame.normal;
    const GfVec3f frameWorldNormal =
        ty::TransformNormalToWorld(instanceContext, objectNormal);
    if (GfDot(frameWorldNormal, faceForwardedWorldNormal) < 0.0f) {
        objectNormal = -objectNormal;
        objectDndu = -objectDndu;
        objectDndv = -objectDndv;
    }

    const GfVec3f worldDndu = ty::TransformNormalDerivativeToWorld(
        instanceContext, objectNormal, objectDndu);
    const GfVec3f worldDndv = ty::TransformNormalDerivativeToWorld(
        instanceContext, objectNormal, objectDndv);
    if (!ty::IsFinite(worldDndu) || !ty::IsFinite(worldDndv)) {
        return false;
    }

    *outDndu = worldDndu;
    *outDndv = worldDndv;
    return true;
}

PXR_NAMESPACE_CLOSE_SCOPE
