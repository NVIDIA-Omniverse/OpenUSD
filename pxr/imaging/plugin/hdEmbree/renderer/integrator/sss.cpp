//
// hdEmbree random-walk SSS helpers.
//
#include "pxr/imaging/plugin/hdEmbree/renderer/integrator/sss.h"

#include "pxr/imaging/plugin/hdEmbree/renderer/integrator/medium.h"
#include "pxr/imaging/plugin/hdEmbree/renderer/renderer.h"
#include "../rendererImpl.h"
#include "pxr/imaging/plugin/hdEmbree/renderer/sampling/sampling.h"

#include "pxr/base/gf/math.h"

#include <algorithm>
#include <cmath>
#include <limits>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

constexpr float _kBias = 1.0e-4f;

}  // namespace

// =============================================================================
// HdEmbreeRandomWalkSSS: self-contained SSS random walk.
// - Chiang 2016 polynomial remap (albedo + radius -> sigma_t, alpha).
// - Channel-MIS (balance heuristic) per bounce.
// - Henyey-Greenstein classic sampling + forward/backward Dwivedi guided
//   sampling.
// - Similarity relation after kSimilarityLevel bounces (isotropic + reduced
//   sigma).
// - Opposite interface detection on the first bounce for backward guiding.
// =============================================================================

namespace {

constexpr int _kSssMaxBounces = 256;
constexpr float _kVolumeThroughputEps = 1.0e-6f;
constexpr float _kSigmaTEps = 1.0e-6f;

struct _SssWalkState {
    // Walk state.
    GfVec3f rayOrigin;
    GfVec3f rayDir;
    GfVec3f throughput;           // walk-internal throughput; starts at min-alpha
                                  // correction factor (Phase 2).

    // Medium coefficients (Phase 2: Chiang polynomial remap).
    GfVec3f sigma_t;
    GfVec3f sigma_s;
    GfVec3f alpha;                // single-scatter albedo (Phase 2)
    float anisotropy;

    // Opposite-interface state used by backward Dwivedi guiding.
    bool have_opposite_interface = false;
    float opposite_distance = 0.0f;
    GfVec3f entryPos;
    GfVec3f entryNormal;
    unsigned int ownerInstanceId = 0;
    unsigned int ownerGeomId = 0;

    // Phase 3: Dwivedi + similarity state (computed once from Chiang
    // coefficients by _InitDwivediAndSimilarity).
    float diffusion_length = 0.0f;
    float phase_log = 0.0f;
    float guided_fraction = 0.0f;

    // Similarity-reduced coefficients (Wrenninge-Villemin-Hery). Used after
    // bounce > kSimilarityLevel, where scattering is treated as isotropic
    // with reduced scattering.
    GfVec3f sigma_t_star = GfVec3f(0.0f);
    GfVec3f sigma_s_star = GfVec3f(0.0f);
};

struct _SssTraceResult {
    bool foundSurface = false;
    bool hitOwner = false;
    float t = 0.0f;
    GfVec3f hitPos = GfVec3f(0.0f);
    GfVec3f hitNormal = GfVec3f(0.0f);
    GfVec3f objectHitNormal = GfVec3f(0.0f);
    unsigned int instanceId = RTC_INVALID_GEOMETRY_ID;
    unsigned int geomId = RTC_INVALID_GEOMETRY_ID;
    unsigned int primId = RTC_INVALID_GEOMETRY_ID;
    float u = 0.0f;
    float v = 0.0f;
};

// Phase 2: Chiang polynomial remap + min-alpha throughput correction.
//
// HdEmbreeChiangRemap produces (sigma_t, alpha) such that a random walk with
// these coefficients reproduces the target diffuse reflectance, and clamps
// alpha to kMinAlpha (0.2) for numerical stability. When alpha was clamped
// up from a smaller raw value, we compensate by starting the walk throughput
// at (raw / clamped) so the expected reflectance matches the un-clamped case.
// (Cycles `subsurface_random_walk_init`.)
static void
_InitChiangCoefficients(
    GfVec3f const& albedo,
    GfVec3f const& radius,
    float anisotropy,
    GfVec3f* sigma_t,
    GfVec3f* sigma_s,
    GfVec3f* alpha,
    GfVec3f* throughputCorrection)
{
    GfVec3f rawAlpha;
    HdEmbreeChiangRemap(albedo, radius, anisotropy, sigma_t, alpha, &rawAlpha);
    for (int i = 0; i < 3; ++i) {
        (*sigma_s)[i] = (*sigma_t)[i] * (*alpha)[i];
    }

    // Min-alpha correction: if the Chiang polynomial yielded alpha < kMinAlpha
    // we clamped alpha up; offset throughput by raw/clamped so the expected
    // reflectance is unchanged.  Must mirror the clamp value in
    // HdEmbreeChiangRemap.
    constexpr float kMinAlpha = 0.2f;
    *throughputCorrection = GfVec3f(1.0f);
    for (int i = 0; i < 3; ++i) {
        if (rawAlpha[i] < kMinAlpha) {
            (*throughputCorrection)[i] = rawAlpha[i] / kMinAlpha;
        }
    }
}

static float
_CleanCoefficient(float value)
{
    return (std::isfinite(value) && value > 0.0f) ? value : 0.0f;
}

static void
_InitPrecomputedCoefficients(
    GfVec3f const& sigmaA,
    GfVec3f const& sigmaS,
    GfVec3f* sigma_t,
    GfVec3f* sigma_s,
    GfVec3f* alpha,
    GfVec3f* throughputCorrection)
{
    for (int i = 0; i < 3; ++i) {
        const float cleanSigmaA = _CleanCoefficient(sigmaA[i]);
        const float cleanSigmaS = _CleanCoefficient(sigmaS[i]);
        (*sigma_s)[i] = cleanSigmaS;
        (*sigma_t)[i] = cleanSigmaA + cleanSigmaS;
        (*alpha)[i] = (*sigma_t)[i] > _kSigmaTEps
            ? std::clamp(cleanSigmaS / (*sigma_t)[i], 0.0f, 0.999999f)
            : 0.0f;
    }
    *throughputCorrection = GfVec3f(1.0f);
}

// Phase 3: Dwivedi diffusion length + similarity-reduced coefficients.
//
// Must be called after _InitChiangCoefficients has populated
// sigma_t / sigma_s / alpha / anisotropy on the walk state.
//
// Returns false if the diffusion length is too close to 1 (which would make
// phase_log explode and collapse throughput).
static bool
_InitDwivediAndSimilarity(_SssWalkState* st)
{
    // Forward Dwivedi: diffusion_length based on max alpha (safety: use the
    // strongest-scattering channel to avoid too-long guided stretching).
    const float maxAlpha = std::max({st->alpha[0], st->alpha[1], st->alpha[2]});
    st->diffusion_length = HdEmbreeDiffusionLengthDwivedi(maxAlpha);

    // Degenerate guard: L == 1 causes phase_log = inf and throughput collapse.
    if (st->diffusion_length <= 1.0f + 1.0e-6f) {
        return false;
    }
    st->phase_log = std::log(
        (st->diffusion_length + 1.0f) / (st->diffusion_length - 1.0f));

    // guided_fraction: favor classic for strong HG anisotropy (the HG sample
    // already focuses the forward distribution); favor guided for isotropic /
    // low-anisotropy media (where Dwivedi helps exit the walk faster).
    //   guided_fraction = 1 - max(0.5, |g|^0.125)
    st->guided_fraction =
        1.0f - std::max(0.5f, std::pow(std::fabs(st->anisotropy), 0.125f));

    // Similarity-reduced coefficients (Wrenninge-Villemin-Hery).
    //   sigma_s* = sigma_s * (1 - g)
    //   sigma_t* = sigma_a + sigma_s* = sigma_t - sigma_s + sigma_s*
    for (int i = 0; i < 3; ++i) {
        st->sigma_s_star[i] = st->sigma_s[i] * (1.0f - st->anisotropy);
        st->sigma_t_star[i] =
            st->sigma_t[i] - st->sigma_s[i] + st->sigma_s_star[i];
    }
    return true;
}

// Build an orthonormal basis (xAxis, yAxis) for the plane perpendicular to
// zAxis. zAxis is assumed to be normalized. Mirrors the internal
// `mxcpp::_CoordinateSystem` pattern in medium.cpp.
static void
_BuildOrthonormalBasis(
    GfVec3f const& zAxis,
    GfVec3f* xAxis,
    GfVec3f* yAxis)
{
    if (std::fabs(zAxis[2]) < 0.999f) {
        *xAxis = GfVec3f(-zAxis[1], zAxis[0], 0.0f);
    } else {
        *xAxis = GfCross(GfVec3f(0.0f, 1.0f, 0.0f), zAxis);
    }
    if (xAxis->GetLengthSq() <= 1.0e-12f) {
        *xAxis = GfVec3f(1.0f, 0.0f, 0.0f);
    } else {
        xAxis->Normalize();
    }
    *yAxis = GfCross(zAxis, *xAxis);
    if (yAxis->GetLengthSq() <= 1.0e-12f) {
        *yAxis = GfVec3f(0.0f, 1.0f, 0.0f);
    } else {
        yAxis->Normalize();
    }
}

static GfVec3f
_EvalTransmittance(GfVec3f const& sigma_t, float dist)
{
    return GfVec3f(
        std::exp(-sigma_t[0] * dist),
        std::exp(-sigma_t[1] * dist),
        std::exp(-sigma_t[2] * dist));
}

static float
_MinPositiveComponent(GfVec3f const& v)
{
    float result = std::numeric_limits<float>::max();
    for (int i = 0; i < 3; ++i) {
        if (v[i] > _kSigmaTEps) {
            result = std::min(result, v[i]);
        }
    }
    return result;
}

static bool
_HitOwnerGeometry(RTCRayHit const& rayHit, _SssWalkState const& st)
{
    return rayHit.hit.instID[0] == st.ownerInstanceId &&
           rayHit.hit.geomID == st.ownerGeomId;
}

static _SssTraceResult
_TraceSssBoundary(
    _SssWalkState const& st,
    HdEmbreeSssInput const& in,
    RTCScene scene,
    float rayTfar)
{
    _SssTraceResult result;

    const bool useOwnerScene = (in.ownerScene != nullptr);
    const RTCScene traceScene = useOwnerScene ? in.ownerScene : scene;

    GfVec3f rayOrigin = st.rayOrigin;
    GfVec3f rayDir = st.rayDir;
    if (useOwnerScene) {
        rayOrigin = in.worldToObjectMatrix.Transform(st.rayOrigin);
        rayDir = in.worldToObjectMatrix.TransformDir(st.rayDir);
    }
    if (rayDir.GetLengthSq() <= 1.0e-20f) {
        return result;
    }

    RTCRayHit rayHit;
    rayHit.ray.flags = 0;
    rayHit.ray.org_x = rayOrigin[0];
    rayHit.ray.org_y = rayOrigin[1];
    rayHit.ray.org_z = rayOrigin[2];
    rayHit.ray.tnear = _kBias;
    rayHit.ray.dir_x = rayDir[0];
    rayHit.ray.dir_y = rayDir[1];
    rayHit.ray.dir_z = rayDir[2];
    rayHit.ray.time = 0.0f;
    rayHit.ray.tfar = rayTfar;
    rayHit.ray.mask = static_cast<uint32_t>(HdEmbree_RayMask::Camera);
    rayHit.ray.id = useOwnerScene
        ? HdEmbreeFaceCullBypassRayId
        : 0;
    rayHit.hit.primID = RTC_INVALID_GEOMETRY_ID;
    rayHit.hit.geomID = RTC_INVALID_GEOMETRY_ID;
    rayHit.hit.instID[0] = RTC_INVALID_GEOMETRY_ID;

    rtcIntersect1(traceScene, &rayHit);

    result.foundSurface = (rayHit.hit.geomID != RTC_INVALID_GEOMETRY_ID);
    if (!result.foundSurface) {
        return result;
    }

    result.hitOwner = useOwnerScene
        ? (rayHit.hit.geomID == st.ownerGeomId)
        : _HitOwnerGeometry(rayHit, st);
    result.t = rayHit.ray.tfar;
    result.hitPos = st.rayOrigin + st.rayDir * result.t;
    result.instanceId = useOwnerScene ? st.ownerInstanceId : rayHit.hit.instID[0];
    result.geomId = rayHit.hit.geomID;
    result.primId = rayHit.hit.primID;
    result.u = rayHit.hit.u;
    result.v = rayHit.hit.v;

    float orientationSign = 1.0f;
    if (useOwnerScene) {
        RTCGeometry const hitGeometry =
            rtcGetGeometry(traceScene, rayHit.hit.geomID);
        auto const* prototypeContext = hitGeometry
            ? static_cast<HdEmbreePrototypeContext const*>(
                rtcGetGeometryUserData(hitGeometry))
            : nullptr;
        if (prototypeContext) {
            orientationSign = prototypeContext->orientationSign;
        }
    }
    const GfVec3f embreeObjectHitNormal(
        rayHit.hit.Ng_x, rayHit.hit.Ng_y, rayHit.hit.Ng_z);
    result.objectHitNormal = embreeObjectHitNormal;
    GfVec3f hitNormal = orientationSign * embreeObjectHitNormal;
    if (useOwnerScene) {
        hitNormal = _TransformNormalToWorld(
            in.worldToObjectMatrix, hitNormal);
    }
    if (hitNormal.GetLengthSq() > 1.0e-20f) {
        hitNormal.Normalize();
    } else {
        hitNormal = -st.rayDir;
    }
    result.hitNormal = hitNormal;

    return result;
}

// Chiang 2016 remap (Cycles subsurface_random_walk_remap port, per-channel).
//
// Converts single-channel (albedo, d=radius, g=anisotropy) into (sigma_t, alpha)
// such that a random walk with these coefficients produces the target diffuse
// reflectance.
//
// Source: Blender Cycles src/kernel/integrator/subsurface_random_walk.h
// (Apache 2.0). Polynomial coefficients from Chiang et al. 2016.
static void
_ChiangRemapChannel(float albedo, float d, float g,
                    float* out_sigma_t, float* out_rawAlpha)
{
    const float g2 = g * g;
    const float g3 = g2 * g;
    const float g4 = g3 * g;
    const float g5 = g4 * g;
    const float g6 = g5 * g;
    const float g7 = g6 * g;

    const float A = 1.8260523782f + -1.28451056436f * g + -1.79904629312f * g2 +
                    9.19393289202f * g3 + -22.8215585862f * g4 +
                    32.0234874259f * g5 + -23.6264803333f * g6 +
                    7.21067002658f * g7;
    const float B = 4.98511194385f +
                    0.127355959438f *
                        std::exp(31.1491581433f * g + -201.847017512f * g2 +
                                  841.576016723f * g3 + -2018.09288505f * g4 +
                                  2731.71560286f * g5 + -1935.41424244f * g6 +
                                  559.009054474f * g7);
    const float C = 1.09686102424f + -0.394704063468f * g + 1.05258115941f * g2 +
                    -8.83963712726f * g3 + 28.8643230661f * g4 +
                    -46.8802913581f * g5 + 38.5402837518f * g6 +
                    -12.7181042538f * g7;
    const float D = 0.496310210422f + 0.360146581622f * g +
                    -2.15139309747f * g2 + 17.8896899217f * g3 +
                    -55.2984010333f * g4 + 82.065982243f * g5 +
                    -58.5106008578f * g6 + 15.8478295021f * g7;
    const float E = 4.23190299701f +
                    0.00310603949088f *
                        std::exp(76.7316253952f * g + -594.356773233f * g2 +
                                  2448.8834203f * g3 + -5576.68528998f * g4 +
                                  7116.60171912f * g5 + -4763.54467887f * g6 +
                                  1303.5318055f * g7);
    const float F = 2.40602999408f + -2.51814844609f * g + 9.18494908356f * g2 +
                    -79.2191708682f * g3 + 259.082868209f * g4 +
                    -403.613804597f * g5 + 302.85712436f * g6 +
                    -87.4370473567f * g7;

    const float blend = std::pow(albedo, 0.25f);

    float alpha = (1.0f - blend) * A * std::pow(std::atan(B * albedo), C) +
                  blend * D * std::pow(std::atan(E * albedo), F);
    alpha = std::clamp(alpha, 0.0f, 0.999999f);

    const float sigma_t_prime = 1.0f / std::max(d, 1.0e-16f);
    const float sigma_t = sigma_t_prime / std::max(1.0f - g, 1.0e-6f);

    *out_sigma_t = sigma_t;
    *out_rawAlpha = alpha;
}

}  // namespace

void
HdEmbreeChiangRemap(
    const GfVec3f& albedo,
    const GfVec3f& radius,
    float anisotropy,
    GfVec3f* sigma_t,
    GfVec3f* alpha,
    GfVec3f* rawAlphaOut)
{
    const float g = std::clamp(anisotropy, -0.99f, 0.99f);
    GfVec3f rawAlpha;
    for (int i = 0; i < 3; ++i) {
        _ChiangRemapChannel(
            std::clamp(albedo[i], 0.0f, 1.0f),
            std::max(radius[i], 1.0e-16f),
            g,
            &(*sigma_t)[i],
            &rawAlpha[i]);
    }

    // Min-alpha clamp for numerical stability (Cycles pattern).
    // Callers use rawAlphaOut to compute min-alpha throughput correction.
    constexpr float kMinAlpha = 0.2f;
    for (int i = 0; i < 3; ++i) {
        (*alpha)[i] = std::max(rawAlpha[i], kMinAlpha);
    }
    if (rawAlphaOut) {
        *rawAlphaOut = rawAlpha;
    }
}

float
HdEmbreeDiffusionLengthDwivedi(float alpha)
{
    // Eq. 67 from d'Eon-Krivanek 2020 (via Cycles
    // subsurface_random_walk.h::diffusion_length_dwivedi).
    //
    // Source: Blender Cycles (Apache 2.0).
    alpha = std::clamp(alpha, 1.0e-6f, 0.999999f);
    const float denom =
        1.0f - std::pow(alpha, 2.44294f - 0.0215813f * alpha + 0.578637f / alpha);
    return 1.0f / std::sqrt(std::max(denom, 1.0e-12f));
}

float
HdEmbreeEvalPhaseDwivedi(float L, float phase_log, float cos_theta)
{
    // Eq. 9 from Meng-Hanika-Dachsbacher 2016, using precomputed phase_log.
    // Source: Blender Cycles (Apache 2.0).
    return 1.0f / (std::max(L - cos_theta, 1.0e-6f) * phase_log);
}

float
HdEmbreeSamplePhaseDwivedi(float L, float phase_log, float u)
{
    // Eq. 10 from Meng-Hanika-Dachsbacher 2016.
    // Returns cos_theta. Uses inverse CDF:
    //   cos_theta = L - (L+1) * pow((L-1)/(L+1), u)
    // Implemented with precomputed phase_log = log((L+1)/(L-1)).
    // Source: Blender Cycles (Apache 2.0).
    const float uClamped = std::clamp(u, 0.0f, 1.0f);
    return L - (L + 1.0f) * std::exp(-uClamped * phase_log);
}

float
HdEmbreeBackwardDwivediFraction(
    float oppositeDistance,
    float x,
    float diffusionLength)
{
    if (oppositeDistance <= 0.0f || diffusionLength <= 0.0f) {
        return 0.0f;
    }

    const float clampedX = std::clamp(x, 0.0f, oppositeDistance);
    const float exponent =
        (oppositeDistance - 2.0f * clampedX) / diffusionLength;
    return 1.0f / (1.0f + std::exp(exponent));
}

HdEmbreeSssOutput
HdEmbreeRandomWalkSSS(
    HdEmbreeSssInput const& in,
    HdEmbreeSampleDomain const& domain,
    RTCScene scene)
{
    // Phase 3: after kSimilarityLevel bounces we switch to isotropic scattering
    // with the similarity-reduced sigma values (Wrenninge-Villemin-Hery).
    // The guided_fraction is fixed at 0.75 in that regime since there is no
    // HG forward peak to compete with.
    constexpr int kSimilarityLevel = 9;
    constexpr float kReducedGuidedFraction = 0.75f;
    constexpr float kTwoPi = 2.0f * 3.14159265358979323846f;
    constexpr float kInvTwoPi = 1.0f / (2.0f * 3.14159265358979323846f);

    HdEmbreeSssOutput out;  // default-initialized (success=false)

    _SssWalkState st;
    GfVec3f throughputCorrection;
    if (in.usePrecomputedCoefficients) {
        _InitPrecomputedCoefficients(
            in.precomputedSigmaA,
            in.precomputedSigmaS,
            &st.sigma_t,
            &st.sigma_s,
            &st.alpha,
            &throughputCorrection);
    } else {
        _InitChiangCoefficients(
            in.albedo,
            in.radius,
            in.anisotropy,
            &st.sigma_t,
            &st.sigma_s,
            &st.alpha,
            &throughputCorrection);
    }
    st.throughput = throughputCorrection;
    st.rayOrigin = in.entryPos;
    st.rayDir = in.entryDir;
    st.anisotropy = std::clamp(in.anisotropy, -0.99f, 0.99f);
    st.entryPos = in.entryPos;
    st.entryNormal = in.entryGeomNormal;
    st.ownerInstanceId = in.ownerInstanceId;
    st.ownerGeomId = in.ownerGeomId;
    // have_opposite_interface / opposite_distance start empty and may be
    // populated by the extended first-bounce ray below.

    if (!_InitDwivediAndSimilarity(&st)) {
        return out;  // degenerate diffusion length
    }

    // Match Cycles: Dwivedi guiding is sampled around the stored entry normal.
    // In our renderer the entry geometric normal is face-forwarded toward `wo`,
    // so this axis is the same outward-facing normal Cycles uses for `N`.
    const GfVec3f guideAxis = st.entryNormal;

    for (int bounce = 0; bounce < _kSssMaxBounces; ++bounce) {
        ++out.walkSteps;
        const HdEmbreeSampleDomain bounceDomain =
            domain.Chain(HdEmbreeSampleDomainKey::SssBounce, bounce);

        // Similarity switch: after kSimilarityLevel the medium is approximated
        // as isotropic with reduced scattering.
        GfVec3f sigma_t_eff;
        GfVec3f sigma_s_eff;
        float anisotropy_eff;
        float guided_fraction_eff;
        if (bounce <= kSimilarityLevel) {
            sigma_t_eff = st.sigma_t;
            sigma_s_eff = st.sigma_s;
            anisotropy_eff = st.anisotropy;
            guided_fraction_eff = st.guided_fraction;
        } else {
            sigma_t_eff = st.sigma_t_star;
            sigma_s_eff = st.sigma_s_star;
            anisotropy_eff = 0.0f;  // isotropic regime
            guided_fraction_eff = kReducedGuidedFraction;
        }

        // Chiang channel selection MIS (balance heuristic) per bounce.
        mxcpp::Vec3f channelPdfMx;
        const int channel = mxcpp::ChannelMIS(
            /*throughput*/ mxcpp::Vec3f(
                st.throughput[0], st.throughput[1], st.throughput[2]),
            /*weights*/ mxcpp::Vec3f(
                st.alpha[0], st.alpha[1], st.alpha[2]),
            bounceDomain.Fork(HdEmbreeSampleDomainKey::SssChannel).Draw1D(),
            &channelPdfMx);
        const GfVec3f channelPdf(
            channelPdfMx[0], channelPdfMx[1], channelPdfMx[2]);

        float sampleSigmaT = sigma_t_eff[channel];
        if (sampleSigmaT <= _kSigmaTEps) {
            return out;  // degenerate medium
        }

        // Direction sampling.
        // bounce 0 uses the pre-set entry direction (no guiding possible yet).
        // bounce > 0 picks guided (Dwivedi around guideAxis) vs classic (HG)
        // stochastically with probability `guided_fraction_eff`.
        float forward_stretching = 1.0f;
        float forward_pdf_factor = 0.0f;

        float backward_stretching = 1.0f;
        float backward_pdf_factor = 0.0f;
        float backwardFraction = 0.0f;

        bool guidedThisBounce = false;
        bool guideBackward = false;
        if (bounce > 0) {
            guidedThisBounce =
                (bounceDomain.Fork(HdEmbreeSampleDomainKey::SssGuideChoice)
                     .Draw1D() < guided_fraction_eff);

            if (st.have_opposite_interface) {
                const float x = GfDot(
                    st.rayOrigin - st.entryPos,
                    -st.entryNormal);
                backwardFraction = HdEmbreeBackwardDwivediFraction(
                    st.opposite_distance,
                    x,
                    st.diffusion_length);
                if (guidedThisBounce) {
                    guideBackward =
                        (bounceDomain
                             .Fork(HdEmbreeSampleDomainKey::SssBackwardChoice)
                             .Draw1D() < backwardFraction);
                }
            }

            const GfVec2f phaseSample =
                bounceDomain
                    .Fork(HdEmbreeSampleDomainKey::SssPhaseDirection)
                    .Draw2D();
            const float rand_a = phaseSample[0];
            const float rand_b = phaseSample[1];

            GfVec3f newDir;
            float cosTheta_ent = 0.0f;
            float hg_pdf = 1.0f;

            if (guidedThisBounce) {
                // Forward Dwivedi: sample cos_theta around the inward-facing
                // entry direction (guideAxis).
                float cos_theta = HdEmbreeSamplePhaseDwivedi(
                    st.diffusion_length, st.phase_log, rand_a);
                // Backward Dwivedi mirrors the guide distribution along the
                // entry normal, biasing directions toward the opposite side.
                if (guideBackward) {
                    cos_theta = -cos_theta;
                }

                cosTheta_ent = cos_theta;

                GfVec3f X, Y;
                _BuildOrthonormalBasis(guideAxis, &X, &Y);
                const float sin_theta = std::sqrt(
                    std::max(0.0f, 1.0f - cos_theta * cos_theta));
                const float phi = kTwoPi * rand_b;
                newDir = X * (sin_theta * std::cos(phi)) +
                         Y * (sin_theta * std::sin(phi)) +
                         guideAxis * cos_theta;
                if (newDir.GetLengthSq() > 1.0e-20f) {
                    newDir.Normalize();
                } else {
                    newDir = guideAxis;
                }

                hg_pdf = mxcpp::PdfHenyeyGreenstein(
                    mxcpp::Vec3f(newDir[0], newDir[1], newDir[2]),
                    mxcpp::Vec3f(-st.rayDir[0], -st.rayDir[1], -st.rayDir[2]),
                    anisotropy_eff);
            } else {
                // Classic HG sampling around the incoming ray direction.
                const mxcpp::Vec3f wo(
                    -st.rayDir[0], -st.rayDir[1], -st.rayDir[2]);
                const mxcpp::Vec3f sampled =
                    mxcpp::SampleHenyeyGreenstein(
                        wo, anisotropy_eff, rand_a, rand_b);
                newDir = GfVec3f(sampled[0], sampled[1], sampled[2]);
                cosTheta_ent = GfDot(newDir, guideAxis);
                hg_pdf = mxcpp::PdfHenyeyGreenstein(
                    mxcpp::Vec3f(newDir[0], newDir[1], newDir[2]),
                    mxcpp::Vec3f(-st.rayDir[0], -st.rayDir[1], -st.rayDir[2]),
                    anisotropy_eff);
            }

            st.rayDir = newDir;

            // Guided PDF factor used later to mix into the classic PDF.
            // forward_pdf_factor converts the classic (sigma_s * T) PDF into
            // the guided-direction PDF by multiplying by
            //   (1/(2*pi)) * p_Dwivedi(cosTheta_ent) / p_HG(cosTheta_ray)
            // (the 1/(2*pi) captures the azimuth, which Dwivedi samples
            //  uniformly while HG already includes it in its normalization).
            const float hg_pdf_safe = std::max(hg_pdf, 1.0e-8f);
            forward_pdf_factor = kInvTwoPi *
                HdEmbreeEvalPhaseDwivedi(
                    st.diffusion_length, st.phase_log, cosTheta_ent) /
                hg_pdf_safe;
            backward_pdf_factor = kInvTwoPi *
                HdEmbreeEvalPhaseDwivedi(
                    st.diffusion_length, st.phase_log, -cosTheta_ent) /
                hg_pdf_safe;

            forward_stretching = 1.0f - cosTheta_ent / st.diffusion_length;
            backward_stretching = 1.0f + cosTheta_ent / st.diffusion_length;

            if (guidedThisBounce) {
                sampleSigmaT *= guideBackward
                    ? backward_stretching
                    : forward_stretching;
            }
        }

        // Free-flight distance sample for the (possibly stretched) sigma_t.
        const float u = std::clamp(
            bounceDomain.Fork(HdEmbreeSampleDomainKey::SssFreeFlight).Draw1D(),
            1.0e-6f,
            1.0f - 1.0e-6f);
        float t = -std::log(1.0f - u) /
                  std::max(sampleSigmaT, _kSigmaTEps);
        const float sampledT = t;

        // Ray cast (use Camera mask to match the path tracer's visibility set).
        float rayTfar = sampledT;
        if (bounce == 0) {
            const float minSigmaT = _MinPositiveComponent(sigma_t_eff);
            if (minSigmaT < std::numeric_limits<float>::max()) {
                rayTfar = std::max(sampledT, 10.0f / minSigmaT);
            }
        }

        const _SssTraceResult trace =
            _TraceSssBoundary(st, in, scene, rayTfar);
        ++out.intersectionTests;

        bool hit = trace.foundSurface && trace.hitOwner;
        if (bounce == 0) {
            if (trace.foundSurface && trace.hitOwner) {
                const float oppositeDistance =
                    GfDot(trace.hitPos - st.entryPos, -st.entryNormal);
                if (oppositeDistance > _kBias) {
                    st.have_opposite_interface = true;
                    st.opposite_distance = oppositeDistance;
                }
            }

            // The extended first ray is only for detecting the opposite
            // interface. The actual walk still scatters if the sampled free
            // flight distance is shorter than the first surface hit.
            hit = trace.foundSurface && trace.hitOwner && trace.t < sampledT;
        }

        if (hit) {
            t = trace.t;
        }

        // Classic sampling PDF + contribution.
        //   hit:     sampleContrib = T(t),                classicPdf = T(t)
        //   scatter: sampleContrib = sigma_s * T(t),      classicPdf = sigma_t * T(t)
        const GfVec3f transmittance = _EvalTransmittance(sigma_t_eff, t);
        GfVec3f classicPdf;
        GfVec3f sampleContrib;
        if (hit) {
            classicPdf = transmittance;
            sampleContrib = transmittance;
        } else {
            // Distance-sampling PDF uses sigma_t (exponential sampling pdf).
            classicPdf = GfCompMult(sigma_t_eff, transmittance);
            // Throughput numerator uses sigma_s (scatter albedo weighting).
            sampleContrib = GfCompMult(sigma_s_eff, transmittance);
        }

        // MIS blend between classic and guided (Dwivedi) sampling PDFs.
        GfVec3f pdf = classicPdf;
        if (bounce > 0) {
            // Guided PDF uses the stretched sigma_t (distance was sampled
            // with the stretched extinction when guided, and the PDF has to
            // match that distribution for correct MIS weighting).
            const GfVec3f stretched_sigma_t = sigma_t_eff * forward_stretching;
            const GfVec3f stretched_trans =
                _EvalTransmittance(stretched_sigma_t, t);
            GfVec3f forward_guided_pdf;
            if (hit) {
                forward_guided_pdf = stretched_trans;
            } else {
                // Match Cycles' subsurface_random_walk_pdf: pdf = sigma_t * T for scatter.
                forward_guided_pdf = GfCompMult(stretched_sigma_t, stretched_trans);
            }
            forward_guided_pdf = forward_guided_pdf * forward_pdf_factor;

            GfVec3f guidedPdf = forward_guided_pdf;
            if (st.have_opposite_interface) {
                const GfVec3f backward_stretched_sigma_t =
                    sigma_t_eff * backward_stretching;
                const GfVec3f backward_stretched_trans =
                    _EvalTransmittance(backward_stretched_sigma_t, t);
                GfVec3f backward_guided_pdf;
                if (hit) {
                    backward_guided_pdf = backward_stretched_trans;
                } else {
                    backward_guided_pdf = GfCompMult(
                        backward_stretched_sigma_t,
                        backward_stretched_trans);
                }
                backward_guided_pdf =
                    backward_guided_pdf * backward_pdf_factor;

                guidedPdf =
                    forward_guided_pdf * (1.0f - backwardFraction) +
                    backward_guided_pdf * backwardFraction;
            }

            pdf = classicPdf * (1.0f - guided_fraction_eff) +
                  guidedPdf * guided_fraction_eff;
        }

        const float denom = GfDot(channelPdf, pdf);
        if (!std::isfinite(denom) || denom <= 1.0e-20f) {
            return out;
        }
        st.throughput = GfCompMult(st.throughput,
                                   sampleContrib * (1.0f / denom));

        // Termination on degenerate / near-zero throughput.
        const float maxTp = std::max({st.throughput[0],
                                      st.throughput[1],
                                      st.throughput[2]});
        if (!std::isfinite(maxTp) || maxTp < _kVolumeThroughputEps) {
            return out;
        }

        if (hit) {
            // Build exit info from the surface intersection.
            const GfVec3f hitPos = trace.hitPos;
            GfVec3f hitNormal = trace.hitNormal;
            GfVec3f objectHitNormal = trace.objectHitNormal;
            if (hitNormal.GetLengthSq() > 1.0e-20f) {
                hitNormal.Normalize();
            } else {
                hitNormal = -st.rayDir;  // fallback
                objectHitNormal = _TransformNormalToObject(
                    in.objectToWorldMatrix, hitNormal);
            }
            // Orient outward: the exit normal should align with the ray direction
            // (the ray leaves the medium, so the outward face's normal is in the
            // same half-space as rayDir). Flip only if Embree returned an inward Ng.
            if (GfDot(hitNormal, st.rayDir) < 0.0f) {
                hitNormal = -hitNormal;
                objectHitNormal = -objectHitNormal;
            }
            if (objectHitNormal.GetLengthSq() > 1.0e-20f) {
                objectHitNormal.Normalize();
            }

            out.success = true;
            out.exitPos = hitPos;
            out.exitGeomNormal = hitNormal;
            out.exitDir = st.rayDir;
            out.exitObjectGeomNormal = objectHitNormal;
            out.exitInstanceId = trace.instanceId;
            out.exitGeomId = trace.geomId;
            out.exitPrimId = trace.primId;
            out.exitU = trace.u;
            out.exitV = trace.v;
            out.throughputWeight = st.throughput;
            return out;
        }

        // Advance ray origin to the scatter point for the next bounce.
        st.rayOrigin = st.rayOrigin + st.rayDir * t;
    }

    return out;  // max bounces exceeded
}

HdEmbreeRenderer::_SubsurfaceResult
HdEmbreeRenderer::_TraceSubsurface(
    _SubsurfaceInput const& input,
    HdEmbreeSampleDomain const& domain,
    _PathState* state) const
{
    if (!state || !input.rayHit || !input.instanceContext ||
        !input.closure) {
        return _SubsurfaceResult::Terminate;
    }

    GfVec3f entryDirection = input.sampledEntryDirection;
    if (!input.hasSampledEntryDirection) {
        const GfVec2f sample =
            domain.Fork(HdEmbreeSampleDomainKey::SssEntryDirection).Draw2D();
        mxcpp::Vec3f sampledDirection;
        if (!mxcpp::Bsdf::SampleSubsurfaceEntry(
                *input.closure,
                _ToMx(input.normal),
                _ToMx(input.wo),
                sample[0],
                sample[1],
                sampledDirection)) {
            return _SubsurfaceResult::Terminate;
        }
        entryDirection = _ToGf(sampledDirection);
    }

    // A direction that is inward in the shading frame can still point out of
    // the true face when a normal map strongly tilts the frame.
    if (GfDot(input.faceNormal, entryDirection) >= 0.0f) {
        return _SubsurfaceResult::Terminate;
    }

    GfVec3f entryWeight = input.entryWeight;
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(entryWeight[i]) || entryWeight[i] < 0.0f) {
            entryWeight[i] = 0.0f;
        }
    }
    _ApplyPathWeight(entryWeight, state);
    if (_IsNearlyBlack(
            _GetPathThroughputRgb(*state), _minLuminanceCutoff)) {
        return _SubsurfaceResult::Terminate;
    }

    HdEmbreeSssInput walkInput;
    walkInput.entryPos = input.hitPos;
    walkInput.entryGeomNormal = input.normal;
    walkInput.entryDir = entryDirection;
    walkInput.albedo = _ToGf(input.closure->subsurfaceColor);
    walkInput.radius = GfCompMult(
        _ToGf(input.closure->subsurfaceRadius),
        _ToGf(input.closure->subsurfaceRadiusScale));
    walkInput.anisotropy =
        std::clamp(input.closure->subsurfaceAnisotropy, -0.99f, 0.99f);
    if (input.closure->hasPrecomputedSubsurfaceMedium &&
        !input.closure->precomputedSubsurfaceMedium.IsVacuum()) {
        walkInput.usePrecomputedCoefficients = true;
        walkInput.precomputedSigmaA =
            _ToGf(input.closure->precomputedSubsurfaceMedium.sigmaA);
        walkInput.precomputedSigmaS =
            _ToGf(input.closure->precomputedSubsurfaceMedium.sigmaS);
        walkInput.anisotropy = std::clamp(
            input.closure->precomputedSubsurfaceMedium.anisotropy,
            -0.99f,
            0.99f);
    }
    walkInput.ior = std::max(input.closure->specularIor, 1.0f);
    walkInput.ownerInstanceId = input.rayHit->hit.instID[0];
    walkInput.ownerGeomId = input.rayHit->hit.geomID;
    walkInput.ownerScene = input.instanceContext->rootScene;
    walkInput.objectToWorldMatrix =
        input.instanceContext->objectToWorldMatrix;
    walkInput.worldToObjectMatrix =
        input.instanceContext->worldToObjectMatrix;

    HdEmbreeSssOutput output = HdEmbreeRandomWalkSSS(
        walkInput,
        domain.Fork(HdEmbreeSampleDomainKey::SssEntry),
        _scene);
    _sssCallCount.fetch_add(1, std::memory_order_relaxed);
    _sssWalkStepCount.fetch_add(
        output.walkSteps, std::memory_order_relaxed);
    _sssIntersectionCount.fetch_add(
        output.intersectionTests, std::memory_order_relaxed);
    if (output.success) {
        _sssSuccessCount.fetch_add(1, std::memory_order_relaxed);
    } else {
        return _SubsurfaceResult::Terminate;
    }

    GfVec3f walkWeight = output.throughputWeight;
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(walkWeight[i]) || walkWeight[i] < 0.0f) {
            walkWeight[i] = 0.0f;
        }
    }
    _ApplyPathWeight(walkWeight, state);
    if (_IsNearlyBlack(
            _GetPathThroughputRgb(*state), _minLuminanceCutoff)) {
        return _SubsurfaceResult::Terminate;
    }

    state->rayOrigin = output.exitPos;
    state->rayDir = -output.exitDir;
    state->rayDiff.hasDifferentials = false;
    state->lastBsdfPdf = 0.0f;
    state->lastScatterWasMedium = false;
    state->hasDiffuseLikeAncestor = true;
    state->isFirstBounce = false;
    state->syntheticLambertianExit = output;
    state->useSyntheticLambertian = true;
    state->medium = HdEmbreeMediumState();
    return _SubsurfaceResult::ContinueAtExit;
}

PXR_NAMESPACE_CLOSE_SCOPE
