//
// Copyright 2018 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/imaging/plugin/hdEmbree/renderer.h"

#include "pxr/imaging/plugin/hdEmbree/config.h"
#include "pxr/imaging/plugin/hdEmbree/renderDelegate.h"
#include "pxr/imaging/plugin/hdEmbree/light.h"
#include "pxr/imaging/plugin/hdEmbree/lightSamplers.h"
#include "pxr/imaging/plugin/hdEmbree/material.h"
#include "pxr/imaging/plugin/hdEmbree/mesh.h"
#include "pxr/imaging/plugin/hdEmbree/renderBuffer.h"
#include "pxr/imaging/plugin/hdEmbree/MaterialXCpp/materials/bsdf.h"

#include "pxr/imaging/hd/perfLog.h"

#include "pxr/base/gf/matrix3f.h"
#include "pxr/base/tf/hash.h"
#include "pxr/base/work/loops.h"
#include "pxr/base/work/threadLimits.h"

#include "pxr/base/work/workTBB/tbb_version.h"

#include "pxr/base/tf/hash.h"

#include <embree4/rtcore_common.h>
#include <embree4/rtcore_geometry.h>
#include <embree4/rtcore_scene.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdint.h>
#include <thread>

// -------------------------------------------------------------------------
// Old TBB workaround - we plan to remove this once OpenUSD adopts
// oneTBB as a min spec. This applies the "Work" thread limit to the
// render thread if "Work" is using old TBB, but won't affect other "Work"
// implementations.  Note that it may affect Embree TBB usage as well.
//
// Note: The TBB version macro is located in different headers in legacy TBB.
// -------------------------------------------------------------------------
#if TBB_INTERFACE_VERSION_MAJOR < 12

#include <optional>
#include <tbb/task_scheduler_init.h>

namespace {

PXR_NAMESPACE_USING_DIRECTIVE

// Make the calling context respect PXR_WORK_THREAD_LIMIT, if run from a thread
// other than the main thread (ie, the renderThread)
class _ScopedThreadScheduler {
public:
    _ScopedThreadScheduler() {
        auto limit = WorkGetConcurrencyLimitSetting();
        if (limit != 0) {
            _tbbTaskSchedInit.emplace(limit);
        }
    }

    std::optional<tbb::task_scheduler_init> _tbbTaskSchedInit;
};

}  // anonymous namespace

#endif  // TBB_INTERFACE_VERSION_MAJOR < 12

namespace {

PXR_NAMESPACE_USING_DIRECTIVE

// -------------------------------------------------------------------------
// Constants
// -------------------------------------------------------------------------

template <typename T>
constexpr T _pi = static_cast<T>(M_PI);

constexpr float _rayHitContinueBias = 0.001f;
constexpr float _minLuminanceCutoff = 1e-9f;
static const TfToken _tokensTangent("tangent");
static const TfToken _tokensBitangent("bitangent");
static const TfToken _tokensComputedTangent("hdEmbreeComputedTangent");
static const TfToken _tokensComputedBitangent("hdEmbreeComputedBitangent");
static const TfToken _tokensSt("st");

// -------------------------------------------------------------------------
// General Ray Utilities
// -------------------------------------------------------------------------

inline GfVec3f
_CalculateHitPosition(RTCRayHit const& rayHit)
{
    return GfVec3f(rayHit.ray.org_x + rayHit.ray.tfar * rayHit.ray.dir_x,
                   rayHit.ray.org_y + rayHit.ray.tfar * rayHit.ray.dir_y,
                   rayHit.ray.org_z + rayHit.ray.tfar * rayHit.ray.dir_z);
}

// Dot product, but set to 0 if less than 0 - ie, 0 for backward-facing rays
inline float
_DotZeroClip(GfVec3f const& a, GfVec3f const& b)
{
    return std::max(0.0f, GfDot(a, b));
}

}  // anonymous namespace

PXR_NAMESPACE_OPEN_SCOPE

// Conversion helpers between GfVec3f and mxcpp::Vec3f (Imath::V3f).
// GfVec3f in this build does not have implicit Imath conversion.
namespace {

/// Numerically stable computation of a*b - c*d using Kahan compensation.
inline float
_DifferenceOfProducts(float a, float b, float c, float d)
{
    float cd = c * d;
    float err = std::fma(-c, d, cd);
    float dop = std::fma(a, b, -cd);
    return dop + err;
}

inline GfVec3f
_ToGf(const mxcpp::Vec3f& v)
{
    return GfVec3f(v[0], v[1], v[2]);
}

inline mxcpp::Vec3f
_ToMx(const GfVec3f& v)
{
    return mxcpp::Vec3f(v[0], v[1], v[2]);
}

inline mxcpp::Vec2f
_ToMx(const GfVec2f& v)
{
    return mxcpp::Vec2f(v[0], v[1]);
}

inline mxcpp::Mat4f
_ToMx(const GfMatrix4f& m)
{
    mxcpp::Mat4f result;
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            result[row][col] = m[row][col];
        }
    }
    return result;
}

// Callback data for geompropvalue node — holds references needed to
// sample an arbitrary primvar at a ray hit point.
struct _GeomPropCallbackData {
    const TfHashMap<TfToken, HdEmbreePrimvarSampler*,
                    TfToken::HashFunctor>* primvarMap;
    unsigned int primID;
    float u, v;
};

// Callback function for geompropvalue: sample a primvar by name,
// trying common types in order.  Returns empty Value on failure.
// Note: HdEmbreePrimvarSampler::Sample() checks HdTupleType internally
// and returns false on type mismatch, so the widest-first probing is safe.
static mxcpp::Value
_SampleGeomProp(const void* userData, const std::string& name)
{
    auto* data = static_cast<const _GeomPropCallbackData*>(userData);
    auto it = data->primvarMap->find(TfToken(name));
    if (it == data->primvarMap->end()) {
        return mxcpp::Value();
    }

    auto* sampler = it->second;

    // Try types from widest to narrowest.
    {
        GfVec4f val;
        if (sampler->Sample(data->primID, data->u, data->v, &val)) {
            return mxcpp::Value(mxcpp::Vec4f(val[0], val[1], val[2], val[3]));
        }
    }
    {
        GfVec3f val;
        if (sampler->Sample(data->primID, data->u, data->v, &val)) {
            return mxcpp::Value(mxcpp::Vec3f(val[0], val[1], val[2]));
        }
    }
    {
        GfVec2f val;
        if (sampler->Sample(data->primID, data->u, data->v, &val)) {
            return mxcpp::Value(mxcpp::Vec2f(val[0], val[1]));
        }
    }
    {
        float val;
        if (sampler->Sample(data->primID, data->u, data->v, &val)) {
            return mxcpp::Value(val);
        }
    }
    {
        int val;
        if (sampler->Sample(data->primID, data->u, data->v, &val)) {
            return mxcpp::Value(val);
        }
    }
    {
        bool val;
        if (sampler->Sample(data->primID, data->u, data->v, &val)) {
            return mxcpp::Value(val);
        }
    }

    return mxcpp::Value();
}

/// Returns true if the "points" primvar uses subdivision sampling.
static bool
_IsSubdivMesh(HdEmbreePrototypeContext const* prototypeContext)
{
    if (auto* mesh = dynamic_cast<HdEmbreeMesh*>(prototypeContext->rprim)) {
        return mesh->EmbreeMeshIsRefined();
    }
    return false;
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
    rtcInterpolate1(
        rtcGetGeometry(rootScene, geomID),
        primID, u, v,
        RTC_BUFFER_TYPE_VERTEX, 0,
        reinterpret_cast<float*>(&posValue),
        reinterpret_cast<float*>(&dPdu),
        reinterpret_cast<float*>(&dPdv),
        3);
    (void)posValue;

    GfVec3f limitNormal = GfCross(dPdu, dPdv);
    if (limitNormal.GetLengthSq() <= 1e-18f) {
        return false;
    }

    limitNormal.Normalize();
    *outNormal = limitNormal;
    return true;
}

/// Resolve the object-space shading normal for a hit.
static GfVec3f
_ResolveObjectSpaceNormal(
    HdEmbreePrototypeContext const* prototypeContext,
    RTCScene rootScene,
    unsigned int geomID,
    RTCRayHit const& rayHit)
{
    GfVec3f normal(rayHit.hit.Ng_x, rayHit.hit.Ng_y, rayHit.hit.Ng_z);

    auto it = prototypeContext->primvarMap.find(HdTokens->normals);
    if (it != prototypeContext->primvarMap.end() &&
        it->second->Sample(rayHit.hit.primID, rayHit.hit.u, rayHit.hit.v,
                           &normal)) {
        return normal;
    }

    if (_IsSubdivMesh(prototypeContext)) {
        GfVec3f limitNormal;
        if (_TryComputeSubdivLimitNormal(
                rootScene,
                geomID,
                rayHit.hit.primID,
                rayHit.hit.u,
                rayHit.hit.v,
                &limitNormal)) {
            return limitNormal;
        }
    }

    return normal;
}

/// Compute dPdu/dPdv for a triangle mesh hit.
/// If st primvar is available, computes dP/ds, dP/dt via Jacobian inverse.
/// If not, uses barycentric edge vectors (P1-P0, P2-P0).
/// Falls back to GfBuildOrthonormalFrame on degenerate cases.
static void
_ComputeTriangleSurfaceDerivatives(
    HdEmbreePrototypeContext const* prototypeContext,
    unsigned int primID,
    GfVec3f const& normal,
    GfVec3f* outDPdu, GfVec3f* outDPdv,
    GfVec3f* outDndu, GfVec3f* outDndv)
{
    bool haveCachedDerivatives = false;
    {
        HdEmbreeMesh* mesh =
            dynamic_cast<HdEmbreeMesh*>(prototypeContext->rprim);
        if (mesh) {
            const VtVec3fArray& cachedDPdu = mesh->GetTriangleDPdu();
            const VtVec3fArray& cachedDPdv = mesh->GetTriangleDPdv();
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
                dynamic_cast<HdEmbreeTriangleVertexSampler*>(it->second);
            if (vtxSampler) {
                haveSt = vtxSampler->SampleVertices(
                    primID, &st[0], &st[1], &st[2]);
            }
            if (!haveSt) {
                auto* fvSampler =
                    dynamic_cast<HdEmbreeTriangleFaceVaryingSampler*>(
                        it->second);
                if (fvSampler) {
                    haveSt = fvSampler->SampleVertices(
                        primID, &st[0], &st[1], &st[2]);
                }
            }
        }
    }

    // 3. Normal derivatives (dndu, dndv)
    GfVec3f N[3];
    bool haveNormals = false;
    {
        auto it = prototypeContext->primvarMap.find(HdTokens->normals);
        if (it != prototypeContext->primvarMap.end()) {
            auto* vtxSampler =
                dynamic_cast<HdEmbreeTriangleVertexSampler*>(it->second);
            if (vtxSampler) {
                haveNormals = vtxSampler->SampleVertices(
                    primID, &N[0], &N[1], &N[2]);
            }
            if (!haveNormals) {
                auto* fvSampler =
                    dynamic_cast<HdEmbreeTriangleFaceVaryingSampler*>(
                        it->second);
                if (fvSampler) {
                    haveNormals = fvSampler->SampleVertices(
                        primID, &N[0], &N[1], &N[2]);
                }
            }
        }
    }

    if (haveNormals) {
        GfVec3f dN1 = N[1] - N[0];
        GfVec3f dN2 = N[2] - N[0];
        if (haveSt) {
            GfVec2f dst1 = st[1] - st[0];
            GfVec2f dst2 = st[2] - st[0];
            float det = _DifferenceOfProducts(
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
    } else {
        *outDndu = GfVec3f(0.0f);
        *outDndv = GfVec3f(0.0f);
    }
}

/// Compute dPdu/dPdv for a subdivision surface hit using rtcInterpolate1.
static void
_ComputeSubdivSurfaceDerivatives(
    HdEmbreePrototypeContext const* prototypeContext,
    RTCScene rootScene,
    unsigned int geomID,
    unsigned int primID, float u, float v,
    GfVec3f const& normal,
    GfVec3f* outDPdu, GfVec3f* outDPdv,
    GfVec3f* outDndu, GfVec3f* outDndv)
{
    // 1. Position derivatives via rtcInterpolate1.
    //    Vertex positions are in Embree's primary vertex buffer
    //    (RTC_BUFFER_TYPE_VERTEX), NOT in a user vertex attribute buffer.
    //    We must call rtcInterpolate1 directly.
    bool havePositionDerivs = false;
    {
        GfVec3f posVal;
        rtcInterpolate1(
            rtcGetGeometry(rootScene, geomID),
            primID, u, v,
            RTC_BUFFER_TYPE_VERTEX, 0,
            reinterpret_cast<float*>(&posVal),
            reinterpret_cast<float*>(outDPdu),
            reinterpret_cast<float*>(outDPdv),
            3);
        havePositionDerivs = true;
    }

    if (!havePositionDerivs) {
        GfBuildOrthonormalFrame(normal, outDPdu, outDPdv);
        *outDndu = GfVec3f(0.0f);
        *outDndv = GfVec3f(0.0f);
        return;
    }

    // 2. If st available, transform from parametric to st space.
    //    Handle both GfVec2f and GfVec3f st buffers.
    //    Support both vertex and face-varying interpolation modes.
    {
        auto it = prototypeContext->primvarMap.find(_tokensSt);
        if (it != prototypeContext->primvarMap.end()) {
            // Helper: try SampleWithDerivatives on a sampler, handling
            // both GfVec2f and GfVec3f st buffers.
            const auto tryStDerivatives =
                [&](auto* sampler, GfVec2f* dStdu, GfVec2f* dStdv) -> bool {
                if (!sampler) return false;
                // Try GfVec2f first
                {
                    GfVec2f stVal, dStdu2, dStdv2;
                    if (sampler->SampleWithDerivatives(
                            primID, u, v, &stVal, &dStdu2, &dStdv2)) {
                        *dStdu = dStdu2;
                        *dStdv = dStdv2;
                        return true;
                    }
                }
                // Fallback to GfVec3f (ignore z component)
                {
                    GfVec3f stVal3, dStdu3, dStdv3;
                    if (sampler->SampleWithDerivatives(
                            primID, u, v, &stVal3, &dStdu3, &dStdv3)) {
                        *dStdu = GfVec2f(dStdu3[0], dStdu3[1]);
                        *dStdv = GfVec2f(dStdv3[0], dStdv3[1]);
                        return true;
                    }
                }
                return false;
            };

            GfVec2f dStdu(0.0f), dStdv(0.0f);
            bool haveSt = false;

            // Try vertex-interpolated st sampler
            auto* vertexSampler =
                dynamic_cast<HdEmbreeSubdivVertexSampler*>(it->second);
            haveSt = tryStDerivatives(vertexSampler, &dStdu, &dStdv);

            // Try varying-interpolated st sampler
            if (!haveSt) {
                auto* varyingSampler =
                    dynamic_cast<HdEmbreeSubdivVaryingSampler*>(it->second);
                haveSt = tryStDerivatives(varyingSampler, &dStdu, &dStdv);
            }

            // Try face-varying st sampler
            if (!haveSt) {
                auto* fvarSampler =
                    dynamic_cast<HdEmbreeSubdivFaceVaryingSampler*>(
                        it->second);
                haveSt = tryStDerivatives(fvarSampler, &dStdu, &dStdv);
            }

            if (haveSt) {
                float det = _DifferenceOfProducts(
                    dStdu[0], dStdv[1], dStdu[1], dStdv[0]);
                if (std::abs(det) > 1e-9f) {
                    float invDet = 1.0f / det;
                    GfVec3f dPdu_param = *outDPdu;
                    GfVec3f dPdv_param = *outDPdv;
                    // Chain rule: dP/ds = dP/du * du/ds + dP/dv * dv/ds
                    // Inverse Jacobian: [du/ds, dv/ds] = inv([ds/du, dt/du; ds/dv, dt/dv])
                    *outDPdu = ( dStdv[1] * dPdu_param
                               - dStdu[1] * dPdv_param) * invDet;
                    *outDPdv = (-dStdv[0] * dPdu_param
                               + dStdu[0] * dPdv_param) * invDet;
                }
            }
        }
    }

    // Degenerate check
    if (GfCross(*outDPdu, *outDPdv).GetLengthSq() < 1e-18f) {
        GfBuildOrthonormalFrame(normal, outDPdu, outDPdv);
    }

    // 3. Normal derivatives
    //    Support both vertex and face-varying interpolation modes.
    *outDndu = GfVec3f(0.0f);
    *outDndv = GfVec3f(0.0f);
    {
        auto it = prototypeContext->primvarMap.find(HdTokens->normals);
        if (it != prototypeContext->primvarMap.end()) {
            auto* vertexSampler =
                dynamic_cast<HdEmbreeSubdivVertexSampler*>(it->second);
            auto* varyingSampler =
                dynamic_cast<HdEmbreeSubdivVaryingSampler*>(it->second);
            auto* fvarSampler =
                dynamic_cast<HdEmbreeSubdivFaceVaryingSampler*>(it->second);
            if (vertexSampler) {
                GfVec3f nVal;
                vertexSampler->SampleWithDerivatives(
                    primID, u, v, &nVal, outDndu, outDndv);
            } else if (varyingSampler) {
                GfVec3f nVal;
                varyingSampler->SampleWithDerivatives(
                    primID, u, v, &nVal, outDndu, outDndv);
            } else if (fvarSampler) {
                GfVec3f nVal;
                fvarSampler->SampleWithDerivatives(
                    primID, u, v, &nVal, outDndu, outDndv);
            }
        }
    }
}

/// Compute screen-space derivatives from ray differentials and surface
/// derivatives.  When ray differentials are unavailable (non-specular
/// bounces), uses a camera-projection-based fallback approximation.
/// Populates dPdx, dPdy, dudx, dvdx, dudy, dvdy on the ShadingContext.
///
/// Note: viewMatrix, inverseProjMatrix, imageWidth, imageHeight, and
/// samplesPerPixel are only used for the fallback path. They come from
/// HdEmbreeRenderer member variables, accessed by the caller.
static void
_ComputeScreenSpaceDerivatives(
    HdEmbreeRayDifferential const& rayDiff,
    GfVec3f const& hitPos,
    GfVec3f const& normal,
    GfVec3f const& dPdu,
    GfVec3f const& dPdv,
    GfMatrix4d const& viewMatrix,
    GfMatrix4d const& inverseProjMatrix,
    float imageWidth,
    float imageHeight,
    int samplesPerPixel,
    mxcpp::ShadingContext& ctx)
{
    GfVec3f dPdx(0.0f);
    GfVec3f dPdy(0.0f);

    if (rayDiff.hasDifferentials) {
        // Intersect differential rays with the tangent plane at hitPos.
        float d = -GfDot(normal, hitPos);
        float rxDotN = GfDot(normal, rayDiff.rxDirection);
        if (std::abs(rxDotN) > 1e-10f) {
            float tx = -(GfDot(normal, rayDiff.rxOrigin) + d) / rxDotN;
            GfVec3f px = rayDiff.rxOrigin + tx * rayDiff.rxDirection;
            dPdx = px - hitPos;
        }
        float ryDotN = GfDot(normal, rayDiff.ryDirection);
        if (std::abs(ryDotN) > 1e-10f) {
            float ty = -(GfDot(normal, rayDiff.ryOrigin) + d) / ryDotN;
            GfVec3f py = rayDiff.ryOrigin + ty * rayDiff.ryDirection;
            dPdy = py - hitPos;
        }
    } else {
        // Fallback: approximate dPdx/dPdy from camera projection.
        GfVec3f hitCamera = GfVec3f(viewMatrix.Transform(hitPos));
        float dist = hitCamera.GetLength();
        if (dist > 1e-6f) {
            GfVec3f ndcCenter(0.0f, 0.0f, -1.0f);
            GfVec3f ndcDx(2.0f / imageWidth, 0.0f, -1.0f);
            GfVec3f ndcDy(0.0f, 2.0f / imageHeight, -1.0f);
            GfVec3f camCenter = GfVec3f(inverseProjMatrix.Transform(ndcCenter));
            GfVec3f camDx = GfVec3f(inverseProjMatrix.Transform(ndcDx));
            GfVec3f camDy = GfVec3f(inverseProjMatrix.Transform(ndcDy));
            float pixelScaleX = (camDx - camCenter).GetLength() * dist;
            float pixelScaleY = (camDy - camCenter).GetLength() * dist;

            float sppScale = (samplesPerPixel > 1)
                ? 1.0f / std::sqrt(static_cast<float>(samplesPerPixel))
                : 1.0f;
            pixelScaleX *= sppScale;
            pixelScaleY *= sppScale;

            GfVec3f t, b;
            GfBuildOrthonormalFrame(normal, &t, &b);
            dPdx = t * pixelScaleX;
            dPdy = b * pixelScaleY;
        }
    }

    ctx.dPdx = _ToMx(dPdx);
    ctx.dPdy = _ToMx(dPdy);

    // Solve for UV derivatives: A^T A x = A^T b (least squares)
    float ata00 = GfDot(dPdu, dPdu);
    float ata01 = GfDot(dPdu, dPdv);
    float ata11 = GfDot(dPdv, dPdv);
    float detATA = _DifferenceOfProducts(ata00, ata11, ata01, ata01);

    if (std::abs(detATA) > 1e-18f) {
        float invDet = 1.0f / detATA;
        float atb0x = GfDot(dPdu, dPdx);
        float atb1x = GfDot(dPdv, dPdx);
        float atb0y = GfDot(dPdu, dPdy);
        float atb1y = GfDot(dPdv, dPdy);

        ctx.dudx = std::clamp(
            _DifferenceOfProducts(ata11, atb0x, ata01, atb1x) * invDet,
            -1e8f, 1e8f);
        ctx.dvdx = std::clamp(
            _DifferenceOfProducts(ata00, atb1x, ata01, atb0x) * invDet,
            -1e8f, 1e8f);
        ctx.dudy = std::clamp(
            _DifferenceOfProducts(ata11, atb0y, ata01, atb1y) * invDet,
            -1e8f, 1e8f);
        ctx.dvdy = std::clamp(
            _DifferenceOfProducts(ata00, atb1y, ata01, atb0y) * invDet,
            -1e8f, 1e8f);
    }
}

}  // anonymous namespace

HdEmbreeRenderer::HdEmbreeRenderer()
    : _aovBindings()
    , _aovNames()
    , _aovBindingsNeedValidation(false)
    , _aovBindingsValid(false)
    , _width(0)
    , _height(0)
    , _viewMatrix(1.0f) // == identity
    , _projMatrix(1.0f) // == identity
    , _inverseViewMatrix(1.0f) // == identity
    , _inverseProjMatrix(1.0f) // == identity
    , _scene(nullptr)
    , _samplesToConvergence(0)
    , _ambientOcclusionSamples(0)
    , _enableSceneColors(false)
    , _domeLightCameraVisibility(true)
    , _enableLighting(false)
    , _maxBounces(HdEmbreeDefaultMaxBounces)
    , _minBouncesBeforeRR(HdEmbreeDefaultMinBouncesBeforeRR)
    , _samplerSequence(HdEmbreeSamplerSequence::Sobol)
    , _enableAdaptiveSampling(HdEmbreeDefaultEnableAdaptiveSampling)
    , _adaptiveThreshold(HdEmbreeDefaultAdaptiveThreshold)
    , _minSamplesBeforeAdaptive(HdEmbreeDefaultMinSamplesBeforeAdaptive)
    , _lightSamplesPerHit(HdEmbreeDefaultLightSamplesPerHit)
    , _stratifyLightSamples(HdEmbreeDefaultStratifyLightSamples)
    , _showAdaptiveHeatmap(HdEmbreeDefaultShowAdaptiveHeatmap)
    , _usePerChannelVariance(HdEmbreeDefaultUsePerChannelVariance)
    , _fireflyClampThreshold(HdEmbreeDefaultFireflyClampThreshold)
    , _sceneFrame(0.0f)
    , _sceneTime(0.0f)
    , _completedSamples(0)
{
}

HdEmbreeRenderer::~HdEmbreeRenderer() = default;

void
HdEmbreeRenderer::SetScene(RTCScene scene)
{
    _scene = scene;
}

void
HdEmbreeRenderer::SetSamplesToConvergence(int samplesToConvergence)
{
    _samplesToConvergence = samplesToConvergence;
}

void
HdEmbreeRenderer::SetAmbientOcclusionSamples(int ambientOcclusionSamples)
{
    _ambientOcclusionSamples = ambientOcclusionSamples;
}

void
HdEmbreeRenderer::SetEnableSceneColors(bool enableSceneColors)
{
    _enableSceneColors = enableSceneColors;
}

void
HdEmbreeRenderer::SetDomeLightCameraVisibility(bool domeLightCameraVisibility)
{
    _domeLightCameraVisibility = domeLightCameraVisibility;
}

void
HdEmbreeRenderer::SetEnableLighting(bool enableLighting)
{
    _enableLighting = enableLighting;
}

void
HdEmbreeRenderer::SetMaxBounces(int maxBounces)
{
    _maxBounces = maxBounces;
}

void
HdEmbreeRenderer::SetMinBouncesBeforeRR(int minBounces)
{
    _minBouncesBeforeRR = minBounces;
}

void
HdEmbreeRenderer::SetUseSobol(bool useSobol)
{
    _samplerSequence = useSobol
        ? HdEmbreeSamplerSequence::Sobol
        : HdEmbreeSamplerSequence::Random;
}

void
HdEmbreeRenderer::SetSamplerSequence(HdEmbreeSamplerSequence sequence)
{
    _samplerSequence = sequence;
}

void
HdEmbreeRenderer::SetEnableAdaptiveSampling(bool enable)
{
    _enableAdaptiveSampling = enable;
}

void
HdEmbreeRenderer::SetAdaptiveThreshold(float threshold)
{
    _adaptiveThreshold = threshold;
}

void
HdEmbreeRenderer::SetMinSamplesBeforeAdaptive(int minSamples)
{
    _minSamplesBeforeAdaptive = minSamples;
}

void
HdEmbreeRenderer::SetLightSamplesPerHit(int samples)
{
    _lightSamplesPerHit = std::max(1, samples);
}

void
HdEmbreeRenderer::SetStratifyLightSamples(bool stratify)
{
    _stratifyLightSamples = stratify;
}

void
HdEmbreeRenderer::SetShowAdaptiveHeatmap(bool show)
{
    _showAdaptiveHeatmap = show;
}

void
HdEmbreeRenderer::SetUsePerChannelVariance(bool use)
{
    _usePerChannelVariance = use;
}

void
HdEmbreeRenderer::SetFireflyClampThreshold(float threshold)
{
    _fireflyClampThreshold = threshold;
}

void
HdEmbreeRenderer::SetRandomNumberSeed(int randomNumberSeed)
{
    _randomNumberSeed = randomNumberSeed;
}

void
HdEmbreeRenderer::SetDataWindow(const GfRect2i& dataWindow)
{
    _dataWindow = dataWindow;

    // Here for clients that do not use camera framing but the
    // viewport.
    //
    // Re-validate the attachments, since attachment viewport and
    // render viewport need to match.
    _aovBindingsNeedValidation = true;
}

void
HdEmbreeRenderer::SetCamera(const GfMatrix4d& viewMatrix,
                            const GfMatrix4d& projMatrix)
{
    _viewMatrix = viewMatrix;
    _projMatrix = projMatrix;
    _inverseViewMatrix = viewMatrix.GetInverse();
    _inverseProjMatrix = projMatrix.GetInverse();
}

void
HdEmbreeRenderer::SetSceneFrameAndTime(float frame, float time)
{
    _sceneFrame = frame;
    _sceneTime = time;
}

void
HdEmbreeRenderer::SetAovBindings(
    HdRenderPassAovBindingVector const& aovBindings)
{
    _aovBindings = aovBindings;
    _aovNames.resize(_aovBindings.size());
    for (size_t i = 0; i < _aovBindings.size(); ++i) {
        _aovNames[i] = HdParsedAovToken(_aovBindings[i].aovName);
    }

    // Re-validate the attachments.
    _aovBindingsNeedValidation = true;
}

void
HdEmbreeRenderer::AddLight(
    SdfPath const& lightPath,
    HdEmbree_Light* light)
{
    ScopedLock lightsWriteLock(_lightsWriteMutex);
    _lightMap[lightPath] = light;

    if (light->IsDome()) {
        _domes.push_back(light);
    }
}

void
HdEmbreeRenderer::RemoveLight(SdfPath const& lightPath, HdEmbree_Light* light)
{
    ScopedLock lightsWriteLock(_lightsWriteMutex);
    _lightMap.erase(lightPath);
    _domes.erase(std::remove_if(_domes.begin(), _domes.end(),
                                [&light](auto& l){ return l == light; }),
                 _domes.end());
}

bool
HdEmbreeRenderer::_ValidateAovBindings()
{
    if (!_aovBindingsNeedValidation) {
        return _aovBindingsValid;
    }

    _aovBindingsNeedValidation = false;
    _aovBindingsValid = true;

    for (size_t i = 0; i < _aovBindings.size(); ++i) {
        // By the time the attachment gets here, there should be a bound
        // output buffer.
        if (_aovBindings[i].renderBuffer == nullptr) {
            TF_WARN("Aov '%s' doesn't have any renderbuffer bound",
                    _aovNames[i].name.GetText());
            _aovBindingsValid = false;
            continue;
        }

        if (_aovNames[i].name != HdAovTokens->color &&
            _aovNames[i].name != HdAovTokens->cameraDepth &&
            _aovNames[i].name != HdAovTokens->depth &&
            _aovNames[i].name != HdAovTokens->primId &&
            _aovNames[i].name != HdAovTokens->instanceId &&
            _aovNames[i].name != HdAovTokens->elementId &&
            _aovNames[i].name != HdAovTokens->Neye &&
            _aovNames[i].name != HdAovTokens->normal &&
            _aovNames[i].name != HdEmbreeAovTokens->adaptiveHeatmap &&
            !_aovNames[i].isPrimvar) {
            TF_WARN("Unsupported attachment with Aov '%s' won't be rendered to",
                    _aovNames[i].name.GetText());
        }

        HdFormat format = _aovBindings[i].renderBuffer->GetFormat();

        // depth is only supported for float32 attachments
        if ((_aovNames[i].name == HdAovTokens->cameraDepth ||
             _aovNames[i].name == HdAovTokens->depth) &&
            format != HdFormatFloat32) {
            TF_WARN("Aov '%s' has unsupported format '%s'",
                    _aovNames[i].name.GetText(),
                    TfEnum::GetName(format).c_str());
            _aovBindingsValid = false;
        }

        // ids are only supported for int32 attachments
        if ((_aovNames[i].name == HdAovTokens->primId ||
             _aovNames[i].name == HdAovTokens->instanceId ||
             _aovNames[i].name == HdAovTokens->elementId) &&
            format != HdFormatInt32) {
            TF_WARN("Aov '%s' has unsupported format '%s'",
                    _aovNames[i].name.GetText(),
                    TfEnum::GetName(format).c_str());
            _aovBindingsValid = false;
        }

        // Normal is only supported for vec3 attachments of float.
        if ((_aovNames[i].name == HdAovTokens->Neye ||
             _aovNames[i].name == HdAovTokens->normal) &&
            format != HdFormatFloat32Vec3) {
            TF_WARN("Aov '%s' has unsupported format '%s'",
                    _aovNames[i].name.GetText(),
                    TfEnum::GetName(format).c_str());
            _aovBindingsValid = false;
        }

        // Primvars support vec3 output (though some channels may not be used).
        if (_aovNames[i].isPrimvar &&
            format != HdFormatFloat32Vec3) {
            TF_WARN("Aov 'primvars:%s' has unsupported format '%s'",
                    _aovNames[i].name.GetText(),
                    TfEnum::GetName(format).c_str());
            _aovBindingsValid = false;
        }

        // color is only supported for vec3/vec4 attachments of float,
        // unorm, or snorm.
        if (_aovNames[i].name == HdAovTokens->color) {
            switch (format) {
                case HdFormatUNorm8Vec4:
                case HdFormatUNorm8Vec3:
                case HdFormatSNorm8Vec4:
                case HdFormatSNorm8Vec3:
                case HdFormatFloat32Vec4:
                case HdFormatFloat32Vec3:
                    break;
                default:
                    TF_WARN("Aov '%s' has unsupported format '%s'",
                        _aovNames[i].name.GetText(),
                        TfEnum::GetName(format).c_str());
                    _aovBindingsValid = false;
                    break;
            }
        }

        // make sure the clear value is reasonable for the format of the
        // attached buffer.
        if (!_aovBindings[i].clearValue.IsEmpty()) {
            HdTupleType clearType =
                HdGetValueTupleType(_aovBindings[i].clearValue);

            // array-valued clear types aren't supported.
            if (clearType.count != 1) {
                TF_WARN("Aov '%s' clear value type '%s' is an array",
                        _aovNames[i].name.GetText(),
                        _aovBindings[i].clearValue.GetTypeName().c_str());
                _aovBindingsValid = false;
            }

            // color only supports float/double vec3/4
            if (_aovNames[i].name == HdAovTokens->color &&
                clearType.type != HdTypeFloatVec3 &&
                clearType.type != HdTypeFloatVec4 &&
                clearType.type != HdTypeDoubleVec3 &&
                clearType.type != HdTypeDoubleVec4) {
                TF_WARN("Aov '%s' clear value type '%s' isn't compatible",
                        _aovNames[i].name.GetText(),
                        _aovBindings[i].clearValue.GetTypeName().c_str());
                _aovBindingsValid = false;
            }

            // only clear float formats with float, int with int, float3 with
            // float3.
            if ((format == HdFormatFloat32 && clearType.type != HdTypeFloat) ||
                (format == HdFormatInt32 && clearType.type != HdTypeInt32) ||
                (format == HdFormatFloat32Vec3 &&
                 clearType.type != HdTypeFloatVec3)) {
                TF_WARN("Aov '%s' clear value type '%s' isn't compatible with"
                        " format %s",
                        _aovNames[i].name.GetText(),
                        _aovBindings[i].clearValue.GetTypeName().c_str(),
                        TfEnum::GetName(format).c_str());
                _aovBindingsValid = false;
            }
        }
    }

    return _aovBindingsValid;
}

/* static */
GfVec4f
HdEmbreeRenderer::_GetClearColor(VtValue const& clearValue)
{
    HdTupleType type = HdGetValueTupleType(clearValue);
    if (type.count != 1) {
        return GfVec4f(0.0f, 0.0f, 0.0f, 1.0f);
    }

    switch(type.type) {
        case HdTypeFloatVec3:
        {
            GfVec3f f =
                *(static_cast<const GfVec3f*>(HdGetValueData(clearValue)));
            return GfVec4f(f[0], f[1], f[2], 1.0f);
        }
        case HdTypeFloatVec4:
        {
            GfVec4f f =
                *(static_cast<const GfVec4f*>(HdGetValueData(clearValue)));
            return f;
        }
        case HdTypeDoubleVec3:
        {
            GfVec3d f =
                *(static_cast<const GfVec3d*>(HdGetValueData(clearValue)));
            return GfVec4f(f[0], f[1], f[2], 1.0f);
        }
        case HdTypeDoubleVec4:
        {
            GfVec4d f =
                *(static_cast<const GfVec4d*>(HdGetValueData(clearValue)));
            return GfVec4f(f);
        }
        default:
            return GfVec4f(0.0f, 0.0f, 0.0f, 1.0f);
    }
}

void
HdEmbreeRenderer::Clear()
{
    if (!_ValidateAovBindings()) {
        return;
    }

    for (size_t i = 0; i < _aovBindings.size(); ++i) {
        if (_aovBindings[i].clearValue.IsEmpty()) {
            continue;
        }

        HdEmbreeRenderBuffer *rb =
            static_cast<HdEmbreeRenderBuffer*>(_aovBindings[i].renderBuffer);

        rb->Map();
        if (_aovNames[i].name == HdAovTokens->color) {
            GfVec4f clearColor = _GetClearColor(_aovBindings[i].clearValue);
            rb->Clear(4, clearColor.data());
        } else if (rb->GetFormat() == HdFormatInt32) {
            int32_t clearValue = _aovBindings[i].clearValue.Get<int32_t>();
            rb->Clear(1, &clearValue);
        } else if (rb->GetFormat() == HdFormatFloat32) {
            float clearValue = _aovBindings[i].clearValue.Get<float>();
            rb->Clear(1, &clearValue);
        } else if (rb->GetFormat() == HdFormatFloat32Vec3) {
            GfVec3f clearValue = _aovBindings[i].clearValue.Get<GfVec3f>();
            rb->Clear(3, clearValue.data());
        } // else, _ValidateAovBindings would have already warned.

        rb->Unmap();
        rb->SetConverged(false);
    }

    // Reset adaptive sampling state.
    std::fill(_pixelMean.begin(), _pixelMean.end(), GfVec3f(0.0f));
    std::fill(_pixelM2.begin(), _pixelM2.end(), GfVec3f(0.0f));
    std::fill(_pixelSampleCount.begin(), _pixelSampleCount.end(), 0);
    std::fill(_pixelConverged.begin(), _pixelConverged.end(), false);
}

void
HdEmbreeRenderer::MarkAovBuffersUnconverged()
{
    for (size_t i = 0; i < _aovBindings.size(); ++i) {
        HdEmbreeRenderBuffer *rb =
            static_cast<HdEmbreeRenderBuffer*>(_aovBindings[i].renderBuffer);
        rb->SetConverged(false);
    }
}

int
HdEmbreeRenderer::GetCompletedSamples() const
{
    return _completedSamples.load();
}

float
HdEmbreeRenderer::GetRenderElapsedSeconds() const
{
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<float>(now - _renderStartTime).count();
}

static
bool
_IsContained(const GfRect2i& rect, int width, int height)
{
    return
        rect.GetMinX() >= 0 && rect.GetMaxX() < width &&
        rect.GetMinY() >= 0 && rect.GetMaxY() < height;
}

void
HdEmbreeRenderer::_PreRenderSetup()
{
    _completedSamples.store(0);

    // Commit any pending changes to the scene.
    rtcCommitScene(_scene);

    if (!_ValidateAovBindings()) {
        // We aren't going to render anything. Just mark all AOVs as converged
        // so that we will stop rendering.
        for (size_t i = 0; i < _aovBindings.size(); ++i) {
            HdEmbreeRenderBuffer *rb = static_cast<HdEmbreeRenderBuffer*>(
                _aovBindings[i].renderBuffer);
            rb->SetConverged(true);
        }
        // XXX:validation
        TF_WARN("Could not validate Aovs. Render will not complete");
        return;
    }

    _width  = 0;
    _height = 0;

    // Map all of the attachments.
    for (size_t i = 0; i < _aovBindings.size(); ++i) {
        //
        // XXX
        //
        // A scene delegate might specify the path to a
        // render buffer instead of a pointer to the
        // render buffer.
        //
        static_cast<HdEmbreeRenderBuffer*>(
            _aovBindings[i].renderBuffer)->Map();

        if (i == 0) {
            _width  = _aovBindings[i].renderBuffer->GetWidth();
            _height = _aovBindings[i].renderBuffer->GetHeight();
        } else {
            if (_width  != _aovBindings[i].renderBuffer->GetWidth() ||
                 _height != _aovBindings[i].renderBuffer->GetHeight()) {
                TF_CODING_ERROR(
                    "Embree render buffers have inconsistent sizes");
            }
        }
    }

    if (_width > 0 || _height > 0) {
        if (!_IsContained(_dataWindow, _width, _height)) {
            TF_CODING_ERROR(
                "dataWindow is larger than render buffer");
        }
    }

    // Allocate adaptive sampling arrays if enabled.
    if (_enableAdaptiveSampling && _width > 0 && _height > 0) {
        const size_t numPixels = _width * _height;
        _pixelMean.resize(numPixels, GfVec3f(0.0f));
        _pixelM2.resize(numPixels, GfVec3f(0.0f));
        _pixelSampleCount.resize(numPixels, 0);
        _pixelConverged.resize(numPixels, false);
    }

    _BuildAovDispatchTable();
}

// ---------------------------------------------------------------------------
// _BuildAovDispatchTable
// ---------------------------------------------------------------------------

void
HdEmbreeRenderer::_BuildAovDispatchTable()
{
    _aovWriters.clear();
    _needColor = _enableAdaptiveSampling;
    _colorClearValue = GfVec4f(0.0f);
    _varianceFn = nullptr;

    // Find color clear value and set _needColor.
    for (size_t i = 0; i < _aovNames.size(); ++i) {
        if (_aovNames[i].name == HdAovTokens->color) {
            _needColor = true;
            _colorClearValue = _GetClearColor(_aovBindings[i].clearValue);
            break;
        }
    }

    // Set variance function.
    if (_enableAdaptiveSampling && !_pixelConverged.empty()) {
        _varianceFn = _usePerChannelVariance
            ? &_UpdateVariancePerChannel
            : &_UpdateVarianceLuminance;
    }

    // Build writer table.
    for (size_t i = 0; i < _aovBindings.size(); ++i) {
        HdEmbreeRenderBuffer *rb = static_cast<HdEmbreeRenderBuffer*>(
            _aovBindings[i].renderBuffer);
        const auto& aovName = _aovNames[i];

        if (aovName.name == HdAovTokens->color) {
            _AovWriteFn fn = (_showAdaptiveHeatmap
                && _enableAdaptiveSampling
                && !_pixelSampleCount.empty())
                ? &_WriteColorHeatmap : &_WriteColor;
            _aovWriters.push_back(_AovWriter{rb, fn, {}});
        } else if (aovName.name == HdAovTokens->cameraDepth &&
                   rb->GetFormat() == HdFormatFloat32) {
            _aovWriters.push_back(_AovWriter{rb, &_WriteDepth, {}});
        } else if (aovName.name == HdAovTokens->depth &&
                   rb->GetFormat() == HdFormatFloat32) {
            _aovWriters.push_back(_AovWriter{rb, &_WriteClipDepth, {}});
        } else if ((aovName.name == HdAovTokens->primId ||
                    aovName.name == HdAovTokens->elementId ||
                    aovName.name == HdAovTokens->instanceId) &&
                   rb->GetFormat() == HdFormatInt32) {
            _aovWriters.push_back(_AovWriter{rb, &_WriteId, aovName.name});
        } else if (aovName.name == HdAovTokens->normal &&
                   rb->GetFormat() == HdFormatFloat32Vec3) {
            _aovWriters.push_back(_AovWriter{rb, &_WriteNormal, {}});
        } else if (aovName.name == HdAovTokens->Neye &&
                   rb->GetFormat() == HdFormatFloat32Vec3) {
            _aovWriters.push_back(_AovWriter{rb, &_WriteNormalEye, {}});
        } else if (aovName.isPrimvar &&
                   rb->GetFormat() == HdFormatFloat32Vec3) {
            _aovWriters.push_back(_AovWriter{rb, &_WritePrimvar, aovName.name});
        } else if (aovName.name == HdEmbreeAovTokens->adaptiveHeatmap) {
            if (_enableAdaptiveSampling && !_pixelSampleCount.empty()) {
                _aovWriters.push_back(
                    _AovWriter{rb, &_WriteAdaptiveHeatmap, {}});
            }
        }
    }
}

void
HdEmbreeRenderer::Render(HdRenderThread *renderThread)
{
#if TBB_INTERFACE_VERSION_MAJOR < 12
    _ScopedThreadScheduler scheduler;
#endif

    _PreRenderSetup();

    _renderStartTime = std::chrono::steady_clock::now();

    // Compute baseSeed once per Render() call so that every pixel uses a
    // consistent Owen scrambling seed across all samples.  Previously this
    // was computed inside _RenderTiles using system_clock::now(), which meant
    // each pass (and each thread chunk) got a different seed, destroying the
    // low-discrepancy property of the Sobol sequence.
    uint32_t baseSeed;
    if (_randomNumberSeed == -1) {
        baseSeed = static_cast<uint32_t>(
            std::chrono::system_clock::now().time_since_epoch().count());
    } else {
        baseSeed = static_cast<uint32_t>(_randomNumberSeed);
    }

    // Render the image. Each pass through the loop adds a sample per pixel
    // (with jittered ray direction); the longer the loop runs, the less noisy
    // the image becomes. We add a cancellation point once per loop.
    //
    // We consider the image converged after N samples, which is a convenient
    // and simple heuristic.
    for (int i = 0; i < _samplesToConvergence; ++i) {
        // Pause point.
        while (renderThread->IsPauseRequested()) {
            if (renderThread->IsStopRequested()) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        // Cancellation point.
        if (renderThread->IsStopRequested()) {
            break;
        }

        const unsigned int tileSize = HdEmbreeConfig::GetInstance().tileSize;
        const unsigned int numTilesX =
            (_dataWindow.GetWidth() + tileSize - 1) / tileSize;
        const unsigned int numTilesY =
            (_dataWindow.GetHeight() + tileSize - 1) / tileSize;

        // Render by scheduling square tiles of the sample buffer in a parallel
        // for loop.
        // Always pass the renderThread to _RenderTiles to allow the first frame
        // to be interrupted.
        WorkParallelForN(numTilesX * numTilesY,
            std::bind(&HdEmbreeRenderer::_RenderTiles, this,
                renderThread, i, baseSeed,
                std::placeholders::_1, std::placeholders::_2));

        // After the first pass, mark the single-sampled attachments as
        // converged and unmap them. If there are no multisampled attachments,
        // we are done.
        if (i == 0) {
            bool moreWork = false;
            for (size_t i = 0; i < _aovBindings.size(); ++i) {
                HdEmbreeRenderBuffer *rb = static_cast<HdEmbreeRenderBuffer*>(
                    _aovBindings[i].renderBuffer);
                if (rb->IsMultiSampled()) {
                    moreWork = true;
                }
            }
            if (!moreWork) {
                _completedSamples.store(i + 1);
                break;
            }
        }

        // Track the number of completed samples for external consumption.
        _completedSamples.store(i + 1);

        // If adaptive sampling is enabled, check if all pixels converged.
        if (_enableAdaptiveSampling && !_pixelConverged.empty()) {
            bool allConverged = true;
            for (size_t p = 0; p < _pixelConverged.size(); ++p) {
                if (!_pixelConverged[p]) {
                    allConverged = false;
                    break;
                }
            }
            if (allConverged) {
                break;
            }
        }

        // Cancellation point.
        if (renderThread->IsStopRequested()) {
            break;
        }
    }

    // Mark the multisampled attachments as converged and unmap all buffers.
    for (size_t i = 0; i < _aovBindings.size(); ++i) {
        HdEmbreeRenderBuffer *rb = static_cast<HdEmbreeRenderBuffer*>(
            _aovBindings[i].renderBuffer);
        rb->Unmap();
        rb->SetConverged(true);
    }

    // Print render statistics.
    {
        const float elapsedSec = GetRenderElapsedSeconds();
        const int completedSamples = _completedSamples.load();
        const int w = _dataWindow.GetWidth();
        const int h = _dataWindow.GetHeight();
        const long long totalSamples =
            static_cast<long long>(w) * h * completedSamples;

        std::printf("\n");
        std::printf("===== hdEmbree Render Statistics =====\n");
        std::printf("  Resolution       : %d x %d\n", w, h);
        std::printf("  Samples/pixel    : %d / %d\n",
                    completedSamples, _samplesToConvergence);
        std::printf("  Total samples    : %lld\n", totalSamples);
        std::printf("  Render time      : %.3f s\n", elapsedSec);
        if (elapsedSec > 0.0f) {
            std::printf("  Samples/sec      : %.0f\n",
                        totalSamples / static_cast<double>(elapsedSec));
            std::printf("  Pixels/sec       : %.0f\n",
                        (static_cast<double>(w) * h * completedSamples)
                            / elapsedSec);
        }
        std::printf("  Max bounces      : %d\n", _maxBounces);
        std::printf("  Light samples    : %d\n", _lightSamplesPerHit);
        std::printf("  Sampler sequence : %s\n",
                    HdEmbreeGetSamplerSequenceToken(_samplerSequence).GetText());

        if (_enableAdaptiveSampling && !_pixelConverged.empty()) {
            size_t convergedCount = 0;
            double avgSamples = 0.0;
            for (size_t p = 0; p < _pixelConverged.size(); ++p) {
                if (_pixelConverged[p]) {
                    ++convergedCount;
                }
                avgSamples += _pixelSampleCount[p];
            }
            avgSamples /= _pixelConverged.size();
            const double convergedPct =
                100.0 * convergedCount / _pixelConverged.size();
            std::printf("  Adaptive sampling: on (threshold=%.4f)\n",
                        _adaptiveThreshold);
            std::printf("  Converged pixels : %zu / %zu (%.1f%%)\n",
                        convergedCount, _pixelConverged.size(), convergedPct);
            std::printf("  Avg samples/pixel: %.1f\n", avgSamples);
        }
        std::printf("======================================\n");
        std::fflush(stdout);
    }
}

void
HdEmbreeRenderer::_RenderTiles(HdRenderThread *renderThread, int sampleNum,
                               uint32_t baseSeed,
                               size_t tileStart, size_t tileEnd)
{
    const unsigned int minX = _dataWindow.GetMinX();
    unsigned int minY = _dataWindow.GetMinY();
    const unsigned int maxX = _dataWindow.GetMaxX() + 1;
    unsigned int maxY = _dataWindow.GetMaxY() + 1;

    // If a client does not use AOVs and we have no render buffers,
    // _height is 0 and we shouldn't use it to flip the data window.
    if (_height > 0) {
        // The data window is y-Down but the image line order
        // is from bottom to top, so we need to flip it.
        std::swap(minY, maxY);
        minY = _height - minY;
        maxY = _height - maxY;
    }

    const unsigned int tileSize =
        HdEmbreeConfig::GetInstance().tileSize;
    const unsigned int numTilesX =
        (_dataWindow.GetWidth() + tileSize - 1) / tileSize;

    // _RenderTiles gets a range of tiles; iterate through them.
    for (unsigned int tile = tileStart; tile < tileEnd; ++tile) {
        // Cancellation point.
        if (renderThread && renderThread->IsStopRequested()) {
            break;
        }

        // Compute the pixel location of tile boundaries.
        const unsigned int tileY = tile / numTilesX;
        const unsigned int tileX = tile - tileY * numTilesX;
        // (Above is equivalent to: tileX = tile % numTilesX)
        const unsigned int x0 = tileX * tileSize + minX;
        const unsigned int y0 = tileY * tileSize + minY;
        // Clamp to data window, in case tileSize doesn't
        // neatly divide its with and height.
        const unsigned int x1 = std::min(x0 + tileSize, maxX);
        const unsigned int y1 = std::min(y0 + tileSize, maxY);

        // Loop over pixels casting rays.
        for (unsigned int y = y0; y < y1; ++y) {
            for (unsigned int x = x0; x < x1; ++x) {

                // Skip converged pixels in adaptive sampling mode.
                const size_t pixelIdx = y * _width + x;
                if (_enableAdaptiveSampling
                    && !_pixelConverged.empty()
                    && _pixelConverged[pixelIdx]) {
                    continue;
                }

                // Create a per-pixel sampler (Sobol or pseudo-random).
                uint32_t pixelSeed = static_cast<uint32_t>(
                    TfHash::Combine(baseSeed, x, y));
                HdEmbreeSobolSampler sampler(
                    pixelSeed,
                    x,
                    y,
                    sampleNum,
                    _samplerSequence);

                // Jitter the camera ray direction.
                GfVec2f jitter(0.0f, 0.0f);
                if (HdEmbreeConfig::GetInstance().jitterCamera) {
                    jitter = GfVec2f(sampler.Next(), sampler.Next());
                }

                // Un-transform the pixel's NDC coordinates through the
                // projection matrix to get the trace of the camera ray in the
                // near plane.
                const float w(_dataWindow.GetWidth());
                const float h(_dataWindow.GetHeight());

                const GfVec3f ndc(
                    2.0f * ((x + jitter[0] - minX) / w) - 1.0f,
                    2.0f * ((y + jitter[1] - minY) / h) - 1.0f,
                    -1.0f);
                const GfVec3f nearPlaneTrace(_inverseProjMatrix.Transform(ndc));

                GfVec3f origin;
                GfVec3f dir;

                const bool isOrthographic = round(_projMatrix[3][3]) == 1.0;
                if (isOrthographic) {
                    // During orthographic projection: trace parallel rays
                    // from the near plane trace.
                    origin = nearPlaneTrace;
                    dir = GfVec3f(0.0f, 0.0f, -1.0f);
                } else {
                    // Otherwise, assume this is a perspective projection;
                    // project from the camera origin through the
                    // near plane trace.
                    origin = GfVec3f(0.0f, 0.0f, 0.0f);
                    dir = nearPlaneTrace;
                }
                // Transform camera rays to world space.
                origin = GfVec3f(_inverseViewMatrix.Transform(origin));
                dir = GfVec3f(
                    _inverseViewMatrix.TransformDir(dir)).GetNormalized();

                // --- Ray differential ---
                HdEmbreeRayDifferential rayDiff;
                {
                    const GfVec3f ndcDx(
                        2.0f * ((x + 1.0f + jitter[0] - minX) / w) - 1.0f,
                        2.0f * ((y + jitter[1] - minY) / h) - 1.0f,
                        -1.0f);
                    const GfVec3f ndcDy(
                        2.0f * ((x + jitter[0] - minX) / w) - 1.0f,
                        2.0f * ((y + 1.0f + jitter[1] - minY) / h) - 1.0f,
                        -1.0f);
                    const GfVec3f nearDx(
                        _inverseProjMatrix.Transform(ndcDx));
                    const GfVec3f nearDy(
                        _inverseProjMatrix.Transform(ndcDy));

                    if (isOrthographic) {
                        rayDiff.rxOrigin = GfVec3f(
                            _inverseViewMatrix.Transform(nearDx));
                        rayDiff.ryOrigin = GfVec3f(
                            _inverseViewMatrix.Transform(nearDy));
                        rayDiff.rxDirection = dir;
                        rayDiff.ryDirection = dir;
                    } else {
                        rayDiff.rxOrigin = origin;
                        rayDiff.ryOrigin = origin;
                        rayDiff.rxDirection = GfVec3f(
                            _inverseViewMatrix.TransformDir(nearDx))
                            .GetNormalized();
                        rayDiff.ryDirection = GfVec3f(
                            _inverseViewMatrix.TransformDir(nearDy))
                            .GetNormalized();
                    }
                    rayDiff.hasDifferentials = true;

                    // Scale by 1/sqrt(spp) to match sampling rate.
                    if (_samplesToConvergence > 1) {
                        float scale = 1.0f / std::sqrt(
                            static_cast<float>(_samplesToConvergence));
                        if (isOrthographic) {
                            GfVec3f dOx = rayDiff.rxOrigin - origin;
                            GfVec3f dOy = rayDiff.ryOrigin - origin;
                            rayDiff.rxOrigin = origin + dOx * scale;
                            rayDiff.ryOrigin = origin + dOy * scale;
                        } else {
                            GfVec3f dDx = rayDiff.rxDirection - dir;
                            GfVec3f dDy = rayDiff.ryDirection - dir;
                            rayDiff.rxDirection =
                                (dir + dDx * scale).GetNormalized();
                            rayDiff.ryDirection =
                                (dir + dDy * scale).GetNormalized();
                        }
                    }
                }

                // Trace the ray.
                _TraceRay(x, y, origin, dir, sampler, rayDiff);
            }
        }
    }
}

/// Fill in an RTCRay structure from the given parameters.
static void
_PopulateRay(
    RTCRay *ray,
    GfVec3f const& origin,
    GfVec3f const& dir,
    float nearest,
    float furthest = std::numeric_limits<float>::infinity(),
    HdEmbree_RayMask mask = HdEmbree_RayMask::All)
{
    ray->org_x = origin[0];
    ray->org_y = origin[1];
    ray->org_z = origin[2];
    ray->tnear = nearest;

    ray->dir_x = dir[0];
    ray->dir_y = dir[1];
    ray->dir_z = dir[2];
    ray->time = 0.0f;

    ray->tfar = furthest;
    ray->mask = static_cast<uint32_t>(mask);
}

static GfVec3f
_OffsetRayOrigin(
    GfVec3f const& position,
    GfVec3f const& normal,
    GfVec3f const& direction,
    float bias = 1.0e-4f)
{
    if (normal.GetLengthSq() < 1e-18f) {
        return position + direction * bias;
    }

    GfVec3f offsetNormal = normal.GetNormalized();
    if (GfDot(offsetNormal, direction) < 0.0f) {
        offsetNormal = -offsetNormal;
    }
    return position + offsetNormal * bias;
}

/// Fill in an RTCRayHit structure from the given parameters.
// note this containts a Ray and a RayHit
static void
_PopulateRayHit(
    RTCRayHit* rayHit,
    GfVec3f const& origin,
    GfVec3f const& dir,
    float nearest,
    float furthest = std::numeric_limits<float>::infinity(),
    HdEmbree_RayMask mask = HdEmbree_RayMask::All)
{
    // Fill in defaults for the ray
    _PopulateRay(&rayHit->ray, origin, dir, nearest, furthest, mask);

    // Fill in defaults for the hit
    rayHit->hit.primID = RTC_INVALID_GEOMETRY_ID;
    rayHit->hit.geomID = RTC_INVALID_GEOMETRY_ID;
}

/// Generate a random cosine-weighted direction ray (in the hemisphere
/// around <0,0,1>).  The input is a pair of uniformly distributed random
/// numbers in the range [0,1].
///
/// The algorithm here is to generate a random point on the disk, and project
/// that point to the unit hemisphere.
static GfVec3f
_CosineWeightedDirection(GfVec2f const& uniform_float)
{
    GfVec3f dir;
    float theta = 2.0f * _pi<float> * uniform_float[0];
    float eta = uniform_float[1];
    float sqrteta = sqrtf(eta);
    dir[0] = cosf(theta) * sqrteta;
    dir[1] = sinf(theta) * sqrteta;
    dir[2] = sqrtf(1.0f-eta);
    return dir;
}

// ---------------------------------------------------------------------------
// AOV writer functions (dispatched via _aovWriters table)
// ---------------------------------------------------------------------------

/* static */
GfVec4f
HdEmbreeRenderer::_HeatmapColor(float t)
{
    t = std::min(t, 1.0f);
    float r, g, b;
    if (t < 0.25f) {
        float s = t / 0.25f;
        r = 0.0f; g = s; b = 1.0f;
    } else if (t < 0.5f) {
        float s = (t - 0.25f) / 0.25f;
        r = 0.0f; g = 1.0f; b = 1.0f - s;
    } else if (t < 0.75f) {
        float s = (t - 0.5f) / 0.25f;
        r = s; g = 1.0f; b = 0.0f;
    } else {
        float s = (t - 0.75f) / 0.25f;
        r = 1.0f; g = 1.0f - s; b = 0.0f;
    }
    return GfVec4f(r, g, b, 1.0f);
}

/* static */
void
HdEmbreeRenderer::_WriteColor(
    HdEmbreeRenderer*, _AovWriter const& w,
    RTCRayHit const&, GfVec4f const& color,
    unsigned int x, unsigned int y)
{
    w.buffer->Write(GfVec3i(x, y, 1), 4, color.data());
}

/* static */
void
HdEmbreeRenderer::_WriteColorHeatmap(
    HdEmbreeRenderer* self, _AovWriter const& w,
    RTCRayHit const&, GfVec4f const&,
    unsigned int x, unsigned int y)
{
    const size_t idx = y * self->_width + x;
    float t = static_cast<float>(self->_pixelSampleCount[idx])
            / static_cast<float>(std::max(1, self->_samplesToConvergence));
    GfVec4f heatColor = _HeatmapColor(t);
    w.buffer->WriteOutput(GfVec3i(x, y, 1), 4, heatColor.data());
}

/* static */
void
HdEmbreeRenderer::_WriteDepth(
    HdEmbreeRenderer* self, _AovWriter const& w,
    RTCRayHit const& rayHit, GfVec4f const&,
    unsigned int x, unsigned int y)
{
    float depth;
    if (self->_ComputeDepth(rayHit, &depth, false)) {
        w.buffer->Write(GfVec3i(x, y, 1), 1, &depth);
    }
}

/* static */
void
HdEmbreeRenderer::_WriteClipDepth(
    HdEmbreeRenderer* self, _AovWriter const& w,
    RTCRayHit const& rayHit, GfVec4f const&,
    unsigned int x, unsigned int y)
{
    float depth;
    if (self->_ComputeDepth(rayHit, &depth, true)) {
        w.buffer->Write(GfVec3i(x, y, 1), 1, &depth);
    }
}

/* static */
void
HdEmbreeRenderer::_WriteId(
    HdEmbreeRenderer* self, _AovWriter const& w,
    RTCRayHit const& rayHit, GfVec4f const&,
    unsigned int x, unsigned int y)
{
    int32_t id;
    if (self->_ComputeId(rayHit, w.token, &id)) {
        w.buffer->Write(GfVec3i(x, y, 1), 1, &id);
    }
}

/* static */
void
HdEmbreeRenderer::_WriteNormal(
    HdEmbreeRenderer* self, _AovWriter const& w,
    RTCRayHit const& rayHit, GfVec4f const&,
    unsigned int x, unsigned int y)
{
    GfVec3f normal;
    if (self->_ComputeNormal(rayHit, &normal, false)) {
        w.buffer->Write(GfVec3i(x, y, 1), 3, normal.data());
    }
}

/* static */
void
HdEmbreeRenderer::_WriteNormalEye(
    HdEmbreeRenderer* self, _AovWriter const& w,
    RTCRayHit const& rayHit, GfVec4f const&,
    unsigned int x, unsigned int y)
{
    GfVec3f normal;
    if (self->_ComputeNormal(rayHit, &normal, true)) {
        w.buffer->Write(GfVec3i(x, y, 1), 3, normal.data());
    }
}

/* static */
void
HdEmbreeRenderer::_WritePrimvar(
    HdEmbreeRenderer* self, _AovWriter const& w,
    RTCRayHit const& rayHit, GfVec4f const&,
    unsigned int x, unsigned int y)
{
    GfVec3f value;
    if (self->_ComputePrimvar(rayHit, w.token, &value)) {
        w.buffer->Write(GfVec3i(x, y, 1), 3, value.data());
    }
}

/* static */
void
HdEmbreeRenderer::_WriteAdaptiveHeatmap(
    HdEmbreeRenderer* self, _AovWriter const& w,
    RTCRayHit const&, GfVec4f const&,
    unsigned int x, unsigned int y)
{
    const size_t idx = y * self->_width + x;
    float t = static_cast<float>(self->_pixelSampleCount[idx] + 1)
            / static_cast<float>(std::max(1, self->_samplesToConvergence));
    GfVec4f heatColor = _HeatmapColor(t);
    w.buffer->Write(GfVec3i(x, y, 1), 4, heatColor.data());
}

// ---------------------------------------------------------------------------
// Variance update functions
// ---------------------------------------------------------------------------

/* static */
void
HdEmbreeRenderer::_UpdateVariancePerChannel(
    HdEmbreeRenderer* self,
    unsigned int x, unsigned int y,
    GfVec3f const& rgb)
{
    const size_t idx = y * self->_width + x;
    uint32_t count = ++self->_pixelSampleCount[idx];
    GfVec3f delta = rgb - self->_pixelMean[idx];
    self->_pixelMean[idx] += delta / static_cast<float>(count);
    GfVec3f delta2 = rgb - self->_pixelMean[idx];
    self->_pixelM2[idx] += GfCompMult(delta, delta2);

    if (count >= static_cast<uint32_t>(self->_minSamplesBeforeAdaptive)) {
        float fCount = static_cast<float>(count);
        GfVec3f varOfMean = self->_pixelM2[idx] / (fCount * fCount);
        const GfVec3f &mean = self->_pixelMean[idx];
        constexpr float kMinValue = 0.001f;
        float relVar[3];
        for (int c = 0; c < 3; ++c) {
            relVar[c] = (std::abs(mean[c]) > kMinValue)
                ? varOfMean[c] / std::abs(mean[c])
                : varOfMean[c];
        }
        float maxVar = std::max({relVar[0], relVar[1], relVar[2]});
        if (maxVar <= self->_adaptiveThreshold) {
            self->_pixelConverged[idx] = true;
        }
    }
}

/* static */
void
HdEmbreeRenderer::_UpdateVarianceLuminance(
    HdEmbreeRenderer* self,
    unsigned int x, unsigned int y,
    GfVec3f const& rgb)
{
    const size_t idx = y * self->_width + x;
    uint32_t count = ++self->_pixelSampleCount[idx];
    GfVec3f delta = rgb - self->_pixelMean[idx];
    self->_pixelMean[idx] += delta / static_cast<float>(count);
    GfVec3f delta2 = rgb - self->_pixelMean[idx];
    self->_pixelM2[idx] += GfCompMult(delta, delta2);

    if (count >= static_cast<uint32_t>(self->_minSamplesBeforeAdaptive)) {
        float fCount = static_cast<float>(count);
        GfVec3f varOfMean = self->_pixelM2[idx] / (fCount * fCount);
        const GfVec3f &mean = self->_pixelMean[idx];
        constexpr float kMinValue = 0.001f;
        float luminance = 0.2126f * mean[0]
                        + 0.7152f * mean[1]
                        + 0.0722f * mean[2];
        float maxVar;
        if (luminance > kMinValue) {
            float invL2 = 1.0f / (luminance * luminance);
            maxVar = std::max({varOfMean[0] * invL2,
                               varOfMean[1] * invL2,
                               varOfMean[2] * invL2});
        } else {
            maxVar = std::max({varOfMean[0],
                               varOfMean[1],
                               varOfMean[2]});
        }
        if (maxVar <= self->_adaptiveThreshold) {
            self->_pixelConverged[idx] = true;
        }
    }
}

// ---------------------------------------------------------------------------
// _TraceRay
// ---------------------------------------------------------------------------

void
HdEmbreeRenderer::_TraceRay(unsigned int x, unsigned int y,
                            GfVec3f const& origin, GfVec3f const& dir,
                            HdEmbreeSobolSampler& sampler,
                            HdEmbreeRayDifferential const& rayDiff)
{
    // Intersect the camera ray.
    RTCRayHit rayHit; // EMBREE_FIXME: use RTCRay for occlusion rays
    rayHit.ray.flags = 0;
    _PopulateRayHit(&rayHit, origin, dir, 0.0f,
                    std::numeric_limits<float>::max(),
                    HdEmbree_RayMask::Camera);
    {
        rtcIntersect1(_scene, &rayHit);
    }

    GfVec4f colorSample(0.0f);
    if (_needColor) {
        colorSample = _ComputeColor(rayHit, rayDiff, sampler, _colorClearValue);
    }

    if (_varianceFn) {
        GfVec3f rgb(colorSample[0], colorSample[1], colorSample[2]);
        _varianceFn(this, x, y, rgb);
    }

    for (const auto& writer : _aovWriters) {
        if (!writer.buffer->IsConverged()) {
            writer.writeFn(this, writer, rayHit, colorSample, x, y);
        }
    }
}

bool
HdEmbreeRenderer::_ComputeId(RTCRayHit const& rayHit, TfToken const& idType,
                             int32_t *id)
{
    if (rayHit.hit.geomID == RTC_INVALID_GEOMETRY_ID) {
        return false;
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
            rtcGetGeometryUserData(rtcGetGeometry(instanceContext->rootScene,
                                                  rayHit.hit.geomID)));

    if (idType == HdAovTokens->primId) {
        *id = prototypeContext->rprim->GetPrimId();
    } else if (idType == HdAovTokens->elementId) {
        if (prototypeContext->primitiveParams.empty()) {
            *id = rayHit.hit.primID;
        } else {
            *id = HdMeshUtil::DecodeFaceIndexFromCoarseFaceParam(
                prototypeContext->primitiveParams[rayHit.hit.primID]);
        }
    } else if (idType == HdAovTokens->instanceId) {
        *id = instanceContext->instanceId;
    } else {
        return false;
    }

    return true;
}

bool
HdEmbreeRenderer::_ComputeDepth(RTCRayHit const& rayHit,
                                float *depth,
                                bool clip)
{
    if (rayHit.hit.geomID == RTC_INVALID_GEOMETRY_ID) {
        return false;
    }

    if (clip) {
        GfVec3f hitPos = _CalculateHitPosition(rayHit);

        hitPos = GfVec3f(_viewMatrix.Transform(hitPos));
        hitPos = GfVec3f(_projMatrix.Transform(hitPos));

        // For the depth range transform, we assume [0,1].
        *depth = (hitPos[2] + 1.0f) / 2.0f;
    } else {
        *depth = rayHit.ray.tfar;
    }
    return true;
}

bool
HdEmbreeRenderer::_ComputeNormal(RTCRayHit const& rayHit,
                                 GfVec3f *normal,
                                 bool eye)
{
    if (rayHit.hit.geomID == RTC_INVALID_GEOMETRY_ID) {
        return false;
    }

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

    GfVec3f n = _ResolveObjectSpaceNormal(
        prototypeContext, instanceContext->rootScene, rayHit.hit.geomID,
        rayHit);

    n = instanceContext->objectToWorldMatrix.TransformDir(n);
    if (eye) {
        n = GfVec3f(_viewMatrix.TransformDir(n));
    }
    n.Normalize();

    *normal = n;
    return true;
}

bool
HdEmbreeRenderer::_ComputePrimvar(RTCRayHit const& rayHit,
                                  TfToken const& primvar,
                                  GfVec3f *value)
{
    if (rayHit.hit.geomID == RTC_INVALID_GEOMETRY_ID) {
        return false;
    }

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

    // XXX: This is a little clunky, although sample will early out if the
    // types don't match.
    auto it = prototypeContext->primvarMap.find(primvar);
    if (it != prototypeContext->primvarMap.end()) {
        const HdEmbreePrimvarSampler *sampler = it->second;
        if (sampler->Sample(rayHit.hit.primID, rayHit.hit.u, rayHit.hit.v,
                            value)) {
            return true;
        }
        GfVec2f v2;
        if (sampler->Sample(rayHit.hit.primID, rayHit.hit.u, rayHit.hit.v,
                            &v2)) {
            value->Set(v2[0], v2[1], 0.0f);
            return true;
        }
        float v1;
        if (sampler->Sample(rayHit.hit.primID, rayHit.hit.u, rayHit.hit.v,
                            &v1)) {
            value->Set(v1, 0.0f, 0.0f);
            return true;
        }
    }
    return false;
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
    ctx.texcoord = _ToMx(texcoordVal);
    ctx.displayColor = _ToMx(displayColor);
    ctx.displayOpacity = 1.0f;
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

float
HdEmbreeRenderer::_EvalOpacityAtHit(RTCRayHit const& rayHit) const
{
    const HdEmbreeInstanceContext *instanceContext =
        static_cast<HdEmbreeInstanceContext*>(
            rtcGetGeometryUserData(
                rtcGetGeometry(_scene, rayHit.hit.instID[0])));
    const HdEmbreePrototypeContext *prototypeContext =
        static_cast<HdEmbreePrototypeContext*>(
            rtcGetGeometryUserData(
                rtcGetGeometry(instanceContext->rootScene,
                               rayHit.hit.geomID)));

    HdEmbreeMaterial *material = prototypeContext->material;
    if (!material) return 1.0f;

    mxcpp::EvalGraph *evalGraph = material->GetEvalGraph();
    if (!evalGraph) return 1.0f;

    GfVec3f hitPos = _CalculateHitPosition(rayHit);
    GfVec3f normal = _ResolveObjectSpaceNormal(
        prototypeContext, instanceContext->rootScene, rayHit.hit.geomID,
        rayHit);
    normal = instanceContext->objectToWorldMatrix.TransformDir(normal);
    normal.Normalize();

    try {
        HdEmbreeRayDifferential defaultRayDiff;
        const _ShadingContextOptions options(false);
        mxcpp::ShadingContext ctx = _BuildShadingContext(
            rayHit, defaultRayDiff,
            instanceContext, prototypeContext, hitPos, normal,
            nullptr, nullptr, options);
        _GeomPropCallbackData cbData{
            &prototypeContext->primvarMap,
            rayHit.hit.primID, rayHit.hit.u, rayHit.hit.v};
        ctx.geomPropLookup = &_SampleGeomProp;
        ctx.geomPropUserData = &cbData;
        ctx.uniformProps = &prototypeContext->uniformPrimvarMap;
        mxcpp::SurfaceClosure closure = evalGraph->Evaluate(ctx);
        return closure.opacity;
    } catch (...) {
        return 1.0f;
    }
}

float
HdEmbreeRenderer::_Visibility(
    GfVec3f const& position,
    GfVec3f const& normal,
    GfVec3f const& direction,
    float dist) const
{
    constexpr int kMaxTransparentHits = 16;
    constexpr float kVisThreshold = 1e-4f;
    constexpr float kRayBias = 1e-4f;

    float visibility = 1.0f;
    GfVec3f rayOrigin = _OffsetRayOrigin(position, normal, direction, kRayBias);
    float remaining = dist;

    for (int i = 0; i < kMaxTransparentHits; ++i) {
        RTCRayHit rayHit;
        rayHit.ray.flags = 0;
        _PopulateRayHit(&rayHit, rayOrigin, direction, kRayBias, remaining,
                        HdEmbree_RayMask::Camera);
        rtcIntersect1(_scene, &rayHit);

        if (rayHit.hit.geomID == RTC_INVALID_GEOMETRY_ID) {
            return visibility;
        }

        float opacity = _EvalOpacityAtHit(rayHit);
        visibility *= (1.0f - opacity);

        if (visibility <= kVisThreshold) {
            return 0.0f;
        }

        float hitDist = rayHit.ray.tfar;
        remaining -= hitDist;
        if (remaining <= 0.001f) {
            return visibility;
        }

        GfVec3f hitPos = GfVec3f(
            rayHit.ray.org_x + hitDist * rayHit.ray.dir_x,
            rayHit.ray.org_y + hitDist * rayHit.ray.dir_y,
            rayHit.ray.org_z + hitDist * rayHit.ray.dir_z);
        rayOrigin = _OffsetRayOrigin(hitPos, normal, direction, kRayBias);
    }

    return visibility;
}

GfVec4f
HdEmbreeRenderer::_ComputeColor(RTCRayHit const& rayHit,
                                HdEmbreeRayDifferential const& rayDiff,
                                HdEmbreeSobolSampler &sampler,
                                GfVec4f const& clearColor)
{
    if (rayHit.hit.geomID == RTC_INVALID_GEOMETRY_ID) {
        if (_domes.empty() || !_enableLighting || !_domeLightCameraVisibility) {
            return clearColor;
        }

        // if we missed all geometry in the scene, evaluate the infinite lights
        // directly
        GfVec4f domeColor(0.0f, 0.0f, 0.0f, 1.0f);
        for (auto* dome : _domes) {
            if (!dome->LightData().visible) {
                continue;
            }
            // Direct visibility: sample the dome lights. Since we know
            // we're only sampling domes, we don't care about the position, and
            // the sample direction is the camera ray direction.
            // Passing in (1.0, 0.0) ensures we don't jitter off the normal
            // while sampling.
            HdEmbreeLightSampler::LightSample ls =
                HdEmbreeLightSampler::GetLightSample(
                    dome->LightData(), GfVec3f(0),
                    GfVec3f(rayHit.ray.dir_x, rayHit.ray.dir_y, rayHit.ray.dir_z),
                    1.0f, 0.0f);
            domeColor += GfVec4f(ls.Li[0], ls.Li[1], ls.Li[2], 0);
        }
        return domeColor;
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
        &prototypeContext->primvarMap,
        rayHit.hit.primID, rayHit.hit.u, rayHit.hit.v};
    ctx.geomPropLookup = &_SampleGeomProp;
    ctx.geomPropUserData = &cbData;
    ctx.uniformProps = &prototypeContext->uniformPrimvarMap;

    // Recover tangent frame from context for normal map application.
    GfVec3f tangent = _ToGf(ctx.tangent);
    GfVec3f bitangent = _ToGf(ctx.bitangent);

    // Try to evaluate MaterialXCpp material if one is bound.
    HdEmbreeMaterial *material = prototypeContext->material;
    mxcpp::EvalGraph *evalGraph = nullptr;
    if (material) {
        evalGraph = material->GetEvalGraph();
    }

    mxcpp::SurfaceClosure closure;
    bool hasMaterialClosure = false;

    if (evalGraph) {
        try {
            closure = evalGraph->Evaluate(ctx);
            hasMaterialClosure = true;
        } catch (...) {
            hasMaterialClosure = false;
        }
    }

    // Apply material normal map (tangent-space -> world-space).
    if (hasMaterialClosure &&
        closure.normal != mxcpp::Vec3f(0.0f, 0.0f, 1.0f)) {
        normal = (tangent   * closure.normal[0] +
                  bitangent * closure.normal[1] +
                  normal    * closure.normal[2]).GetNormalized();
    }

    GfVec3f lightingColor(0.0f);

    if (!_enableLighting)
    {
        GfVec3f materialColor;
        if (hasMaterialClosure) {
            materialColor = _ToGf(closure.baseColor);
        } else {
            materialColor = _enableSceneColors
                ? _ToGf(ctx.displayColor) : GfVec3f(0.5f);
        }

        GfVec3f dir = GfVec3f(rayHit.ray.dir_x, rayHit.ray.dir_y,
                              rayHit.ray.dir_z);
        float diffuseLight = fabs(GfDot(-dir, normal)) *
            HdEmbreeConfig::GetInstance().cameraLightIntensity;

        float aoLightIntensity =
            _ComputeAmbientOcclusion(hitPos, normal, sampler);

        lightingColor = materialColor * diffuseLight * aoLightIntensity;
    }
    else
    {
        // Path trace from the camera ray origin.
        GfVec3f origin(rayHit.ray.org_x, rayHit.ray.org_y,
                       rayHit.ray.org_z);
        GfVec3f dir = GfVec3f(rayHit.ray.dir_x, rayHit.ray.dir_y,
                              rayHit.ray.dir_z).GetNormalized();
        lightingColor = _TracePath(origin, dir, rayDiff, sampler);
    }

    GfVec4f output;
    output[0] = std::max(0.0f, lightingColor[0]);
    output[1] = std::max(0.0f, lightingColor[1]);
    output[2] = std::max(0.0f, lightingColor[2]);
    output[3] = hasMaterialClosure ? closure.opacity : 1.0f;
    return output;
}

float
HdEmbreeRenderer::_ComputeAmbientOcclusion(GfVec3f const& position,
                                            GfVec3f const& normal,
                                            HdEmbreeSobolSampler &sampler)
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
    for (int i = 0; i < _ambientOcclusionSamples; ++i) {
        samples[i][0] = (float(i) + sampler.Next()) / _ambientOcclusionSamples;
    }
    // Fisher-Yates shuffle using the Sobol sampler.
    for (int i = _ambientOcclusionSamples - 1; i > 0; --i) {
        int j = static_cast<int>(sampler.Next() * (i + 1));
        j = std::min(j, i);
        std::swap(samples[i], samples[j]);
    }
    for (int i = 0; i < _ambientOcclusionSamples; ++i) {
        samples[i][1] = (float(i) + sampler.Next()) / _ambientOcclusionSamples;
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

// ---------------------------------------------------------------------------
// Direct lighting with MIS (light sampling strategy).
// ---------------------------------------------------------------------------

GfVec3f
HdEmbreeRenderer::_ComputeDirectLightingMIS(
    GfVec3f const& position,
    GfVec3f const& normal,
    GfVec3f const& wo,
    HdEmbreeSobolSampler &sampler,
    bool doubleSided,
    mxcpp::SurfaceClosure const* closure) const
{
    GfVec3f finalColor(0.0f);

    const int N = _lightSamplesPerHit;
    const float invN = 1.0f / static_cast<float>(N);

    // For stratification: compute grid dimensions for N samples.
    // Find the largest sqrtN such that sqrtN*sqrtN <= N, then
    // use sqrtN x ceilN grid where ceilN = ceil(N / sqrtN).
    int stratDimU = 1, stratDimV = 1;
    if (_stratifyLightSamples && N > 1) {
        stratDimU = static_cast<int>(std::sqrt(static_cast<float>(N)));
        if (stratDimU < 1) stratDimU = 1;
        stratDimV = (N + stratDimU - 1) / stratDimU;
    }

    for (auto const& it : _lightMap)
    {
        auto const& light = it.second->LightData();
        if (!light.visible) {
            continue;
        }

        GfVec3f lightContrib(0.0f);

        for (int s = 0; s < N; ++s) {
            // Generate sample coordinates, optionally stratified.
            float u1, u2;
            if (_stratifyLightSamples && N > 1) {
                int su = s % stratDimU;
                int sv = s / stratDimU;
                u1 = (su + sampler.Next()) / static_cast<float>(stratDimU);
                u2 = (sv + sampler.Next()) / static_cast<float>(stratDimV);
            } else {
                u1 = sampler.Next();
                u2 = sampler.Next();
            }

            HdEmbreeLightSampler::LightSample ls =
                HdEmbreeLightSampler::GetLightSample(
                light, position, normal, u1, u2);
            if (GfIsClose(ls.Li, GfVec3f(0.0f), _minLuminanceCutoff)) {
                continue;
            }

            float cosOffNormal = GfDot(ls.wI, normal);
            GfVec3f shadingNormal = normal;
            if (cosOffNormal < 0.0f) {
                if (doubleSided) {
                    cosOffNormal *= -1.0f;
                    shadingNormal = -normal;
                } else {
                    cosOffNormal = 0.0f;
                }
            }

            if (cosOffNormal <= 0.0f) {
                continue;
            }

            float vis = _Visibility(
                position, shadingNormal, ls.wI, ls.dist * 0.99f);
            if (vis <= 0.0f) {
                continue;
            }

            GfVec3f sampleContrib(0.0f);
            if (closure) {
                GfVec3f bsdfValue = _ToGf(mxcpp::Bsdf::EvalSurface(
                    *closure, _ToMx(shadingNormal), _ToMx(ls.wI), _ToMx(wo)));

                for (int i = 0; i < 3; ++i) {
                    if (!std::isfinite(bsdfValue[i])) bsdfValue[i] = 0.0f;
                }

                // MIS weight: one-sample MIS with N light samples.
                // The effective light PDF for this multi-sample estimator
                // is lightPdf (per-sample PDF stays the same; the 1/N
                // averaging is handled outside).
                float lightPdf = (ls.invPdfW > 0.0f)
                    ? 1.0f / ls.invPdfW : 0.0f;
                float bsdfPdf = mxcpp::Bsdf::PdfSurface(
                    *closure, _ToMx(shadingNormal), _ToMx(ls.wI), _ToMx(wo));
                float misW = mxcpp::Bsdf::PowerHeuristic(lightPdf, bsdfPdf);

                sampleContrib = GfCompMult(ls.Li, bsdfValue)
                    * cosOffNormal * vis * ls.invPdfW * misW;
            } else {
                float brdf = 1.0f / _pi<float>;
                sampleContrib = ls.Li * cosOffNormal * brdf
                    * vis * ls.invPdfW;
            }

            // Firefly clamping.
            if (_fireflyClampThreshold > 0.0f) {
                float lum = 0.2126f * sampleContrib[0]
                          + 0.7152f * sampleContrib[1]
                          + 0.0722f * sampleContrib[2];
                if (lum > _fireflyClampThreshold) {
                    sampleContrib *= _fireflyClampThreshold / lum;
                }
            }

            lightContrib += sampleContrib;
        }

        finalColor += lightContrib * invN;
    }
    return finalColor;
}

// ---------------------------------------------------------------------------
// Multi-bounce path tracer with MIS.
// ---------------------------------------------------------------------------

GfVec3f
HdEmbreeRenderer::_TracePath(
    GfVec3f const& origin,
    GfVec3f const& dir,
    HdEmbreeRayDifferential const& rayDiff,
    HdEmbreeSobolSampler &sampler) const
{
    HdEmbreeRayDifferential currentRayDiff = rayDiff;
    GfVec3f radiance(0.0f);
    GfVec3f throughput(1.0f);
    GfVec3f rayOrigin = origin;
    GfVec3f rayDir = dir;
    float lastBsdfPdf = 0.0f;
    bool isFirstBounce = true;
    bool anyNonSpecularBounces = false;

    // Per-bounce derivative state for ray differential propagation
    GfVec3f lastDPdu(0.0f), lastDPdv(0.0f);
    GfVec3f lastDndu(0.0f), lastDndv(0.0f);
    GfVec3f lastDpdx(0.0f), lastDpdy(0.0f);
    float lastDudx = 0, lastDvdx = 0, lastDudy = 0, lastDvdy = 0;

    for (int bounce = 0; bounce <= _maxBounces; ++bounce) {
        // QMC padding: reset the sampler so each bounce independently
        // uses the best (lowest) Sobol dimensions.  The bounce-specific
        // key ensures each bounce gets a different Owen scrambling seed.
        sampler.ResetForBounce(
            static_cast<uint32_t>(bounce + 1) * 0x9e3779b9u);

        RTCRayHit rayHit;
        rayHit.ray.flags = 0;
        _PopulateRayHit(&rayHit, rayOrigin, rayDir,
                        isFirstBounce ? 0.0f : 1e-4f,
                        std::numeric_limits<float>::max(),
                        HdEmbree_RayMask::Camera);
        rtcIntersect1(_scene, &rayHit);

        // --- Miss: dome light contribution ---
        if (rayHit.hit.geomID == RTC_INVALID_GEOMETRY_ID) {
            for (auto* dome : _domes) {
                if (!dome->LightData().visible) {
                    continue;
                }
                HdEmbreeLightSampler::LightSample ls =
                    HdEmbreeLightSampler::GetLightSample(
                        dome->LightData(), GfVec3f(0.0f), rayDir,
                        1.0f, 0.0f);
                GfVec3f domeContrib = ls.Li;

                if (!isFirstBounce && lastBsdfPdf > 0.0f) {
                    // MIS weight for BSDF sampling strategy hitting dome.
                    float domePdf = (ls.invPdfW > 0.0f)
                        ? 1.0f / ls.invPdfW : 0.0f;
                    float misW = mxcpp::Bsdf::PowerHeuristic(
                        lastBsdfPdf, domePdf);
                    domeContrib *= misW;
                }

                radiance += GfCompMult(throughput, domeContrib);
            }
            break;
        }

        // --- Process hit ---
        const HdEmbreeInstanceContext *instanceContext =
            static_cast<HdEmbreeInstanceContext*>(
                rtcGetGeometryUserData(
                    rtcGetGeometry(_scene, rayHit.hit.instID[0])));

        const HdEmbreePrototypeContext *prototypeContext =
            static_cast<HdEmbreePrototypeContext*>(
                rtcGetGeometryUserData(
                    rtcGetGeometry(instanceContext->rootScene,
                                   rayHit.hit.geomID)));

        GfVec3f hitPos = GfVec3f(
            rayHit.ray.org_x + rayHit.ray.tfar * rayHit.ray.dir_x,
            rayHit.ray.org_y + rayHit.ray.tfar * rayHit.ray.dir_y,
            rayHit.ray.org_z + rayHit.ray.tfar * rayHit.ray.dir_z);

        // Normal
        GfVec3f normal = _ResolveObjectSpaceNormal(
            prototypeContext, instanceContext->rootScene, rayHit.hit.geomID,
            rayHit);
        normal = instanceContext->objectToWorldMatrix.TransformDir(normal);
        normal.Normalize();

        GfVec3f wo = -rayDir;

        // Double-sided check
        bool doubleSided = false;
        HdEmbreeMesh *mesh =
            dynamic_cast<HdEmbreeMesh*>(prototypeContext->rprim);
        if (mesh) {
            doubleSided = mesh->EmbreeMeshIsDoubleSided();
        }

        // Orient normal toward the ray
        if (GfDot(normal, wo) < 0.0f && doubleSided) {
            normal = -normal;
        }

        // Build shading context via shared helper, also retrieving
        // normal derivatives for bounce propagation.
        mxcpp::ShadingContext ctx = _BuildShadingContext(
            rayHit, currentRayDiff,
            instanceContext, prototypeContext, hitPos, normal,
            &lastDndu, &lastDndv);
        _GeomPropCallbackData cbData{
            &prototypeContext->primvarMap,
            rayHit.hit.primID, rayHit.hit.u, rayHit.hit.v};
        ctx.geomPropLookup = &_SampleGeomProp;
        ctx.geomPropUserData = &cbData;
        ctx.uniformProps = &prototypeContext->uniformPrimvarMap;

        // Capture surface derivatives from ctx (already computed by
        // _BuildShadingContext) for bounce propagation.
        lastDPdu = _ToGf(ctx.dPdu);
        lastDPdv = _ToGf(ctx.dPdv);
        lastDpdx = _ToGf(ctx.dPdx);
        lastDpdy = _ToGf(ctx.dPdy);
        lastDudx = ctx.dudx;
        lastDvdx = ctx.dvdx;
        lastDudy = ctx.dudy;
        lastDvdy = ctx.dvdy;

        GfVec3f tangent = _ToGf(ctx.tangent);
        GfVec3f bitangent = _ToGf(ctx.bitangent);

        // --- Evaluate material ---
        HdEmbreeMaterial *material = prototypeContext->material;
        mxcpp::EvalGraph *evalGraph = material
            ? material->GetEvalGraph() : nullptr;

        mxcpp::SurfaceClosure closure;
        bool hasClosure = false;

        if (evalGraph) {
            try {
                closure = evalGraph->Evaluate(ctx);
                hasClosure = true;
            } catch (...) {
                hasClosure = false;
            }
        }

        // Apply material normal map (tangent-space -> world-space).
        if (hasClosure &&
            closure.normal != mxcpp::Vec3f(0.0f, 0.0f, 1.0f)) {
            normal = (tangent   * closure.normal[0] +
                      bitangent * closure.normal[1] +
                      normal    * closure.normal[2]).GetNormalized();
            // Re-orient toward the ray for double-sided geometry.
            if (GfDot(normal, wo) < 0.0f && doubleSided) {
                normal = -normal;
            }
        }

        // --- Path regularization ---
        // After the first non-specular bounce, widen narrow specular lobes
        // to reduce fireflies from sharp BSDFs on indirect paths.
        if (hasClosure && anyNonSpecularBounces) {
            closure.Regularize();
        }

        // --- Stochastic opacity pass-through ---
        if (hasClosure && closure.opacity < 1.0f) {
            if (sampler.Next() > closure.opacity) {
                float advance = rayHit.ray.tfar + 1e-4f;
                rayOrigin = hitPos + rayDir * 1e-4f;
                if (currentRayDiff.hasDifferentials) {
                    currentRayDiff.rxOrigin += rayDir * advance;
                    currentRayDiff.ryOrigin += rayDir * advance;
                }
                --bounce;
                isFirstBounce = false;
                lastBsdfPdf = 0.0f;
                continue;
            }
            // We chose to interact; set opacity to 1 so that
            // EvalSurface / SampleSurface don't double-count.
            closure.opacity = 1.0f;
        }

        // --- Emissive ---
        if (hasClosure) {
            radiance += GfCompMult(throughput, _ToGf(closure.emissiveColor));
        }

        // --- Direct lighting (NEE) with MIS ---
        GfVec3f direct(0.0f);
        if (hasClosure) {
            direct = _ComputeDirectLightingMIS(
                hitPos, normal, wo, sampler, doubleSided, &closure);
        } else {
            GfVec3f matColor = _enableSceneColors
                ? _ToGf(ctx.displayColor) : GfVec3f(0.5f);
            mxcpp::SurfaceClosure fallback;
            fallback.baseColor = _ToMx(matColor);
            fallback.roughness = 1.0f;
            fallback.metallic = 0.0f;
            fallback.specular = 0.0f;
            fallback.specularColor = mxcpp::Vec3f(1.0f);
            fallback.specularIor = 1.5f;
            fallback.opacity = 1.0f;
            direct = _ComputeDirectLightingMIS(
                hitPos, normal, wo, sampler, doubleSided, &fallback);
        }
        radiance += GfCompMult(throughput, direct);

        // --- Stop after last allowed bounce ---
        if (bounce >= _maxBounces) break;

        // --- BSDF sampling for next direction ---
        if (!hasClosure) break;

        mxcpp::Bsdf::BsdfSample bs = mxcpp::Bsdf::SampleSurface(
            closure, _ToMx(normal), _ToMx(wo),
            sampler.Next(), sampler.Next(), sampler.Next());
        if (bs.pdf <= 0.0f) break;

        GfVec3f bsdfContrib;
        if (bs.isSpecular) {
            // Delta distribution (e.g. thin-surface transmission):
            // f already contains the throughput coefficient; no cosine
            // or pdf division needed.
            bsdfContrib = _ToGf(bs.f);
        } else {
            float cosTheta = std::abs(GfDot(normal, _ToGf(bs.wi)));
            bsdfContrib = _ToGf(bs.f) * cosTheta / bs.pdf;
        }

        for (int i = 0; i < 3; ++i) {
            if (!std::isfinite(bsdfContrib[i]) || bsdfContrib[i] < 0.0f)
                bsdfContrib[i] = 0.0f;
        }

        throughput = GfCompMult(throughput, bsdfContrib);

        lastBsdfPdf = bs.isSpecular ? 0.0f : bs.pdf;
        if (!bs.isSpecular) {
            anyNonSpecularBounces = true;
        }
        isFirstBounce = false;

        // --- Russian Roulette ---
        if (bounce >= _minBouncesBeforeRR) {
            float q = std::max({throughput[0], throughput[1], throughput[2]});
            q = std::min(q, 0.95f);
            if (q <= 0.0f || sampler.Next() > q) break;
            throughput /= q;
        }

        // --- Propagate ray differentials ---
        if (currentRayDiff.hasDifferentials && bs.isSpecular) {
            GfVec3f wi = _ToGf(bs.wi);
            GfVec3f dndx = lastDndu * lastDudx + lastDndv * lastDvdx;
            GfVec3f dndy = lastDndu * lastDudy + lastDndv * lastDvdy;

            currentRayDiff.rxOrigin = hitPos + lastDpdx;
            currentRayDiff.ryOrigin = hitPos + lastDpdy;

            GfVec3f dwodx = -currentRayDiff.rxDirection - wo;
            GfVec3f dwody = -currentRayDiff.ryDirection - wo;

            // Check if this is a refraction (transmitted ray)
            bool isRefraction = bs.eta != 1.0f;

            if (!isRefraction) {
                // Specular reflection differential
                float dwoDotn_dx = GfDot(dwodx, normal) + GfDot(wo, dndx);
                float dwoDotn_dy = GfDot(dwody, normal) + GfDot(wo, dndy);
                currentRayDiff.rxDirection =
                    wi - dwodx + 2.0f * (GfDot(wo, normal) * dndx +
                                          dwoDotn_dx * normal);
                currentRayDiff.ryDirection =
                    wi - dwody + 2.0f * (GfDot(wo, normal) * dndy +
                                          dwoDotn_dy * normal);
            } else {
                // Specular refraction differential (Snell's law)
                float eta = bs.eta;
                if (eta != 0.0f) {
                    float dwoDotn_dx = GfDot(dwodx, normal) + GfDot(wo, dndx);
                    float dwoDotn_dy = GfDot(dwody, normal) + GfDot(wo, dndy);
                    float mu = GfDot(wo, normal) / eta
                             - std::abs(GfDot(wi, normal));
                    float wiDotN = GfDot(wi, normal);
                    float dmudx = dwoDotn_dx * (1.0f / eta +
                        1.0f / (eta * eta) * GfDot(wo, normal) /
                        (wiDotN != 0.0f ? wiDotN : 1.0f));
                    float dmudy = dwoDotn_dy * (1.0f / eta +
                        1.0f / (eta * eta) * GfDot(wo, normal) /
                        (wiDotN != 0.0f ? wiDotN : 1.0f));
                    currentRayDiff.rxDirection =
                        wi - eta * dwodx +
                        (mu * dndx + dmudx * normal);
                    currentRayDiff.ryDirection =
                        wi - eta * dwody +
                        (mu * dndy + dmudy * normal);
                } else {
                    currentRayDiff.hasDifferentials = false;
                }
            }

            // Squash check: kill differentials if they explode
            if (currentRayDiff.rxDirection.GetLengthSq() > 1e16f ||
                currentRayDiff.ryDirection.GetLengthSq() > 1e16f ||
                currentRayDiff.rxOrigin.GetLengthSq() > 1e16f ||
                currentRayDiff.ryOrigin.GetLengthSq() > 1e16f) {
                currentRayDiff.hasDifferentials = false;
            }
        } else if (!bs.isSpecular) {
            // Non-specular bounce: disable differentials
            currentRayDiff.hasDifferentials = false;
        }

        // --- Next ray ---
        float bias = (GfDot(_ToGf(bs.wi), normal) > 0.0f) ? 1e-4f : -1e-4f;
        rayOrigin = hitPos + normal * bias;
        rayDir = _ToGf(bs.wi);
    }

    return radiance;
}

PXR_NAMESPACE_CLOSE_SCOPE
