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
// - Chiang 2016 polynomial remap (albedo + radius -> extinction, alpha).
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
constexpr float _kExtinctionEps = 1.0e-6f;

struct _SssWalkState {
    // Walk state.
    GfVec3f positionRayOriginWld;
    GfVec3f directionRayWld;
    GfVec3f throughputRgb; // walk-internal throughput; starts at min-alpha
                           // correction factor (Phase 2).

    // Medium coefficients (Phase 2: Chiang polynomial remap).
    GfVec3f extinction;
    GfVec3f scattering;
    GfVec3f alpha;                // single-scatter albedo (Phase 2)
    float anisotropy;

    // Opposite-interface state used by backward Dwivedi guiding.
    bool haveOppositeInterface = false;
    float distanceOppositeWld = 0.0f;
    GfVec3f positionEntryWld;
    GfVec3f normalShdGuideWldOut;
    unsigned int ownerInstanceId = 0;
    unsigned int ownerGeomId = 0;

    // Phase 3: Dwivedi + similarity state (computed once from Chiang
    // coefficients by _InitDwivediAndSimilarity).
    float diffusionLength = 0.0f;
    float phaseLog = 0.0f;
    float guidedFraction = 0.0f;

    // Similarity-reduced coefficients (Wrenninge-Villemin-Hery). Used after
    // bounce > kSimilarityLevel, where scattering is treated as isotropic
    // with reduced scattering.
    GfVec3f extinctionStar = GfVec3f(0.0f);
    GfVec3f scatteringStar = GfVec3f(0.0f);
};

struct _SssTraceResult {
    bool foundSurface = false;
    bool hitOwner = false;
    float distanceWld = 0.0f;
    GfVec3f positionHitWld = GfVec3f(0.0f);
    GfVec3f normalGeomWldExt = GfVec3f(0.0f);
    GfVec3f normalGeomObjExt = GfVec3f(0.0f);
    unsigned int instanceId = RTC_INVALID_GEOMETRY_ID;
    unsigned int geomId = RTC_INVALID_GEOMETRY_ID;
    unsigned int primId = RTC_INVALID_GEOMETRY_ID;
    float coordinateParametricU = 0.0f;
    float coordinateParametricV = 0.0f;
};

// Phase 2: Chiang polynomial remap + min-alpha throughput correction.
//
// HdEmbreeChiangRemap produces (extinction, alpha) such that a random walk with
// these coefficients reproduces the target diffuse reflectance, and clamps
// alpha to kMinAlpha (0.2) for numerical stability. When alpha was clamped
// up from a smaller raw value, we compensate by starting the walk throughput
// at (raw / clamped) so the expected reflectance matches the un-clamped case.
// (Cycles `subsurface_random_walk_init`.)
static void
_InitChiangCoefficients(GfVec3f const& albedo, GfVec3f const& radius,
                        float anisotropy, GfVec3f* extinction,
                        GfVec3f* scattering, GfVec3f* alpha,
                        GfVec3f* throughputCorrection)
{
    GfVec3f rawAlpha;
    HdEmbreeChiangRemap(albedo, radius, anisotropy, extinction, alpha,
                        &rawAlpha);
    for (int indexChannel = 0; indexChannel < 3; ++indexChannel) {
        (*scattering)[indexChannel] =
            (*extinction)[indexChannel] * (*alpha)[indexChannel];
    }

    // Min-alpha correction: if the Chiang polynomial yielded alpha < kMinAlpha
    // we clamped alpha up; offset throughput by raw/clamped so the expected
    // reflectance is unchanged.  Must mirror the clamp value in
    // HdEmbreeChiangRemap.
    constexpr float kMinAlpha = 0.2f;
    *throughputCorrection = GfVec3f(1.0f);
    for (int indexChannel = 0; indexChannel < 3; ++indexChannel) {
        if (rawAlpha[indexChannel] < kMinAlpha) {
            (*throughputCorrection)[indexChannel] =
                rawAlpha[indexChannel] / kMinAlpha;
        }
    }
}

static float
_CleanCoefficient(float value)
{
    return (std::isfinite(value) && value > 0.0f) ? value : 0.0f;
}

static void
_InitPrecomputedCoefficients(GfVec3f const& absorption,
                             GfVec3f const& scattering, GfVec3f* extinction,
                             GfVec3f* scatteringOutput, GfVec3f* alpha,
                             GfVec3f* throughputCorrection)
{
    for (int indexChannel = 0; indexChannel < 3; ++indexChannel) {
        const float cleanAbsorption =
            _CleanCoefficient(absorption[indexChannel]);
        const float cleanScattering =
            _CleanCoefficient(scattering[indexChannel]);
        (*scatteringOutput)[indexChannel] = cleanScattering;
        (*extinction)[indexChannel] = cleanAbsorption + cleanScattering;
        (*alpha)[indexChannel] =
            (*extinction)[indexChannel] > _kExtinctionEps
                ? std::clamp(cleanScattering / (*extinction)[indexChannel],
                             0.0f, 0.999999f)
                : 0.0f;
    }
    *throughputCorrection = GfVec3f(1.0f);
}

// Phase 3: Dwivedi diffusion length + similarity-reduced coefficients.
//
// Must be called after _InitChiangCoefficients has populated
// extinction / scattering / alpha / anisotropy on the walk state.
//
// Returns false if the diffusion length is too close to 1 (which would make
// phaseLog explode and collapse throughput).
static bool
_InitDwivediAndSimilarity(_SssWalkState* state)
{
    // Forward Dwivedi: diffusionLength based on max alpha (safety: use the
    // strongest-scattering channel to avoid too-long guided stretching).
    const float maxAlpha =
        std::max({state->alpha[0], state->alpha[1], state->alpha[2]});
    state->diffusionLength = HdEmbreeDiffusionLengthDwivedi(maxAlpha);

    // Degenerate guard: diffusionLength == 1 causes phaseLog = inf and
    // throughput collapse.
    if (state->diffusionLength <= 1.0f + 1.0e-6f) {
        return false;
    }
    state->phaseLog = std::log((state->diffusionLength + 1.0f) /
                               (state->diffusionLength - 1.0f));

    // guidedFraction: favor classic for strong HG anisotropy (the HG sample
    // already focuses the forward distribution); favor guided for isotropic /
    // low-anisotropy media (where Dwivedi helps exit the walk faster).
    //   guidedFraction = 1 - max(0.5, |g|^0.125)
    state->guidedFraction =
        1.0f - std::max(0.5f, std::pow(std::fabs(state->anisotropy), 0.125f));

    // Similarity-reduced coefficients (Wrenninge-Villemin-Hery).
    //   sigma_s* = sigma_s * (1 - g)
    //   sigma_t* = sigma_a + sigma_s* = sigma_t - sigma_s + sigma_s*
    for (int indexChannel = 0; indexChannel < 3; ++indexChannel) {
        state->scatteringStar[indexChannel] =
            state->scattering[indexChannel] * (1.0f - state->anisotropy);
        state->extinctionStar[indexChannel] =
            state->extinction[indexChannel] - state->scattering[indexChannel] +
            state->scatteringStar[indexChannel];
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
_EvalTransmittance(GfVec3f const& extinction, float distanceWld)
{
    return GfVec3f(std::exp(-extinction[0] * distanceWld),
                   std::exp(-extinction[1] * distanceWld),
                   std::exp(-extinction[2] * distanceWld));
}

static float
_MinPositiveComponent(GfVec3f const& v)
{
    float result = std::numeric_limits<float>::max();
    for (int indexChannel = 0; indexChannel < 3; ++indexChannel) {
        if (v[indexChannel] > _kExtinctionEps) {
            result = std::min(result, v[indexChannel]);
        }
    }
    return result;
}

static bool
_HitOwnerGeometry(RTCRayHit const& rayHit, _SssWalkState const& state)
{
    return rayHit.hit.instID[0] == state.ownerInstanceId &&
           rayHit.hit.geomID == state.ownerGeomId;
}

static _SssTraceResult
_TraceSssBoundary(_SssWalkState const& state, HdEmbreeSssInput const& input,
                  RTCScene scene, float rayTfar)
{
    _SssTraceResult result;

    const bool useOwnerScene = (input.ownerScene != nullptr);
    const RTCScene traceScene = useOwnerScene ? input.ownerScene : scene;

    // Trace coordinates are world space for the top-level scene and owner
    // object space for a prototype scene.
    GfVec3f positionRayOriginScene = state.positionRayOriginWld;
    GfVec3f directionRayScene = state.directionRayWld;
    if (useOwnerScene) {
        positionRayOriginScene =
            input.worldToObjectMatrix.Transform(state.positionRayOriginWld);
        directionRayScene =
            input.worldToObjectMatrix.TransformDir(state.directionRayWld);
    }
    if (directionRayScene.GetLengthSq() <= 1.0e-20f) {
        return result;
    }

    RTCRayHit rayHit;
    rayHit.ray.flags = 0;
    rayHit.ray.org_x = positionRayOriginScene[0];
    rayHit.ray.org_y = positionRayOriginScene[1];
    rayHit.ray.org_z = positionRayOriginScene[2];
    rayHit.ray.tnear = _kBias;
    rayHit.ray.dir_x = directionRayScene[0];
    rayHit.ray.dir_y = directionRayScene[1];
    rayHit.ray.dir_z = directionRayScene[2];
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

    result.hitOwner = useOwnerScene ? (rayHit.hit.geomID == state.ownerGeomId)
                                    : _HitOwnerGeometry(rayHit, state);
    result.distanceWld = rayHit.ray.tfar;
    result.positionHitWld =
        state.positionRayOriginWld + state.directionRayWld * result.distanceWld;
    result.instanceId =
        useOwnerScene ? state.ownerInstanceId : rayHit.hit.instID[0];
    result.geomId = rayHit.hit.geomID;
    result.primId = rayHit.hit.primID;
    result.coordinateParametricU = rayHit.hit.u;
    result.coordinateParametricV = rayHit.hit.v;

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
    result.normalGeomObjExt = embreeObjectHitNormal;
    GfVec3f normalGeomWldExt = orientationSign * embreeObjectHitNormal;
    if (useOwnerScene) {
        normalGeomWldExt = _TransformNormalToWorld(input.worldToObjectMatrix,
                                                   normalGeomWldExt);
    }
    if (normalGeomWldExt.GetLengthSq() > 1.0e-20f) {
        normalGeomWldExt.Normalize();
    } else {
        normalGeomWldExt = -state.directionRayWld;
    }
    result.normalGeomWldExt = normalGeomWldExt;

    return result;
}

// Chiang 2016 remap (Cycles subsurface_random_walk_remap port, per-channel).
//
// Converts one (albedo, radius, anisotropy) channel into extinction and alpha.
// such that a random walk with these coefficients produces the target diffuse
// reflectance.
//
// Source: Blender Cycles src/kernel/integrator/subsurface_random_walk.h
// (Apache 2.0). Polynomial coefficients from Chiang et al. 2016.
static void
_ChiangRemapChannel(float albedo, float radius, float anisotropy,
                    float* extinctionOutput, float* rawAlphaOutput)
{
    const float anisotropy2 = anisotropy * anisotropy;
    const float anisotropy3 = anisotropy2 * anisotropy;
    const float anisotropy4 = anisotropy3 * anisotropy;
    const float anisotropy5 = anisotropy4 * anisotropy;
    const float anisotropy6 = anisotropy5 * anisotropy;
    const float anisotropy7 = anisotropy6 * anisotropy;

    const float coefficientA =
        1.8260523782f + -1.28451056436f * anisotropy +
        -1.79904629312f * anisotropy2 + 9.19393289202f * anisotropy3 +
        -22.8215585862f * anisotropy4 + 32.0234874259f * anisotropy5 +
        -23.6264803333f * anisotropy6 + 7.21067002658f * anisotropy7;
    const float coefficientB =
        4.98511194385f +
        0.127355959438f *
            std::exp(
                31.1491581433f * anisotropy + -201.847017512f * anisotropy2 +
                841.576016723f * anisotropy3 + -2018.09288505f * anisotropy4 +
                2731.71560286f * anisotropy5 + -1935.41424244f * anisotropy6 +
                559.009054474f * anisotropy7);
    const float coefficientC =
        1.09686102424f + -0.394704063468f * anisotropy +
        1.05258115941f * anisotropy2 + -8.83963712726f * anisotropy3 +
        28.8643230661f * anisotropy4 + -46.8802913581f * anisotropy5 +
        38.5402837518f * anisotropy6 + -12.7181042538f * anisotropy7;
    const float coefficientD =
        0.496310210422f + 0.360146581622f * anisotropy +
        -2.15139309747f * anisotropy2 + 17.8896899217f * anisotropy3 +
        -55.2984010333f * anisotropy4 + 82.065982243f * anisotropy5 +
        -58.5106008578f * anisotropy6 + 15.8478295021f * anisotropy7;
    const float coefficientE =
        4.23190299701f +
        0.00310603949088f *
            std::exp(
                76.7316253952f * anisotropy + -594.356773233f * anisotropy2 +
                2448.8834203f * anisotropy3 + -5576.68528998f * anisotropy4 +
                7116.60171912f * anisotropy5 + -4763.54467887f * anisotropy6 +
                1303.5318055f * anisotropy7);
    const float coefficientF =
        2.40602999408f + -2.51814844609f * anisotropy +
        9.18494908356f * anisotropy2 + -79.2191708682f * anisotropy3 +
        259.082868209f * anisotropy4 + -403.613804597f * anisotropy5 +
        302.85712436f * anisotropy6 + -87.4370473567f * anisotropy7;

    const float blend = std::pow(albedo, 0.25f);

    float alpha = (1.0f - blend) * coefficientA *
                      std::pow(std::atan(coefficientB * albedo), coefficientC) +
                  blend * coefficientD *
                      std::pow(std::atan(coefficientE * albedo), coefficientF);
    alpha = std::clamp(alpha, 0.0f, 0.999999f);

    const float extinctionPrime = 1.0f / std::max(radius, 1.0e-16f);
    const float extinction =
        extinctionPrime / std::max(1.0f - anisotropy, 1.0e-6f);

    *extinctionOutput = extinction;
    *rawAlphaOutput = alpha;
}

}  // namespace

void
HdEmbreeChiangRemap(const GfVec3f& albedo, const GfVec3f& radius,
                    float anisotropy, GfVec3f* extinctionOutput,
                    GfVec3f* alphaOutput, GfVec3f* rawAlphaOutput)
{
    const float anisotropyClamped = std::clamp(anisotropy, -0.99f, 0.99f);
    GfVec3f rawAlpha;
    for (int indexChannel = 0; indexChannel < 3; ++indexChannel) {
        _ChiangRemapChannel(
            std::clamp(albedo[indexChannel], 0.0f, 1.0f),
            std::max(radius[indexChannel], 1.0e-16f), anisotropyClamped,
            &(*extinctionOutput)[indexChannel], &rawAlpha[indexChannel]);
    }

    // Min-alpha clamp for numerical stability (Cycles pattern).
    // Callers use rawAlphaOutput to compute min-alpha throughput correction.
    constexpr float kMinAlpha = 0.2f;
    for (int indexChannel = 0; indexChannel < 3; ++indexChannel) {
        (*alphaOutput)[indexChannel] =
            std::max(rawAlpha[indexChannel], kMinAlpha);
    }
    if (rawAlphaOutput) {
        *rawAlphaOutput = rawAlpha;
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
HdEmbreeEvalPhaseDwivedi(float diffusionLength, float phaseLog, float cosTheta)
{
    // Eq. 9 from Meng-Hanika-Dachsbacher 2016, using precomputed phaseLog.
    // Source: Blender Cycles (Apache 2.0).
    return 1.0f / (std::max(diffusionLength - cosTheta, 1.0e-6f) * phaseLog);
}

float
HdEmbreeSamplePhaseDwivedi(float diffusionLength, float phaseLog, float u1)
{
    // Eq. 10 from Meng-Hanika-Dachsbacher 2016.
    // Returns cosTheta. Using L for diffusion length, the inverse CDF is:
    //   cosTheta = L - (L+1) * pow((L-1)/(L+1), u)
    // Implemented with precomputed phaseLog = log((L+1)/(L-1)).
    // Source: Blender Cycles (Apache 2.0).
    const float uClamped = std::clamp(u1, 0.0f, 1.0f);
    return diffusionLength -
           (diffusionLength + 1.0f) * std::exp(-uClamped * phaseLog);
}

float
HdEmbreeBackwardDwivediFraction(float distanceOppositeWld,
                                float distanceFromEntryPlaneWld,
                                float diffusionLength)
{
    if (distanceOppositeWld <= 0.0f || diffusionLength <= 0.0f) {
        return 0.0f;
    }

    const float distanceFromEntryPlaneClampedWld =
        std::clamp(distanceFromEntryPlaneWld, 0.0f, distanceOppositeWld);
    const float exponent =
        (distanceOppositeWld - 2.0f * distanceFromEntryPlaneClampedWld) /
        diffusionLength;
    return 1.0f / (1.0f + std::exp(exponent));
}

HdEmbreeSssOutput
HdEmbreeRandomWalkSSS(HdEmbreeSssInput const& input,
                      HdEmbreeSampleDomain const& domain, RTCScene scene)
{
    // Phase 3: after kSimilarityLevel bounces we switch to isotropic scattering
    // with the similarity-reduced sigma values (Wrenninge-Villemin-Hery).
    // The guidedFraction is fixed at 0.75 in that regime since there is no
    // HG forward peak to compete with.
    constexpr int kSimilarityLevel = 9;
    constexpr float kReducedGuidedFraction = 0.75f;
    constexpr float kTwoPi = 2.0f * 3.14159265358979323846f;
    constexpr float kInvTwoPi = 1.0f / (2.0f * 3.14159265358979323846f);

    HdEmbreeSssOutput output; // default-initialized (success=false)

    _SssWalkState state;
    GfVec3f throughputCorrection;
    if (input.usePrecomputedCoefficients) {
        _InitPrecomputedCoefficients(input.precomputedAbsorption,
                                     input.precomputedScattering,
                                     &state.extinction, &state.scattering,
                                     &state.alpha, &throughputCorrection);
    } else {
        _InitChiangCoefficients(input.albedo, input.radius, input.anisotropy,
                                &state.extinction, &state.scattering,
                                &state.alpha, &throughputCorrection);
    }
    state.throughputRgb = throughputCorrection;
    state.positionRayOriginWld = input.positionEntryWld;
    state.directionRayWld = input.directionEntryWld;
    state.anisotropy = std::clamp(input.anisotropy, -0.99f, 0.99f);
    state.positionEntryWld = input.positionEntryWld;
    state.normalShdGuideWldOut = input.normalShdEntryGuideWldOut;
    state.ownerInstanceId = input.ownerInstanceId;
    state.ownerGeomId = input.ownerGeomId;
    // haveOppositeInterface / distanceOppositeWld start empty and may be
    // populated by the extended first-bounce ray below.

    if (!_InitDwivediAndSimilarity(&state)) {
        return output; // degenerate diffusion length
    }

    // Keep the existing material-resolved entry guide for Dwivedi sampling.
    // Changing it to a geometric normal would alter transport behavior.
    const GfVec3f guideAxis = state.normalShdGuideWldOut;

    for (int bounce = 0; bounce < _kSssMaxBounces; ++bounce) {
        ++output.walkSteps;
        const HdEmbreeSampleDomain bounceDomain =
            domain.Chain(HdEmbreeSampleDomainKey::SssBounce, bounce);

        // Similarity switch: after kSimilarityLevel the medium is approximated
        // as isotropic with reduced scattering.
        GfVec3f extinctionEffective;
        GfVec3f scatteringEffective;
        float anisotropyEffective;
        float guidedFractionEffective;
        if (bounce <= kSimilarityLevel) {
            extinctionEffective = state.extinction;
            scatteringEffective = state.scattering;
            anisotropyEffective = state.anisotropy;
            guidedFractionEffective = state.guidedFraction;
        } else {
            extinctionEffective = state.extinctionStar;
            scatteringEffective = state.scatteringStar;
            anisotropyEffective = 0.0f; // isotropic regime
            guidedFractionEffective = kReducedGuidedFraction;
        }

        // Chiang channel selection MIS (balance heuristic) per bounce.
        mxcpp::Vec3f channelPdfMx;
        const int channel = mxcpp::ChannelMIS(
            /*throughput*/ mxcpp::Vec3f(state.throughputRgb[0],
                                        state.throughputRgb[1],
                                        state.throughputRgb[2]),
            /*weights*/
            mxcpp::Vec3f(state.alpha[0], state.alpha[1], state.alpha[2]),
            bounceDomain.Fork(HdEmbreeSampleDomainKey::SssChannel).Draw1D(),
            &channelPdfMx);
        const GfVec3f channelPdf(
            channelPdfMx[0], channelPdfMx[1], channelPdfMx[2]);

        float sampleExtinction = extinctionEffective[channel];
        if (sampleExtinction <= _kExtinctionEps) {
            return output; // degenerate medium
        }

        // Direction sampling.
        // bounce 0 uses the pre-set entry direction (no guiding possible yet).
        // bounce > 0 picks guided (Dwivedi around guideAxis) vs classic (HG)
        // stochastically with probability `guidedFractionEffective`.
        float stretchingForward = 1.0f;
        float pdfFactorForward = 0.0f;

        float stretchingBackward = 1.0f;
        float pdfFactorBackward = 0.0f;
        float backwardFraction = 0.0f;

        bool guidedThisBounce = false;
        bool guideBackward = false;
        if (bounce > 0) {
            guidedThisBounce =
                (bounceDomain.Fork(HdEmbreeSampleDomainKey::SssGuideChoice)
                     .Draw1D() < guidedFractionEffective);

            if (state.haveOppositeInterface) {
                const float distanceFromEntryPlaneWld =
                    GfDot(state.positionRayOriginWld - state.positionEntryWld,
                          -state.normalShdGuideWldOut);
                backwardFraction = HdEmbreeBackwardDwivediFraction(
                    state.distanceOppositeWld, distanceFromEntryPlaneWld,
                    state.diffusionLength);
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
            const float u1 = phaseSample[0];
            const float u2 = phaseSample[1];

            GfVec3f directionScatteredWld;
            float cosThetaEntry = 0.0f;
            float pdfHenyeyGreenstein = 1.0f;

            if (guidedThisBounce) {
                // Forward Dwivedi: sample cosTheta around the inward-facing
                // entry direction (guideAxis).
                float cosTheta = HdEmbreeSamplePhaseDwivedi(
                    state.diffusionLength, state.phaseLog, u1);
                // Backward Dwivedi mirrors the guide distribution along the
                // entry normal, biasing directions toward the opposite side.
                if (guideBackward) {
                    cosTheta = -cosTheta;
                }

                cosThetaEntry = cosTheta;

                GfVec3f axisGuideX, axisGuideY;
                _BuildOrthonormalBasis(guideAxis, &axisGuideX, &axisGuideY);
                const float sinTheta =
                    std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
                const float phi = kTwoPi * u2;
                directionScatteredWld =
                    axisGuideX * (sinTheta * std::cos(phi)) +
                    axisGuideY * (sinTheta * std::sin(phi)) +
                    guideAxis * cosTheta;
                if (directionScatteredWld.GetLengthSq() > 1.0e-20f) {
                    directionScatteredWld.Normalize();
                } else {
                    directionScatteredWld = guideAxis;
                }

                pdfHenyeyGreenstein = mxcpp::PdfHenyeyGreenstein(
                    mxcpp::Vec3f(directionScatteredWld[0],
                                 directionScatteredWld[1],
                                 directionScatteredWld[2]),
                    mxcpp::Vec3f(-state.directionRayWld[0],
                                 -state.directionRayWld[1],
                                 -state.directionRayWld[2]),
                    anisotropyEffective);
            } else {
                // Classic HG sampling around the incoming ray direction.
                const mxcpp::Vec3f omegaOutWld(-state.directionRayWld[0],
                                               -state.directionRayWld[1],
                                               -state.directionRayWld[2]);
                const mxcpp::Vec3f sampled = mxcpp::SampleHenyeyGreenstein(
                    omegaOutWld, anisotropyEffective, u1, u2);
                directionScatteredWld =
                    GfVec3f(sampled[0], sampled[1], sampled[2]);
                cosThetaEntry = GfDot(directionScatteredWld, guideAxis);
                pdfHenyeyGreenstein = mxcpp::PdfHenyeyGreenstein(
                    mxcpp::Vec3f(directionScatteredWld[0],
                                 directionScatteredWld[1],
                                 directionScatteredWld[2]),
                    mxcpp::Vec3f(-state.directionRayWld[0],
                                 -state.directionRayWld[1],
                                 -state.directionRayWld[2]),
                    anisotropyEffective);
            }

            state.directionRayWld = directionScatteredWld;

            // Guided PDF factor used later to mix into the classic PDF.
            // pdfFactorForward converts the classic (scattering * T) PDF into
            // the guided-direction PDF by multiplying by
            //   (1/(2*pi)) * p_Dwivedi(cosThetaEntry) / p_HG(cosTheta_ray)
            // (the 1/(2*pi) captures the azimuth, which Dwivedi samples
            //  uniformly while HG already includes it in its normalization).
            const float pdfHenyeyGreensteinSafe =
                std::max(pdfHenyeyGreenstein, 1.0e-8f);
            pdfFactorForward =
                kInvTwoPi *
                HdEmbreeEvalPhaseDwivedi(state.diffusionLength, state.phaseLog,
                                         cosThetaEntry) /
                pdfHenyeyGreensteinSafe;
            pdfFactorBackward =
                kInvTwoPi *
                HdEmbreeEvalPhaseDwivedi(state.diffusionLength, state.phaseLog,
                                         -cosThetaEntry) /
                pdfHenyeyGreensteinSafe;

            stretchingForward = 1.0f - cosThetaEntry / state.diffusionLength;
            stretchingBackward = 1.0f + cosThetaEntry / state.diffusionLength;

            if (guidedThisBounce) {
                sampleExtinction *=
                    guideBackward ? stretchingBackward : stretchingForward;
            }
        }

        // Free-flight distance sample for the (possibly stretched) extinction.
        const float u1 = std::clamp(
            bounceDomain.Fork(HdEmbreeSampleDomainKey::SssFreeFlight).Draw1D(),
            1.0e-6f, 1.0f - 1.0e-6f);
        const float distanceFreeFlightSampledWld =
            -std::log(1.0f - u1) / std::max(sampleExtinction, _kExtinctionEps);
        float distanceSegmentWld = distanceFreeFlightSampledWld;

        // Ray cast (use Camera mask to match the path tracer's visibility set).
        float distanceRayMaximumWld = distanceFreeFlightSampledWld;
        if (bounce == 0) {
            const float minExtinction =
                _MinPositiveComponent(extinctionEffective);
            if (minExtinction < std::numeric_limits<float>::max()) {
                distanceRayMaximumWld = std::max(distanceFreeFlightSampledWld,
                                                 10.0f / minExtinction);
            }
        }

        const _SssTraceResult trace =
            _TraceSssBoundary(state, input, scene, distanceRayMaximumWld);
        ++output.intersectionTests;

        bool hit = trace.foundSurface && trace.hitOwner;
        if (bounce == 0) {
            if (trace.foundSurface && trace.hitOwner) {
                const float distanceOppositeWld =
                    GfDot(trace.positionHitWld - state.positionEntryWld,
                          -state.normalShdGuideWldOut);
                if (distanceOppositeWld > _kBias) {
                    state.haveOppositeInterface = true;
                    state.distanceOppositeWld = distanceOppositeWld;
                }
            }

            // The extended first ray is only for detecting the opposite
            // interface. The actual walk still scatters if the sampled free
            // flight distance is shorter than the first surface hit.
            hit = trace.foundSurface && trace.hitOwner &&
                  trace.distanceWld < distanceFreeFlightSampledWld;
        }

        if (hit) {
            distanceSegmentWld = trace.distanceWld;
        }

        // Classic sampling PDF + contribution.
        //   hit:     sampleContrib = T(t),                classicPdf = T(t)
        //   scatter: sampleContrib = scattering * T(t),      classicPdf =
        //   extinction * T(t)
        const GfVec3f transmittance =
            _EvalTransmittance(extinctionEffective, distanceSegmentWld);
        GfVec3f classicPdf;
        GfVec3f sampleContrib;
        if (hit) {
            classicPdf = transmittance;
            sampleContrib = transmittance;
        } else {
            // Distance-sampling PDF uses extinction (exponential sampling pdf).
            classicPdf = GfCompMult(extinctionEffective, transmittance);
            // Throughput numerator uses scattering (scatter albedo weighting).
            sampleContrib = GfCompMult(scatteringEffective, transmittance);
        }

        // MIS blend between classic and guided (Dwivedi) sampling PDFs.
        GfVec3f pdf = classicPdf;
        if (bounce > 0) {
            // Guided PDF uses the stretched extinction (distance was sampled
            // with the stretched extinction when guided, and the PDF has to
            // match that distribution for correct MIS weighting).
            const GfVec3f extinctionStretched =
                extinctionEffective * stretchingForward;
            const GfVec3f transmittanceStretched =
                _EvalTransmittance(extinctionStretched, distanceSegmentWld);
            GfVec3f pdfGuidedForward;
            if (hit) {
                pdfGuidedForward = transmittanceStretched;
            } else {
                // Match Cycles' subsurface_random_walk_pdf: pdf = extinction *
                // T for scatter.
                pdfGuidedForward =
                    GfCompMult(extinctionStretched, transmittanceStretched);
            }
            pdfGuidedForward = pdfGuidedForward * pdfFactorForward;

            GfVec3f guidedPdf = pdfGuidedForward;
            if (state.haveOppositeInterface) {
                const GfVec3f extinctionBackwardStretched =
                    extinctionEffective * stretchingBackward;
                const GfVec3f transmittanceBackwardStretched =
                    _EvalTransmittance(extinctionBackwardStretched,
                                       distanceSegmentWld);
                GfVec3f pdfGuidedBackward;
                if (hit) {
                    pdfGuidedBackward = transmittanceBackwardStretched;
                } else {
                    pdfGuidedBackward =
                        GfCompMult(extinctionBackwardStretched,
                                   transmittanceBackwardStretched);
                }
                pdfGuidedBackward = pdfGuidedBackward * pdfFactorBackward;

                guidedPdf = pdfGuidedForward * (1.0f - backwardFraction) +
                            pdfGuidedBackward * backwardFraction;
            }

            pdf = classicPdf * (1.0f - guidedFractionEffective) +
                  guidedPdf * guidedFractionEffective;
        }

        const float denom = GfDot(channelPdf, pdf);
        if (!std::isfinite(denom) || denom <= 1.0e-20f) {
            return output;
        }
        state.throughputRgb =
            GfCompMult(state.throughputRgb, sampleContrib * (1.0f / denom));

        // Termination on degenerate / near-zero throughput.
        const float maxTp =
            std::max({state.throughputRgb[0], state.throughputRgb[1],
                      state.throughputRgb[2]});
        if (!std::isfinite(maxTp) || maxTp < _kVolumeThroughputEps) {
            return output;
        }

        if (hit) {
            // Build exit info from the surface intersection.
            const GfVec3f positionHitWld = trace.positionHitWld;
            GfVec3f normalGeomWldExt = trace.normalGeomWldExt;
            GfVec3f normalGeomObjExt = trace.normalGeomObjExt;
            if (normalGeomWldExt.GetLengthSq() > 1.0e-20f) {
                normalGeomWldExt.Normalize();
            } else {
                normalGeomWldExt = -state.directionRayWld; // fallback
                normalGeomObjExt = _TransformNormalToObject(
                    input.objectToWorldMatrix, normalGeomWldExt);
            }
            // Orient outward: the exit normal should align with the ray
            // direction (the ray leaves the medium, so the outward face's
            // normal is in the same half-space as directionRayWld). Flip only
            // if Embree returned an inward Ng.
            if (GfDot(normalGeomWldExt, state.directionRayWld) < 0.0f) {
                normalGeomWldExt = -normalGeomWldExt;
                normalGeomObjExt = -normalGeomObjExt;
            }
            if (normalGeomObjExt.GetLengthSq() > 1.0e-20f) {
                normalGeomObjExt.Normalize();
            }

            output.success = true;
            output.positionExitWld = positionHitWld;
            output.normalGeomExitWldExt = normalGeomWldExt;
            output.directionExitWld = state.directionRayWld;
            output.normalGeomExitObjExt = normalGeomObjExt;
            output.exitInstanceId = trace.instanceId;
            output.exitGeomId = trace.geomId;
            output.exitPrimId = trace.primId;
            output.coordinateParametricExitU = trace.coordinateParametricU;
            output.coordinateParametricExitV = trace.coordinateParametricV;
            output.throughputWeight = state.throughputRgb;
            return output;
        }

        // Advance ray origin to the scatter point for the next bounce.
        state.positionRayOriginWld +=
            state.directionRayWld * distanceSegmentWld;
    }

    return output; // max bounces exceeded
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

    GfVec3f entryDirection = input.directionEntryWld;
    if (!input.hasSampledEntryDirection) {
        const GfVec2f sample =
            domain.Fork(HdEmbreeSampleDomainKey::SssEntryDirection).Draw2D();
        mxcpp::Vec3f sampledDirection;
        if (!mxcpp::Bsdf::SampleSubsurfaceEntry(
                *input.closure, _ToMx(input.normalShdWldOut),
                _ToMx(input.omegaOutWld), sample[0], sample[1],
                sampledDirection)) {
            return _SubsurfaceResult::Terminate;
        }
        entryDirection = _ToGf(sampledDirection);
    }

    // A direction that is inward in the shading frame can still point out of
    // the true face when a normal map strongly tilts the frame.
    if (GfDot(input.normalGeomWldOut, entryDirection) >= 0.0f) {
        return _SubsurfaceResult::Terminate;
    }

    GfVec3f entryWeight = input.entryWeight;
    for (int indexChannel = 0; indexChannel < 3; ++indexChannel) {
        if (!std::isfinite(entryWeight[indexChannel]) ||
            entryWeight[indexChannel] < 0.0f) {
            entryWeight[indexChannel] = 0.0f;
        }
    }
    _ApplyPathWeight(entryWeight, state);
    if (_IsNearlyBlack(
            _GetPathThroughputRgb(*state), _minLuminanceCutoff)) {
        return _SubsurfaceResult::Terminate;
    }

    HdEmbreeSssInput walkInput;
    walkInput.positionEntryWld = input.positionHitWld;
    walkInput.normalShdEntryGuideWldOut = input.normalShdWldOut;
    walkInput.directionEntryWld = entryDirection;
    walkInput.albedo = _ToGf(input.closure->subsurfaceColor);
    walkInput.radius = GfCompMult(
        _ToGf(input.closure->subsurfaceRadius),
        _ToGf(input.closure->subsurfaceRadiusScale));
    walkInput.anisotropy =
        std::clamp(input.closure->subsurfaceAnisotropy, -0.99f, 0.99f);
    if (input.closure->hasPrecomputedSubsurfaceMedium &&
        !input.closure->precomputedSubsurfaceMedium.IsVacuum()) {
        walkInput.usePrecomputedCoefficients = true;
        walkInput.precomputedAbsorption =
            _ToGf(input.closure->precomputedSubsurfaceMedium.absorption);
        walkInput.precomputedScattering =
            _ToGf(input.closure->precomputedSubsurfaceMedium.scattering);
        walkInput.anisotropy = std::clamp(
            input.closure->precomputedSubsurfaceMedium.anisotropy,
            -0.99f,
            0.99f);
    }
    walkInput.iorInterior = std::max(input.closure->specularIor, 1.0f);
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
    for (int indexChannel = 0; indexChannel < 3; ++indexChannel) {
        if (!std::isfinite(walkWeight[indexChannel]) ||
            walkWeight[indexChannel] < 0.0f) {
            walkWeight[indexChannel] = 0.0f;
        }
    }
    _ApplyPathWeight(walkWeight, state);
    if (_IsNearlyBlack(
            _GetPathThroughputRgb(*state), _minLuminanceCutoff)) {
        return _SubsurfaceResult::Terminate;
    }

    state->positionRayOriginWld = output.positionExitWld;
    state->directionRayWld = -output.directionExitWld;
    state->rayDifferential.hasDifferentials = false;
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
