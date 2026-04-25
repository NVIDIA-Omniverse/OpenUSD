//
// Copyright 2018 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/imaging/plugin/hdEmbree/renderer.h"
#include "pxr/imaging/plugin/hdEmbree/oiioTextureSystem.h"

#include "pxr/imaging/plugin/hdEmbree/config.h"
#include "pxr/imaging/plugin/hdEmbree/renderDelegate.h"
#include "pxr/imaging/plugin/hdEmbree/light.h"
#include "pxr/imaging/plugin/hdEmbree/lightSamplers.h"
#include "pxr/imaging/plugin/hdEmbree/material.h"
#include "pxr/imaging/plugin/hdEmbree/mesh.h"
#include "pxr/imaging/plugin/hdEmbree/renderBuffer.h"
#include "pxr/imaging/plugin/hdEmbree/sss.h"
#include "pxr/imaging/plugin/hdEmbree/MaterialXCpp/materials/bsdf.h"
#include "pxr/imaging/plugin/hdEmbree/MaterialXCpp/shadingContext.h"
#include "pxr/imaging/plugin/hdEmbree/MaterialXCpp/spectral.h"

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
constexpr float _volumePdfEps = 1.0e-20f;
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

inline bool
_IsNearlyBlack(const GfVec3f& value, float threshold = 1.0e-4f)
{
    return value[0] <= threshold &&
           value[1] <= threshold &&
           value[2] <= threshold;
}

inline float
_GetOneSampleMisLightPdf(float lightPdf, int sampleCount, bool applyCount)
{
    if (lightPdf <= 0.0f) {
        return 0.0f;
    }
    return applyCount ? lightPdf * static_cast<float>(sampleCount) : lightPdf;
}

inline mxcpp::Mat4f
_ToMx(const GfMatrix4d& m)
{
    mxcpp::Mat4f result;
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            result[row][col] = static_cast<float>(m[row][col]);
        }
    }
    return result;
}

struct _HeroWavelengthState
{
    bool active = false;
    float wavelengthNm = 0.0f;
    float pdf = 0.0f;
};

inline float
_RgbToSpectralValue(const GfVec3f& rgb, const _HeroWavelengthState& hero)
{
    return mxcpp::Spectral::RgbToSpectralValue(_ToMx(rgb), hero.wavelengthNm);
}

inline GfVec3f
_SpectralValueToRgb(float value, const _HeroWavelengthState& hero)
{
    return _ToGf(mxcpp::Spectral::SpectralValueToRgb(
        value,
        hero.wavelengthNm,
        hero.pdf));
}

inline GfVec3f
_SpectralScalarToRgb(float value, const _HeroWavelengthState& hero)
{
    return hero.active ? _SpectralValueToRgb(value, hero)
                       : GfVec3f(value);
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
        GfMatrix4f val;
        if (sampler->Sample(data->primID, data->u, data->v, &val)) {
            return mxcpp::Value(_ToMx(val));
        }
    }
    {
        GfMatrix4d val;
        if (sampler->Sample(data->primID, data->u, data->v, &val)) {
            return mxcpp::Value(_ToMx(val));
        }
    }
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
    , _enableGgxMicrofacetMultipleScattering(true)
    , _useAdobeOpenPBR(false)
    , _textureSystem(std::make_unique<HdEmbreeOiioTextureSystem>())
    , _sceneFrame(0.0f)
    , _sceneTime(0.0f)
    , _completedSamples(0)
    , _sssCallCount(0)
    , _sssSuccessCount(0)
    , _sssWalkStepCount(0)
    , _sssIntersectionCount(0)
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
HdEmbreeRenderer::SetEnableGgxMicrofacetMultipleScattering(bool enable)
{
    _enableGgxMicrofacetMultipleScattering = enable;
    mxcpp::Bsdf::SetGgxMicrofacetMultipleScatteringEnabled(enable);
}

void
HdEmbreeRenderer::SetUseAdobeOpenPBR(bool enable)
{
    _useAdobeOpenPBR = enable;
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
HdEmbreeRenderer::ResetAccumulation()
{
    if (!_ValidateAovBindings()) {
        return;
    }

    for (size_t i = 0; i < _aovBindings.size(); ++i) {
        HdEmbreeRenderBuffer *rb =
            static_cast<HdEmbreeRenderBuffer*>(_aovBindings[i].renderBuffer);
        rb->ClearSamples();
        rb->SetConverged(false);
    }

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

uint64_t
HdEmbreeRenderer::GetSssCallCount() const
{
    return _sssCallCount.load();
}

uint64_t
HdEmbreeRenderer::GetSssSuccessCount() const
{
    return _sssSuccessCount.load();
}

uint64_t
HdEmbreeRenderer::GetSssWalkStepCount() const
{
    return _sssWalkStepCount.load();
}

uint64_t
HdEmbreeRenderer::GetSssIntersectionCount() const
{
    return _sssIntersectionCount.load();
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
    _sssCallCount.store(0);
    _sssSuccessCount.store(0);
    _sssWalkStepCount.store(0);
    _sssIntersectionCount.store(0);

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

    const unsigned int tileSize = HdEmbreeConfig::GetInstance().tileSize;
    const unsigned int numTilesX =
        (_dataWindow.GetWidth() + tileSize - 1) / tileSize;
    const unsigned int numTilesY =
        (_dataWindow.GetHeight() + tileSize - 1) / tileSize;

    // ---- Coarse preview passes ----
    // Render a sparse subset of pixels and block-fill the display buffer
    // so the user sees a mosaic preview almost immediately, then refine.
    {
        static const unsigned int kPreviewStrides[] = {8, 4, 2};
        for (unsigned int stride : kPreviewStrides) {
            if (renderThread->IsStopRequested()) {
                break;
            }

            // Only run coarse passes that are coarser than a single pixel.
            if (stride >= static_cast<unsigned int>(_dataWindow.GetWidth()) &&
                stride >= static_cast<unsigned int>(_dataWindow.GetHeight())) {
                continue;
            }

            WorkParallelForN(numTilesX * numTilesY,
                std::bind(&HdEmbreeRenderer::_RenderTiles, this,
                    renderThread, /*sampleNum=*/0, baseSeed, stride,
                    std::placeholders::_1, std::placeholders::_2));

            if (renderThread->IsStopRequested()) {
                break;
            }

            // Resolve sparse samples into the display buffer and
            // replicate each sampled pixel across its block.
            {
                auto lock = renderThread->LockFramebuffer();
                for (size_t a = 0; a < _aovBindings.size(); ++a) {
                    HdEmbreeRenderBuffer *rb =
                        static_cast<HdEmbreeRenderBuffer*>(
                            _aovBindings[a].renderBuffer);
                    rb->Resolve();
                    rb->BlockFill(stride);
                }
            }
        }

        // Clear the sample accumulation so the full-resolution passes
        // start from a clean slate, while the display buffer retains
        // the coarse preview for visual continuity.
        if (!renderThread->IsStopRequested()) {
            for (size_t a = 0; a < _aovBindings.size(); ++a) {
                HdEmbreeRenderBuffer *rb =
                    static_cast<HdEmbreeRenderBuffer*>(
                        _aovBindings[a].renderBuffer);
                rb->ClearSamples();
            }
            // Reset adaptive sampling state that was partially filled
            // by the coarse passes.
            std::fill(_pixelMean.begin(), _pixelMean.end(), GfVec3f(0.0f));
            std::fill(_pixelM2.begin(), _pixelM2.end(), GfVec3f(0.0f));
            std::fill(
                _pixelSampleCount.begin(), _pixelSampleCount.end(), 0);
            std::fill(
                _pixelConverged.begin(), _pixelConverged.end(), false);
        }
    }

    // ---- Full-resolution multi-sample rendering ----
    // Each pass adds one sample per pixel.  After every pass we resolve
    // the accumulation buffer so the display shows progressively
    // improving quality.
    bool renderFinished = false;
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

        WorkParallelForN(numTilesX * numTilesY,
            std::bind(&HdEmbreeRenderer::_RenderTiles, this,
                renderThread, i, baseSeed, /*stride=*/1u,
                std::placeholders::_1, std::placeholders::_2));

        // Resolve intermediate results so the viewport shows progressive
        // refinement instead of staying blank until convergence.
        {
            auto lock = renderThread->LockFramebuffer();
            for (size_t a = 0; a < _aovBindings.size(); ++a) {
                HdEmbreeRenderBuffer *rb =
                    static_cast<HdEmbreeRenderBuffer*>(
                        _aovBindings[a].renderBuffer);
                rb->Resolve();
            }
        }

        // After the first pass, mark the single-sampled attachments as
        // converged and unmap them. If there are no multisampled attachments,
        // we are done.
        if (i == 0) {
            bool moreWork = false;
            for (size_t a = 0; a < _aovBindings.size(); ++a) {
                HdEmbreeRenderBuffer *rb = static_cast<HdEmbreeRenderBuffer*>(
                    _aovBindings[a].renderBuffer);
                if (rb->IsMultiSampled()) {
                    moreWork = true;
                }
            }
            if (!moreWork) {
                _completedSamples.store(i + 1);
                renderFinished = true;
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
                renderFinished = true;
                break;
            }
        }

        // Cancellation point.
        if (renderThread->IsStopRequested()) {
            break;
        }

        // If this is the last iteration, rendering completed naturally.
        if (i == _samplesToConvergence - 1) {
            renderFinished = true;
        }
    }

    // Mark the multisampled attachments as converged and unmap all buffers.
    for (size_t i = 0; i < _aovBindings.size(); ++i) {
        HdEmbreeRenderBuffer *rb = static_cast<HdEmbreeRenderBuffer*>(
            _aovBindings[i].renderBuffer);
        rb->Unmap();
        rb->SetConverged(true);
    }

    // Print render statistics only when rendering completed (not interrupted).
    if (renderFinished) {
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
        const uint64_t sssCalls = _sssCallCount.load();
        if (sssCalls > 0) {
            const uint64_t sssSuccesses = _sssSuccessCount.load();
            const uint64_t sssWalkSteps = _sssWalkStepCount.load();
            const uint64_t sssIntersections = _sssIntersectionCount.load();
            const double successRate =
                100.0 * static_cast<double>(sssSuccesses)
                / static_cast<double>(sssCalls);
            const double avgStepsPerCall =
                static_cast<double>(sssWalkSteps)
                / static_cast<double>(sssCalls);
            const double avgStepsPerSuccess =
                (sssSuccesses > 0)
                    ? static_cast<double>(sssWalkSteps)
                        / static_cast<double>(sssSuccesses)
                    : 0.0;
            const double avgIntersectionsPerCall =
                static_cast<double>(sssIntersections)
                / static_cast<double>(sssCalls);

            std::printf(
                "  SSS walks        : %llu calls, %llu success (%.1f%%)\n",
                static_cast<unsigned long long>(sssCalls),
                static_cast<unsigned long long>(sssSuccesses),
                successRate);
            std::printf(
                "  SSS avg steps    : %.2f / call, %.2f / success\n",
                avgStepsPerCall,
                avgStepsPerSuccess);
            std::printf(
                "  SSS intersects   : %llu total, %.2f / call\n",
                static_cast<unsigned long long>(sssIntersections),
                avgIntersectionsPerCall);
        }

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
                               uint32_t baseSeed, unsigned int stride,
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
            // For coarse preview passes, only render sparse pixels
            // whose data-window-relative coordinates are multiples
            // of the stride.
            if (stride > 1 && ((y - minY) % stride != 0)) {
                continue;
            }
            for (unsigned int x = x0; x < x1; ++x) {
                if (stride > 1 && ((x - minX) % stride != 0)) {
                    continue;
                }

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
    if (!self->_ComputeId(rayHit, w.token, &id)) {
        id = -1;
    }
    w.buffer->Write(GfVec3i(x, y, 1), 1, &id);
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
    ctx.viewPosition = _ToMx(GfVec3f(_inverseViewMatrix.Transform(GfVec3f(0.0f))));
    ctx.texcoord = _ToMx(texcoordVal);
    ctx.displayColor = _ToMx(displayColor);
    ctx.displayOpacity = 1.0f;
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
    HdEmbreeMesh** outMesh) const
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
    if (outMesh) {
        *outMesh = dynamic_cast<HdEmbreeMesh*>(prototypeContext->rprim);
    }
    if (!material || !outClosure) return false;

    mxcpp::EvalGraph *evalGraph = material->GetEvalGraph();
    if (!evalGraph) return false;

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
            &prototypeContext->primvarMap,
            rayHit.hit.primID, rayHit.hit.u, rayHit.hit.v};
        ctx.geomPropLookup = &_SampleGeomProp;
        ctx.geomPropUserData = &cbData;
        ctx.uniformProps = &prototypeContext->uniformPrimvarMap;
        mxcpp::EvalOptions evalOptions;
        evalOptions.useAdobeOpenPBR = _useAdobeOpenPBR;
        *outClosure = evalGraph->Evaluate(ctx, evalOptions);
        return true;
    } catch (...) {
        return false;
    }
}

GfVec3f
HdEmbreeRenderer::_Visibility(
    GfVec3f const& position,
    GfVec3f const& normal,
    GfVec3f const& direction,
    float dist,
    HdEmbreeMediumState const& mediumState) const
{
    constexpr int kMaxTransparentHits = 16;
    constexpr float kVisThreshold = 1e-4f;
    constexpr float kRayBias = 1e-4f;

    GfVec3f visibility(1.0f);
    HdEmbreeMediumState shadowMedium = mediumState;
    GfVec3f rayOrigin = _OffsetRayOrigin(position, normal, direction, kRayBias);
    float remaining = dist;

    for (int i = 0; i < kMaxTransparentHits; ++i) {
        RTCRayHit rayHit;
        rayHit.ray.flags = 0;
        _PopulateRayHit(&rayHit, rayOrigin, direction, kRayBias, remaining,
                        HdEmbree_RayMask::Camera);
        rtcIntersect1(_scene, &rayHit);

        if (rayHit.hit.geomID == RTC_INVALID_GEOMETRY_ID) {
            if (shadowMedium.active && remaining > 0.0f) {
                visibility = GfCompMult(
                    visibility,
                    _ToGf(mxcpp::EvalBeerTransmittance(
                        shadowMedium.medium,
                        remaining)));
            }
            return visibility;
        }

        const float hitDist = std::min(rayHit.ray.tfar, remaining);
        if (shadowMedium.active && hitDist > 0.0f) {
            visibility = GfCompMult(
                visibility,
                _ToGf(mxcpp::EvalBeerTransmittance(
                    shadowMedium.medium,
                    hitDist)));
        }
        if (_IsNearlyBlack(visibility, kVisThreshold)) {
            return GfVec3f(0.0f);
        }

        mxcpp::SurfaceClosure closure;
        GfVec3f hitNormal(0.0f);
        HdEmbreeMesh* hitMesh = nullptr;
        const bool hasClosure = _TryEvalSurfaceClosureAtHit(
            rayHit, &closure, &hitNormal, &hitMesh);

        float surfaceVisibility = hasClosure ? (1.0f - closure.opacity) : 0.0f;
        if (hasClosure) {
            surfaceVisibility = std::max(surfaceVisibility, closure.transmission);
        }
        visibility *= surfaceVisibility;

        if (_IsNearlyBlack(visibility, kVisThreshold)) {
            return GfVec3f(0.0f);
        }

        remaining -= hitDist;
        if (remaining <= 0.001f) {
            return visibility;
        }

        if (shadowMedium.active && hitMesh == shadowMedium.ownerMesh) {
            shadowMedium = HdEmbreeMediumState();
        } else if (!shadowMedium.active &&
                   hasClosure &&
                   closure.hasInteriorMedium &&
                   closure.transmission > 0.0f &&
                   hitMesh &&
                   GfDot(direction, hitNormal) < 0.0f) {
            shadowMedium.active = true;
            shadowMedium.medium = closure.interiorMedium;
            shadowMedium.ownerMesh = hitMesh;
        }

        GfVec3f hitPos = GfVec3f(
            rayHit.ray.org_x + hitDist * rayHit.ray.dir_x,
            rayHit.ray.org_y + hitDist * rayHit.ray.dir_y,
            rayHit.ray.org_z + hitDist * rayHit.ray.dir_z);
        rayOrigin = _OffsetRayOrigin(
            hitPos,
            hasClosure ? hitNormal : normal,
            direction,
            kRayBias);
    }

    return visibility;
}

bool
HdEmbreeRenderer::_FindNearestFiniteLightHit(
    GfVec3f const& position,
    GfVec3f const& direction,
    float maxDist,
    HdEmbreeLightSampler::LightSample* outSample) const
{
    if (!outSample || maxDist <= 0.0f) {
        return false;
    }

    bool found = false;
    float closestDist = maxDist;
    HdEmbreeLightSampler::LightSample closestSample{};

    for (auto const& it : _lightMap) {
        if (!it.second || it.second->IsDome()) {
            continue;
        }

        auto const& light = it.second->LightData();
        if (!light.visible) {
            continue;
        }

        const HdEmbreeLightSampler::LightSample ls =
            HdEmbreeLightSampler::EvaluateLightDirection(
                light, position, direction);
        if (!ls.valid || ls.dist <= 0.0f || !std::isfinite(ls.dist)) {
            continue;
        }
        if (ls.dist >= closestDist) {
            continue;
        }

        closestDist = ls.dist;
        closestSample = ls;
        found = true;
    }

    if (found) {
        *outSample = closestSample;
    }
    return found;
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
            // we're only evaluating domes along the camera ray direction.
            HdEmbreeLightSampler::LightSample ls =
                HdEmbreeLightSampler::EvaluateDomeLightDirection(
                    dome->LightData(),
                    GfVec3f(
                        rayHit.ray.dir_x,
                        rayHit.ray.dir_y,
                        rayHit.ray.dir_z));
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
            mxcpp::EvalOptions evalOptions;
            evalOptions.useAdobeOpenPBR = _useAdobeOpenPBR;
            closure = evalGraph->Evaluate(ctx, evalOptions);
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
    bool /*doubleSided*/,
    mxcpp::SurfaceClosure const* closure,
    HdEmbreeMediumState const& mediumState,
    bool spectralActive,
    float heroWavelengthNm,
    float heroWavelengthPdf) const
{
    const _HeroWavelengthState hero{
        spectralActive, heroWavelengthNm, heroWavelengthPdf};
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

            // Keep the BSDF normal fixed relative to wo. Reflection lobes
            // reject backside wi internally, while transmission lobes need
            // those samples to survive direct-light evaluation.
            const float absDotNL = std::abs(GfDot(ls.wI, normal));
            if (absDotNL <= 0.0f) {
                continue;
            }

            GfVec3f vis = _Visibility(
                position, normal, ls.wI, ls.dist * 0.99f, mediumState);
            if (_IsNearlyBlack(vis)) {
                continue;
            }

            GfVec3f sampleContrib(0.0f);
            if (closure) {
                GfVec3f bsdfValue = _ToGf(mxcpp::Bsdf::EvalSurface(
                    *closure,
                    _ToMx(normal),
                    _ToMx(ls.wI),
                    _ToMx(wo),
                    heroWavelengthNm));

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
                    *closure,
                    _ToMx(normal),
                    _ToMx(ls.wI),
                    _ToMx(wo),
                    heroWavelengthNm);
                float misW = mxcpp::Bsdf::PowerHeuristic(lightPdf, bsdfPdf);

                if (hero.active) {
                    const float spectralLi = _RgbToSpectralValue(ls.Li, hero);
                    const float spectralVis = _RgbToSpectralValue(vis, hero);
                    const float spectralBsdf =
                        _RgbToSpectralValue(bsdfValue, hero);
                    sampleContrib = _SpectralValueToRgb(
                        spectralLi * spectralBsdf *
                            absDotNL * spectralVis * ls.invPdfW * misW,
                        hero);
                } else {
                    sampleContrib = GfCompMult(
                        GfCompMult(ls.Li, bsdfValue),
                        vis) * absDotNL * ls.invPdfW * misW;
                }
            } else {
                float brdf = 1.0f / _pi<float>;
                if (hero.active) {
                    const float spectralLi = _RgbToSpectralValue(ls.Li, hero);
                    const float spectralVis = _RgbToSpectralValue(vis, hero);
                    sampleContrib = _SpectralValueToRgb(
                        spectralLi * absDotNL * brdf * spectralVis * ls.invPdfW,
                        hero);
                } else {
                    sampleContrib = GfCompMult(ls.Li, vis)
                        * absDotNL * brdf * ls.invPdfW;
                }
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

GfVec3f
HdEmbreeRenderer::_ComputeMediumDirectLighting(
    GfVec3f const& position,
    GfVec3f const& wo,
    HdEmbreeMediumState const& mediumState,
    HdEmbreeSobolSampler& sampler,
    bool spectralActive,
    float heroWavelengthNm,
    float heroWavelengthPdf) const
{
    const _HeroWavelengthState hero{
        spectralActive, heroWavelengthNm, heroWavelengthPdf};
    if (!mediumState.active || mediumState.medium.IsAbsorbingOnly()) {
        return GfVec3f(0.0f);
    }

    GfVec3f finalColor(0.0f);
    const int N = _lightSamplesPerHit;
    const float invN = 1.0f / static_cast<float>(N);

    int stratDimU = 1, stratDimV = 1;
    if (_stratifyLightSamples && N > 1) {
        stratDimU = static_cast<int>(std::sqrt(static_cast<float>(N)));
        if (stratDimU < 1) {
            stratDimU = 1;
        }
        stratDimV = (N + stratDimU - 1) / stratDimU;
    }

    for (auto const& it : _lightMap) {
        auto const& light = it.second->LightData();
        if (!light.visible) {
            continue;
        }

        GfVec3f lightContrib(0.0f);
        for (int s = 0; s < N; ++s) {
            float u1 = 0.0f;
            float u2 = 0.0f;
            if (_stratifyLightSamples && N > 1) {
                int su = s % stratDimU;
                int sv = s / stratDimU;
                u1 = (su + sampler.Next()) / static_cast<float>(stratDimU);
                u2 = (sv + sampler.Next()) / static_cast<float>(stratDimV);
            } else {
                u1 = sampler.Next();
                u2 = sampler.Next();
            }

            const HdEmbreeLightSampler::LightSample ls =
                HdEmbreeLightSampler::GetLightSample(
                    light,
                    position,
                    GfVec3f(0.0f),
                    u1,
                    u2);
            if (GfIsClose(ls.Li, GfVec3f(0.0f), _minLuminanceCutoff)) {
                continue;
            }

            const GfVec3f vis = _Visibility(
                position, ls.wI, ls.wI, ls.dist * 0.99f, mediumState);
            if (_IsNearlyBlack(vis)) {
                continue;
            }

            const float phasePdf = mxcpp::PdfHenyeyGreenstein(
                _ToMx(ls.wI),
                _ToMx(wo),
                mediumState.medium.anisotropy);
            if (phasePdf <= 0.0f) {
                continue;
            }

            float misW = 1.0f;
            if (ls.invPdfW > 0.0f) {
                const float lightPdf = 1.0f / ls.invPdfW;
                const float effectiveLightPdf = _GetOneSampleMisLightPdf(
                    lightPdf,
                    N,
                    true);
                if (effectiveLightPdf > 0.0f) {
                    misW = mxcpp::Bsdf::PowerHeuristic(
                        effectiveLightPdf,
                        phasePdf);
                }
            }

            GfVec3f sampleContrib(0.0f);
            if (hero.active) {
                const float spectralLi = _RgbToSpectralValue(ls.Li, hero);
                const float spectralVis = _RgbToSpectralValue(vis, hero);
                sampleContrib = _SpectralValueToRgb(
                    spectralLi * spectralVis * phasePdf * ls.invPdfW * misW,
                    hero);
            } else {
                sampleContrib =
                    GfCompMult(ls.Li, vis) * phasePdf * ls.invPdfW * misW;
            }

            if (_fireflyClampThreshold > 0.0f) {
                const float lum = 0.2126f * sampleContrib[0]
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

HdEmbreeRenderer::_VolumeTransmissionResult
HdEmbreeRenderer::_TraceVolumeTransmission(
    _VolumeTransmissionInput const& input,
    HdEmbreeMediumState const& mediumState,
    HdEmbreeSobolSampler& sampler,
    _VolumeTransmissionState* state) const
{
    if (!state || !mediumState.active) {
        return _VolumeTransmissionResult::ContinueSurface;
    }

    const _HeroWavelengthState hero{
        input.spectralActive,
        input.heroWavelengthNm,
        input.heroWavelengthPdf};

    const auto applyWeight = [&](GfVec3f const& weight) {
        if (hero.active) {
            state->spectralThroughput *= _RgbToSpectralValue(weight, hero);
        } else {
            state->throughput = GfCompMult(state->throughput, weight);
        }
    };

    const auto rgbThroughput = [&]() {
        return hero.active
            ? _SpectralScalarToRgb(state->spectralThroughput, hero)
            : state->throughput;
    };

    const auto throughputIsBlack = [&]() {
        return _IsNearlyBlack(rgbThroughput(), _minLuminanceCutoff);
    };

    const auto addFiniteLightHit = [&]() {
        GfVec3f lightContrib = input.finiteLightHit.Li;
        if (state->lastBsdfPdf > 0.0f &&
            input.finiteLightHit.invPdfW > 0.0f) {
            const float lightPdf = 1.0f / input.finiteLightHit.invPdfW;
            const float effectiveLightPdf = _GetOneSampleMisLightPdf(
                lightPdf,
                _lightSamplesPerHit,
                state->lastScatterWasMedium);
            if (effectiveLightPdf > 0.0f) {
                lightContrib *= mxcpp::Bsdf::PowerHeuristic(
                    state->lastBsdfPdf,
                    effectiveLightPdf);
            }
        }

        if (hero.active) {
            const float spectralLight =
                _RgbToSpectralValue(lightContrib, hero);
            state->radiance += _SpectralValueToRgb(
                state->spectralThroughput * spectralLight,
                hero);
        } else {
            state->radiance += GfCompMult(state->throughput, lightContrib);
        }

        return _VolumeTransmissionResult::Terminate;
    };

    const mxcpp::MediumProperties& medium = mediumState.medium;

    if (!medium.IsAbsorbingOnly()) {
        const GfVec3f sigmaT = _ToGf(medium.SigmaT());
        const GfVec3f sigmaS = _ToGf(medium.sigmaS);
        GfVec3f albedo(0.0f);
        for (int i = 0; i < 3; ++i) {
            if (sigmaT[i] > 1.0e-6f) {
                albedo[i] = std::clamp(sigmaS[i] / sigmaT[i], 0.0f, 1.0f);
            }
        }

        mxcpp::Vec3f channelPdfMx;
        const int channel = mxcpp::ChannelMIS(
            _ToMx(rgbThroughput()),
            _ToMx(albedo),
            sampler.Next(),
            &channelPdfMx);
        const GfVec3f channelPdf =
            GfVec3f(channelPdfMx[0], channelPdfMx[1], channelPdfMx[2]);

        // Chiang channel MIS: sample a single RGB tracking channel, but
        // evaluate all RGB channels against the mixture pdf.
        const auto evalTransmittance = [&](float distance) {
            return _ToGf(mxcpp::EvalBeerTransmittance(medium, distance));
        };

        const auto evalScatterWeight = [&](float distance) {
            const GfVec3f transmittance = evalTransmittance(distance);
            const GfVec3f pdf = GfCompMult(sigmaT, transmittance);
            const GfVec3f sampleContrib =
                GfCompMult(sigmaS, transmittance);
            const float denom = GfDot(channelPdf, pdf);
            if (!std::isfinite(denom) || denom <= _volumePdfEps) {
                return GfVec3f(0.0f);
            }
            return sampleContrib * (1.0f / denom);
        };

        const auto evalTransmittanceWeight = [&](float distance) {
            const GfVec3f transmittance = evalTransmittance(distance);
            const float denom = GfDot(channelPdf, transmittance);
            if (!std::isfinite(denom) || denom <= _volumePdfEps) {
                return GfVec3f(0.0f);
            }
            return transmittance * (1.0f / denom);
        };

        const float scatterDist =
            mxcpp::SampleFreeFlightChannel(medium, channel, sampler.Next());
        const float maxTravelDist =
            std::min(input.surfaceDist, input.finiteLightDist);
        if (scatterDist < maxTravelDist) {
            const GfVec3f scatterWeight = evalScatterWeight(scatterDist);
            applyWeight(scatterWeight);

            if (throughputIsBlack()) {
                return _VolumeTransmissionResult::Terminate;
            }

            const GfVec3f scatterPos =
                input.rayOrigin + input.rayDir * scatterDist;
            const GfVec3f wo = -input.rayDir;
            const GfVec3f direct = _ComputeMediumDirectLighting(
                scatterPos,
                wo,
                mediumState,
                sampler,
                hero.active,
                hero.wavelengthNm,
                hero.pdf);
            if (hero.active) {
                state->radiance += direct * state->spectralThroughput;
            } else {
                state->radiance += GfCompMult(state->throughput, direct);
            }

            if (input.bounce >= _maxBounces) {
                return _VolumeTransmissionResult::Terminate;
            }

            if (input.bounce >= _minBouncesBeforeRR) {
                float q = hero.active
                    ? std::max({
                        _SpectralScalarToRgb(
                            state->spectralThroughput, hero)[0],
                        _SpectralScalarToRgb(
                            state->spectralThroughput, hero)[1],
                        _SpectralScalarToRgb(
                            state->spectralThroughput, hero)[2]})
                    : std::max({
                        state->throughput[0],
                        state->throughput[1],
                        state->throughput[2]});
                q = std::min(q, 0.95f);
                if (q <= 0.0f || sampler.Next() > q) {
                    return _VolumeTransmissionResult::Terminate;
                }
                if (hero.active) {
                    state->spectralThroughput /= q;
                } else {
                    state->throughput /= q;
                }
            }

            const GfVec3f wi = _ToGf(mxcpp::SampleHenyeyGreenstein(
                _ToMx(wo),
                medium.anisotropy,
                sampler.Next(),
                sampler.Next()));
            const float phasePdf = mxcpp::PdfHenyeyGreenstein(
                _ToMx(wi),
                _ToMx(wo),
                medium.anisotropy);
            if (phasePdf <= 0.0f || !std::isfinite(phasePdf)) {
                return _VolumeTransmissionResult::Terminate;
            }

            state->rayOrigin =
                _OffsetRayOrigin(scatterPos, wi, wi, 1e-4f);
            state->rayDir = wi;
            state->rayDiff.hasDifferentials = false;
            state->lastBsdfPdf = phasePdf;
            state->lastScatterWasMedium = true;
            state->anyNonSpecularBounces = true;
            state->isFirstBounce = false;
            return _VolumeTransmissionResult::ContinueRay;
        }

        if (input.hasFiniteLightHit &&
            input.finiteLightDist < input.surfaceDist &&
            input.finiteLightDist > 0.0f) {
            applyWeight(evalTransmittanceWeight(input.finiteLightDist));

            if (throughputIsBlack()) {
                return _VolumeTransmissionResult::Terminate;
            }

            return addFiniteLightHit();
        }

        if (std::isfinite(input.surfaceDist) && input.surfaceDist > 0.0f) {
            applyWeight(evalTransmittanceWeight(input.surfaceDist));
        } else {
            return _VolumeTransmissionResult::Terminate;
        }
    } else if (input.hasFiniteLightHit &&
               input.finiteLightDist < input.surfaceDist &&
               input.finiteLightDist > 0.0f) {
        const GfVec3f transmittance = _ToGf(
            mxcpp::EvalBeerTransmittance(medium, input.finiteLightDist));
        applyWeight(transmittance);

        if (throughputIsBlack()) {
            return _VolumeTransmissionResult::Terminate;
        }

        return addFiniteLightHit();
    } else if (std::isfinite(input.surfaceDist) && input.surfaceDist > 0.0f) {
        const GfVec3f transmittance = _ToGf(
            mxcpp::EvalBeerTransmittance(medium, input.surfaceDist));
        applyWeight(transmittance);
    } else {
        return _VolumeTransmissionResult::Terminate;
    }

    if (throughputIsBlack()) {
        return _VolumeTransmissionResult::Terminate;
    }

    return _VolumeTransmissionResult::ContinueSurface;
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
    float spectralThroughput = 1.0f;
    _HeroWavelengthState hero;
    GfVec3f rayOrigin = origin;
    GfVec3f rayDir = dir;
    float lastBsdfPdf = 0.0f;
    bool lastScatterWasMedium = false;
    bool isFirstBounce = true;
    bool anyNonSpecularBounces = false;
    bool useSyntheticLambertian = false;
    unsigned int syntheticLambertianInstanceId = RTC_INVALID_GEOMETRY_ID;
    unsigned int syntheticLambertianGeomId = RTC_INVALID_GEOMETRY_ID;
    HdEmbreeMediumState currentMedium;

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

        const float surfaceDist =
            rayHit.hit.geomID != RTC_INVALID_GEOMETRY_ID
                ? rayHit.ray.tfar
                : std::numeric_limits<float>::infinity();
        HdEmbreeLightSampler::LightSample finiteLightHit{};
        const bool hasFiniteLightHit =
            !isFirstBounce &&
            _FindNearestFiniteLightHit(
                rayOrigin,
                rayDir,
                surfaceDist,
                &finiteLightHit);
        const float finiteLightDist =
            hasFiniteLightHit
                ? finiteLightHit.dist
                : std::numeric_limits<float>::infinity();

        if (currentMedium.active) {
            _VolumeTransmissionInput volumeInput;
            volumeInput.rayOrigin = rayOrigin;
            volumeInput.rayDir = rayDir;
            volumeInput.surfaceDist = surfaceDist;
            volumeInput.hasFiniteLightHit = hasFiniteLightHit;
            volumeInput.finiteLightHit = finiteLightHit;
            volumeInput.finiteLightDist = finiteLightDist;
            volumeInput.bounce = bounce;
            volumeInput.spectralActive = hero.active;
            volumeInput.heroWavelengthNm = hero.wavelengthNm;
            volumeInput.heroWavelengthPdf = hero.pdf;

            _VolumeTransmissionState volumeState;
            volumeState.radiance = radiance;
            volumeState.throughput = throughput;
            volumeState.spectralThroughput = spectralThroughput;
            volumeState.rayOrigin = rayOrigin;
            volumeState.rayDir = rayDir;
            volumeState.rayDiff = currentRayDiff;
            volumeState.lastBsdfPdf = lastBsdfPdf;
            volumeState.lastScatterWasMedium = lastScatterWasMedium;
            volumeState.anyNonSpecularBounces = anyNonSpecularBounces;
            volumeState.isFirstBounce = isFirstBounce;

            const _VolumeTransmissionResult volumeResult =
                _TraceVolumeTransmission(
                    volumeInput,
                    currentMedium,
                    sampler,
                    &volumeState);

            radiance = volumeState.radiance;
            throughput = volumeState.throughput;
            spectralThroughput = volumeState.spectralThroughput;
            rayOrigin = volumeState.rayOrigin;
            rayDir = volumeState.rayDir;
            currentRayDiff = volumeState.rayDiff;
            lastBsdfPdf = volumeState.lastBsdfPdf;
            lastScatterWasMedium = volumeState.lastScatterWasMedium;
            anyNonSpecularBounces = volumeState.anyNonSpecularBounces;
            isFirstBounce = volumeState.isFirstBounce;

            if (volumeResult == _VolumeTransmissionResult::Terminate) {
                break;
            }
            if (volumeResult == _VolumeTransmissionResult::ContinueRay) {
                continue;
            }
        }

        if (hasFiniteLightHit && finiteLightDist < surfaceDist) {
            GfVec3f lightContrib = finiteLightHit.Li;
            if (lastBsdfPdf > 0.0f && finiteLightHit.invPdfW > 0.0f) {
                const float lightPdf = 1.0f / finiteLightHit.invPdfW;
                const float effectiveLightPdf = _GetOneSampleMisLightPdf(
                    lightPdf,
                    _lightSamplesPerHit,
                    lastScatterWasMedium);
                if (effectiveLightPdf > 0.0f) {
                    lightContrib *= mxcpp::Bsdf::PowerHeuristic(
                        lastBsdfPdf,
                        effectiveLightPdf);
                }
            }

            if (hero.active) {
                const float spectralLight =
                    _RgbToSpectralValue(lightContrib, hero);
                radiance += _SpectralValueToRgb(
                    spectralThroughput * spectralLight,
                    hero);
            } else {
                radiance += GfCompMult(throughput, lightContrib);
            }
            break;
        }

        // --- Miss: dome light contribution ---
        if (rayHit.hit.geomID == RTC_INVALID_GEOMETRY_ID) {
            for (auto* dome : _domes) {
                if (!dome->LightData().visible) {
                    continue;
                }
                HdEmbreeLightSampler::LightSample ls =
                    HdEmbreeLightSampler::EvaluateDomeLightDirection(
                        dome->LightData(), rayDir);
                GfVec3f domeContrib = ls.Li;

                if (!isFirstBounce && lastBsdfPdf > 0.0f) {
                    // MIS weight for BSDF sampling strategy hitting dome.
                    float domePdf = (ls.invPdfW > 0.0f)
                        ? 1.0f / ls.invPdfW : 0.0f;
                    domePdf = _GetOneSampleMisLightPdf(
                        domePdf,
                        _lightSamplesPerHit,
                        lastScatterWasMedium);
                    float misW = mxcpp::Bsdf::PowerHeuristic(
                        lastBsdfPdf, domePdf);
                    domeContrib *= misW;
                }

                if (hero.active) {
                    const float spectralDome =
                        _RgbToSpectralValue(domeContrib, hero);
                    radiance += _SpectralValueToRgb(
                        spectralThroughput * spectralDome,
                        hero);
                } else {
                    radiance += GfCompMult(throughput, domeContrib);
                }
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

        // Normals.
        //
        // - `geometricNormal`: unflipped smooth shading normal (interpolated
        //   vertex / subdiv-limit normal, else face Ng). Kept in its
        //   world-oriented form because later stages (medium-boundary
        //   crossing detection, next-ray self-intersection bias) rely on
        //   its sign relative to world orientation rather than relative
        //   to the ray direction.
        // - `faceNg`: the true geometric face normal from Embree, then
        //   face-forwarded toward `wo`. This is Cycles' `sd->Ng` and is
        //   used for validity checks that can't tolerate shading-normal
        //   lies (e.g., rejecting an SSS entry whose refracted direction
        //   is outward relative to the face).
        // - `normal`: the shading normal used for BSDF evaluation /
        //   sampling. Starts from `geometricNormal`, then face-forwarded
        //   against `wo`, then perturbed by the material's tangent-space
        //   normal map. This is Cycles' `sd->N`.
        GfVec3f geometricNormal = _ResolveObjectSpaceNormal(
            prototypeContext, instanceContext->rootScene, rayHit.hit.geomID,
            rayHit);
        geometricNormal =
            instanceContext->objectToWorldMatrix.TransformDir(geometricNormal);
        geometricNormal.Normalize();

        GfVec3f faceNg(rayHit.hit.Ng_x, rayHit.hit.Ng_y, rayHit.hit.Ng_z);
        faceNg =
            instanceContext->objectToWorldMatrix.TransformDir(faceNg);
        faceNg.Normalize();

        GfVec3f normal = geometricNormal;

        GfVec3f wo = -rayDir;

        // Double-sided check
        bool doubleSided = false;
        HdEmbreeMesh *mesh =
            dynamic_cast<HdEmbreeMesh*>(prototypeContext->rprim);
        if (mesh) {
            doubleSided = mesh->EmbreeMeshIsDoubleSided();
        }

        // Face-forward the shading normal and the face Ng against the
        // incoming ray, matching pbrt-v4 and Cycles. USD's doubleSided=
        // false doesn't prescribe back-face behavior in a path tracer;
        // without flipping here, back-face hits would leak light (BSDF
        // sampling would continue through an opaque surface since the
        // sampled hemisphere is oriented around a normal pointing away
        // from wo). Flipping both shading N and face Ng together (Cycles
        // kernel/geom/shader_data.h) also prevents shading-normal
        // terminator artifacts from rejecting SSS entries on otherwise
        // front-facing hits. `geometricNormal` is intentionally left
        // unflipped because downstream medium/bias code needs its world
        // orientation.
        if (GfDot(faceNg, wo) < 0.0f) {
            faceNg = -faceNg;
        }
        if (GfDot(normal, wo) < 0.0f) {
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
                mxcpp::EvalOptions evalOptions;
                evalOptions.useAdobeOpenPBR = _useAdobeOpenPBR;
                closure = evalGraph->Evaluate(ctx, evalOptions);
                hasClosure = true;
            } catch (...) {
                hasClosure = false;
            }
        }

        // SSS exit synthesis: at the exit-side surface hit immediately
        // following HdEmbreeRandomWalkSSS, replace the evaluated closure
        // with a weight-1.0 Lambertian so the random-walk's albedo isn't
        // double-counted. Cycles uses the same trick.
        if (useSyntheticLambertian) {
            const bool hitOwner =
                (rayHit.hit.instID[0] == syntheticLambertianInstanceId) &&
                (rayHit.hit.geomID == syntheticLambertianGeomId);
            if (hitOwner) {
                closure = mxcpp::SurfaceClosure{};
                closure.baseColor = mxcpp::Vec3f(1.0f);
                closure.roughness = 1.0f;
                closure.opacity = 1.0f;
                closure.presence = 1.0f;
                hasClosure = true;
            }
            // Always clear flag; even on mismatch we don't want it to linger.
            useSyntheticLambertian = false;
            syntheticLambertianInstanceId = RTC_INVALID_GEOMETRY_ID;
            syntheticLambertianGeomId = RTC_INVALID_GEOMETRY_ID;
        }

        // Apply material normal map (tangent-space -> world-space).
        if (hasClosure &&
            closure.normal != mxcpp::Vec3f(0.0f, 0.0f, 1.0f)) {
            normal = (tangent   * closure.normal[0] +
                      bitangent * closure.normal[1] +
                      normal    * closure.normal[2]).GetNormalized();
            // Re-orient toward the ray (face-forward, matches the geometric
            // normal treatment above).
            if (GfDot(normal, wo) < 0.0f) {
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
        if (hasClosure && closure.presence < 1.0f) {
            if (sampler.Next() > closure.presence) {
                float advance = rayHit.ray.tfar + 1e-4f;
                rayOrigin = hitPos + rayDir * 1e-4f;
                if (currentRayDiff.hasDifferentials) {
                    currentRayDiff.rxOrigin += rayDir * advance;
                    currentRayDiff.ryOrigin += rayDir * advance;
                }
                --bounce;
                isFirstBounce = false;
                lastBsdfPdf = 0.0f;
                lastScatterWasMedium = false;
                continue;
            }
            // We chose to interact; clear the stochastic presence term so
            // EvalSurface / SampleSurface don't attenuate a second time.
            closure.presence = 1.0f;
        }

        if (hasClosure && closure.HasDispersion() && !hero.active) {
            hero.active = true;
            hero.wavelengthNm =
                mxcpp::Spectral::SampleHeroWavelength(sampler.Next());
            hero.pdf = mxcpp::Spectral::HeroWavelengthPdf();
            spectralThroughput = _RgbToSpectralValue(throughput, hero);
        }

        mxcpp::Bsdf::BsdfSample bs;
        bool hasBsdfSample = false;
        if (hasClosure && bounce < _maxBounces) {
            bs = mxcpp::Bsdf::SampleSurface(
                closure, _ToMx(normal), _ToMx(wo),
                sampler.Next(), sampler.Next(), sampler.Next(),
                hero.wavelengthNm);
            hasBsdfSample = bs.isSubsurface || bs.pdf > 0.0f;
        }

        if (hasClosure &&
            hasBsdfSample &&
            bs.isSubsurface &&
            closure.HasSubsurfaceScattering() &&
            mesh) {

            // Entry direction from BSDF (GGX VNDF refraction via specular
            // roughness + IOR).
            //
            // We pass the face-forwarded shading normal (Cycles `sd->N`),
            // not the raw face Ng. This matches Cycles' `bssrdf->N = sd->N`
            // and keeps normal-map perturbations in the refraction basis.
            // Using the unmodified smooth normal here caused the early
            // `cosNI <= 0` rejection to fire on shading-normal terminator
            // hits, which in turn caused the entire path to break with
            // zero radiance (black artifacts at grazing regions).
            mxcpp::Vec3f entryDirMx;
            if (!mxcpp::Bsdf::SampleSubsurfaceEntry(
                    closure, _ToMx(normal), _ToMx(wo),
                    sampler.Next(), sampler.Next(), entryDirMx)) {
                break;
            }
            const GfVec3f entryDir = _ToGf(entryDirMx);

            // Cycles-parity secondary check: reject if the refracted
            // direction still points outward relative to the true face
            // normal. This can happen when the shading normal (used for
            // the GGX-VNDF + Snell refraction) tilts far enough from the
            // face that an "inward" direction in shading-normal space is
            // actually above the geometric face plane. See
            // `subsurface_bounce()` in
            // Cycles (kernel/integrator/subsurface.h): the corresponding
            // LABEL_NONE also terminates the bounce.
            if (GfDot(faceNg, entryDir) >= 0.0f) {
                break;
            }

            // Entry weight (bs.f = Vec3f(subsurface_weight)).
            if (hero.active) {
                float w = mxcpp::Spectral::RgbToSpectralValue(
                    bs.f, hero.wavelengthNm);
                if (!std::isfinite(w) || w < 0.0f) w = 0.0f;
                spectralThroughput *= w;
            } else {
                GfVec3f w = _ToGf(bs.f);
                for (int i = 0; i < 3; ++i) {
                    if (!std::isfinite(w[i]) || w[i] < 0.0f) w[i] = 0.0f;
                }
                throughput = GfCompMult(throughput, w);
            }

            const GfVec3f rgbTpCheck = hero.active
                ? _SpectralScalarToRgb(spectralThroughput, hero)
                : throughput;
            if (_IsNearlyBlack(rgbTpCheck, _minLuminanceCutoff)) {
                break;
            }

            // Random walk inside the medium. The Dwivedi guide axis is the
            // face-forwarded shading normal (Cycles' `subsurface_state.N =
            // sd->N`); using the raw smooth normal here would leave the
            // guide axis pointing away from `wo` on terminator hits.
            HdEmbreeSssInput sssIn;
            sssIn.entryPos = hitPos;
            sssIn.entryGeomNormal = normal;
            sssIn.entryDir = entryDir;
            sssIn.albedo = _ToGf(closure.subsurfaceColor);
            sssIn.radius = GfCompMult(_ToGf(closure.subsurfaceRadius),
                                      _ToGf(closure.subsurfaceRadiusScale));
            sssIn.anisotropy =
                std::clamp(closure.subsurfaceAnisotropy, -0.99f, 0.99f);
            sssIn.ior = std::max(closure.specularIor, 1.0f);
            sssIn.ownerInstanceId = rayHit.hit.instID[0];
            sssIn.ownerGeomId = rayHit.hit.geomID;

            HdEmbreeSssOutput sssOut = HdEmbreeRandomWalkSSS(
                sssIn, sampler, _scene);
            _sssCallCount.fetch_add(1, std::memory_order_relaxed);
            _sssWalkStepCount.fetch_add(
                sssOut.walkSteps, std::memory_order_relaxed);
            _sssIntersectionCount.fetch_add(
                sssOut.intersectionTests, std::memory_order_relaxed);
            if (sssOut.success) {
                _sssSuccessCount.fetch_add(1, std::memory_order_relaxed);
            }

            if (!sssOut.success) {
                break;
            }

            // Apply the random-walk multiplier to throughput.
            if (hero.active) {
                float w = _RgbToSpectralValue(sssOut.throughputWeight, hero);
                if (!std::isfinite(w) || w < 0.0f) w = 0.0f;
                spectralThroughput *= w;
            } else {
                GfVec3f w = sssOut.throughputWeight;
                for (int i = 0; i < 3; ++i) {
                    if (!std::isfinite(w[i]) || w[i] < 0.0f) w[i] = 0.0f;
                }
                throughput = GfCompMult(throughput, w);
            }

            const GfVec3f rgbTpAfter = hero.active
                ? _SpectralScalarToRgb(spectralThroughput, hero)
                : throughput;
            if (_IsNearlyBlack(rgbTpAfter, _minLuminanceCutoff)) {
                break;
            }

            // Cycles-style ray flip: pretend we come from outside at the
            // exit point. The next iteration will hit the same geometry
            // and synthesize a Lambertian closure for the exit BRDF.
            //
            // The origin offset must be *strictly larger* than the next
            // ray cast's tnear (1e-4 for non-first-bounce rays); otherwise
            // the intended exit hit lands at t == tnear, exposing FP
            // precision edge cases where the hit is missed and the ray
            // travels through the mesh to hit the opposite face from the
            // inside. That backfacing hit yields a Lambertian f == 0
            // (non-doubleSided geometry), painting dark artifacts near
            // whatever face the SSS walk tends to exit through.
            constexpr float kSssExitOffset = 1.0e-3f;
            rayOrigin = sssOut.exitPos + sssOut.exitGeomNormal * kSssExitOffset;
            rayDir = -sssOut.exitDir;
            currentRayDiff.hasDifferentials = false;
            lastBsdfPdf = 0.0f;
            lastScatterWasMedium = false;
            anyNonSpecularBounces = true;
            isFirstBounce = false;
            syntheticLambertianInstanceId = rayHit.hit.instID[0];
            syntheticLambertianGeomId = rayHit.hit.geomID;
            useSyntheticLambertian = true;

            // Clear any medium state. The new Phase 1 design has NO
            // subsurface-in-medium state; SSS is fully handled inside
            // HdEmbreeRandomWalkSSS.
            currentMedium = HdEmbreeMediumState();

            // Count the entire SSS closure (entry + random walk + exit
            // Lambertian) as a single bounce, matching how a plain diffuse
            // surface hit costs 1 bounce. Without this, the synthetic
            // Lambertian exit iteration would consume an extra bounce,
            // effectively halving the user's indirect-light budget for
            // paths that traverse SSS.
            --bounce;
            continue;
        }

        // --- Emissive ---
        if (hasClosure) {
            if (hero.active) {
                const float spectralEmissive = mxcpp::Spectral::RgbToSpectralValue(
                    closure.emissiveColor, hero.wavelengthNm);
                radiance += _SpectralValueToRgb(
                    spectralThroughput * spectralEmissive,
                    hero);
            } else {
                radiance += GfCompMult(throughput, _ToGf(closure.emissiveColor));
            }
        }

        // --- Direct lighting (NEE) with MIS ---
        GfVec3f direct(0.0f);
        if (hasClosure) {
            direct = _ComputeDirectLightingMIS(
                hitPos,
                normal,
                wo,
                sampler,
                doubleSided,
                &closure,
                currentMedium,
                hero.active,
                hero.wavelengthNm,
                hero.pdf);
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
                hitPos,
                normal,
                wo,
                sampler,
                doubleSided,
                &fallback,
                currentMedium,
                hero.active,
                hero.wavelengthNm,
                hero.pdf);
        }
        if (hero.active) {
            radiance += direct * spectralThroughput;
        } else {
            radiance += GfCompMult(throughput, direct);
        }

        // --- Stop after last allowed bounce ---
        if (bounce >= _maxBounces) break;

        // --- BSDF sampling for next direction ---
        if (!hasClosure || !hasBsdfSample || bs.isSubsurface) break;

        if (hero.active) {
            float bsdfContrib = 0.0f;
            if (bs.isSpecular) {
                bsdfContrib = mxcpp::Spectral::RgbToSpectralValue(
                    bs.f, hero.wavelengthNm);
            } else {
                const float cosTheta =
                    std::abs(GfDot(normal, _ToGf(bs.wi)));
                bsdfContrib = mxcpp::Spectral::RgbToSpectralValue(
                    bs.f, hero.wavelengthNm) * cosTheta / bs.pdf;
            }

            if (!std::isfinite(bsdfContrib) || bsdfContrib < 0.0f) {
                bsdfContrib = 0.0f;
            }
            spectralThroughput *= bsdfContrib;
        } else {
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
        }

        lastBsdfPdf = bs.isSpecular ? 0.0f : bs.pdf;
        lastScatterWasMedium = false;
        if (!bs.isSpecular) {
            anyNonSpecularBounces = true;
        }
        isFirstBounce = false;

        const GfVec3f wi = _ToGf(bs.wi);
        const float woDotNg = GfDot(wo, geometricNormal);
        const float wiDotNg = GfDot(wi, geometricNormal);
        const bool crossesBoundary =
            (woDotNg > 0.0f && wiDotNg < 0.0f) ||
            (woDotNg < 0.0f && wiDotNg > 0.0f);
        if (crossesBoundary) {
            // Initial implementation keeps only one medium active at a time;
            // nested dielectric stacks are deferred to a later task.
            if (currentMedium.active && currentMedium.ownerMesh == mesh &&
                wiDotNg > 0.0f) {
                currentMedium = HdEmbreeMediumState();
            } else if (!currentMedium.active &&
                       hasClosure &&
                       closure.hasInteriorMedium &&
                       mesh &&
                       wiDotNg < 0.0f) {
                currentMedium.active = true;
                currentMedium.medium = closure.interiorMedium;
                currentMedium.ownerMesh = mesh;
            }
        }

        // --- Russian Roulette ---
        if (bounce >= _minBouncesBeforeRR) {
            float q = hero.active
                ? std::max({
                    _SpectralScalarToRgb(spectralThroughput, hero)[0],
                    _SpectralScalarToRgb(spectralThroughput, hero)[1],
                    _SpectralScalarToRgb(spectralThroughput, hero)[2]})
                : std::max({throughput[0], throughput[1], throughput[2]});
            q = std::min(q, 0.95f);
            if (q <= 0.0f || sampler.Next() > q) break;
            if (hero.active) {
                spectralThroughput /= q;
            } else {
                throughput /= q;
            }
        }

        // --- Propagate ray differentials ---
        if (currentRayDiff.hasDifferentials && bs.isSpecular) {
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
        float bias = (GfDot(wi, geometricNormal) > 0.0f) ? 1e-4f : -1e-4f;
        rayOrigin = hitPos + geometricNormal * bias;
        rayDir = wi;
    }

    return radiance;
}

PXR_NAMESPACE_CLOSE_SCOPE
