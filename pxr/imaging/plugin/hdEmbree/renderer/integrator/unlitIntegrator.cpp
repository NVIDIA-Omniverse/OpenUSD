//
// Copyright 2018 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
// Single-hit unlit, camera-light, and ambient-occlusion integration.

#include <renderer/geometry/primvarSampling.h>
#include <renderer/materials/MaterialXCpp/graph.h>
#include <renderer/materials/MaterialXCpp/shadingContext.h>
#include <renderer/rayUtil.h>
#include <renderer/renderer.h>
#include <renderer/rendererMath.h>

PXR_NAMESPACE_OPEN_SCOPE

/// Generate a random cosine-weighted direction ray (in the hemisphere
/// around <0,0,1>).  The input is a pair of uniformly distributed random
/// numbers in the range [0,1].
///
/// The algorithm here is to generate a random point on the disk, and project
/// that point to the unit hemisphere.
static GfVec3f
_CosineWeightedDirection(GfVec2f const& uniformSamples)
{
    GfVec3f directionLocal;
    float angleAzimuth = 2.0f * ty::Pi<float> * uniformSamples[0];
    float u2 = uniformSamples[1];
    float radiusDisk = sqrtf(u2);
    directionLocal[0] = cosf(angleAzimuth) * radiusDisk;
    directionLocal[1] = sinf(angleAzimuth) * radiusDisk;
    directionLocal[2] = sqrtf(1.0f - u2);
    return directionLocal;
}

HdEmbreeRenderer::_PixelSampleResult
HdEmbreeRenderer::_IntegrateUnlit(
    GfVec3f const& origin,
    GfVec3f const& dir,
    HdEmbreeRayDifferential const& rayDiff,
    HdEmbreeSampleDomain const& domain)
{
    _PixelSampleResult result;
    RTCRayHit& rayHit = result.primaryHit;
    rayHit.ray.flags = 0;
    ty::PopulateRayHit(
        &rayHit, origin, dir, 0.0f,
        std::numeric_limits<float>::max(),
        HdEmbree_RayMask::Camera);
    rtcIntersect1(_scene, &rayHit);

    if (_IsEdgeOnlyWireframeHit(rayHit)) {
        // Skip the camera headlight, material evaluation, and ambient
        // occlusion. _ApplyWireframe() will draw solid black coverage.
        return result;
    }

    if (rayHit.hit.geomID == RTC_INVALID_GEOMETRY_ID) {
        result.color = GfVec4f(
            _colorClearValue[0],
            _colorClearValue[1],
            _colorClearValue[2],
            1.0f);
        return result;
    }

    // Finite lights are black in the explicitly unlit integrator.
    if (_GetLightGeometryHit(rayHit)) {
        result.color = GfVec4f(0.0f, 0.0f, 0.0f, 1.0f);
        return result;
    }

    // Construct the same outward topology and incident material frame used
    // by the path and visibility integrators.
    const GfVec3f omegaOutWld =
        -GfVec3f(rayHit.ray.dir_x, rayHit.ray.dir_y, rayHit.ray.dir_z);
    _SurfaceInteraction interaction;
    HdEmbreeInstanceContext const* instanceContext = nullptr;
    HdEmbreePrototypeContext const* prototypeContext = nullptr;
    if (!_TryBuildSurfaceInteraction(rayHit, omegaOutWld, &interaction,
                                     &instanceContext, &prototypeContext)) {
        result.color = GfVec4f(0.0f, 0.0f, 0.0f, 1.0f);
        return result;
    }
    const GfVec3f positionHitWld = interaction.positionHitWld;
    GfVec3f normalShdWldOut = interaction.GetNormalSrfWldOut();
    // Build shading context via shared helper (texcoord, displayColor,
    // tangent frame all constructed consistently).
    mxcpp::ShadingContext ctx = _BuildShadingContext(
        rayHit, rayDiff, instanceContext, prototypeContext, interaction);
    HdEmbreePrimvarLookup cbData{
        &prototypeContext->geomPropSamplers,
        rayHit.hit.primID, rayHit.hit.u, rayHit.hit.v};
    ctx.geomPropLookup = &HdEmbreeSamplePrimvar;
    ctx.geomPropUserData = &cbData;
    ctx.uniformProps = &prototypeContext->geomPropUniformValues;

    // Recover the tangent frame used to apply a material normal map.
    GfVec3f tangent = ty::ToGf(ctx.tangent);
    GfVec3f bitangent = ty::ToGf(ctx.bitangent);

    // Try to evaluate MaterialXCpp material if one is bound.
    mxcpp::EvalGraph* surfaceGraph = prototypeContext->material
        ? prototypeContext->material->surfaceGraph
        : nullptr;

    mxcpp::SurfaceClosure closure;
    bool hasMaterialClosure = false;

    if (surfaceGraph) {
        mxcpp::EvalOptions evalOptions;
        evalOptions.useAdobeOpenPBR = _settings.useAdobeOpenPBR;
        closure = surfaceGraph->Evaluate(ctx, evalOptions);
        hasMaterialClosure = true;
    }

    if (hasMaterialClosure) {
        mxcpp::Vec3f resolvedNormal;
        if (closure.ResolveNormal(ty::ToMx(tangent), ty::ToMx(bitangent),
                                  ty::ToMx(normalShdWldOut), &resolvedNormal)) {
            GfVec3f candidate;
            const bool valid =
                ty::TryNormalizeDirection(
                    ty::ToGf(resolvedNormal), &candidate) &&
                GfDot(candidate, interaction.GetNormalGeomWldOut()) > 0.0f &&
                GfDot(candidate, omegaOutWld) > 0.0f;
            if (valid) {
                normalShdWldOut = candidate;
            } else {
                ++_invalidMaterialNormalCount;
            }
        }
    }

    GfVec3f materialColor;
    if (hasMaterialClosure) {
        materialColor = ty::ToGf(closure.baseColor);
    } else {
        materialColor = _settings.enableSceneColors
            ? ty::ToGf(ctx.displayColor) : GfVec3f(0.5f);
    }

    // The unlit integrator uses a camera-facing headlight, optionally
    // attenuated by ambient occlusion, with the resolved material normal.
    const GfVec3f rawDir(
        rayHit.ray.dir_x, rayHit.ray.dir_y, rayHit.ray.dir_z);
    float diffuseLight = fabs(GfDot(-rawDir, normalShdWldOut)) *
                         HdEmbreeCameraLightIntensity;

    float aoLightIntensity = _ComputeAmbientOcclusion(
        positionHitWld, normalShdWldOut, interaction.normalGeomWldExt,
        domain.Fork(HdEmbreeSampleDomainKey::AmbientOcclusion));

    const GfVec3f lightingColor =
        materialColor * diffuseLight * aoLightIntensity;

    GfVec4f output;
    output[0] = std::max(0.0f, lightingColor[0]);
    output[1] = std::max(0.0f, lightingColor[1]);
    output[2] = std::max(0.0f, lightingColor[2]);
    output[3] = 1.0f;
    result.color = output;
    return result;
}

float
HdEmbreeRenderer::_ComputeAmbientOcclusion(GfVec3f const& positionWld,
                                           GfVec3f const& normalShdWldOut,
                                           GfVec3f const& normalGeomWldExt,
                                           HdEmbreeSampleDomain const& domain)
{
    // 0 ambient occlusion samples means disable the ambient occlusion term.
    if (_settings.ambientOcclusionSamples < 1) {
        return 1.0f;
    }

    float visibility = 0.0f;

    // For hemisphere sampling we need to choose a coordinate frame at this
    // point. For _CosineWeightedDirection, normalShdWldOut must map to
    // (0,0,1), but radial symmetry makes the other axes arbitrary.
    GfMatrix3f basis(1.0f);
    GfVec3f xAxis;
    if (fabsf(GfDot(normalShdWldOut, GfVec3f(0.0f, 0.0f, 1.0f))) < 0.9f) {
        xAxis = GfCross(normalShdWldOut, GfVec3f(0.0f, 0.0f, 1.0f));
    } else {
        xAxis = GfCross(normalShdWldOut, GfVec3f(0.0f, 1.0f, 0.0f));
    }
    GfVec3f yAxis = GfCross(normalShdWldOut, xAxis);
    basis.SetColumn(0, xAxis.GetNormalized());
    basis.SetColumn(1, yAxis.GetNormalized());
    basis.SetColumn(2, normalShdWldOut);

    // Generate random samples, stratified with Latin Hypercube Sampling.
    // https://en.wikipedia.org/wiki/Latin_hypercube_sampling
    // Stratified sampling means we don't get all of our random samples
    // bunched in the far corner of the hemisphere, but instead have some
    // equal spacing guarantees.
    std::vector<GfVec2f> samples;
    samples.resize(_settings.ambientOcclusionSamples);
    std::vector<float> yJitter;
    yJitter.resize(_settings.ambientOcclusionSamples);
    for (int i = 0; i < _settings.ambientOcclusionSamples; ++i) {
        const GfVec2f sample =
            domain
                .Split(
                    HdEmbreeSampleDomainKey::AmbientOcclusionSample,
                    _settings.ambientOcclusionSamples,
                    i)
                .Draw2D();
        samples[i][0] =
            (static_cast<float>(i) + sample[0]) /
            _settings.ambientOcclusionSamples;
        yJitter[i] = sample[1];
    }
    // Fisher-Yates shuffle using a separate sampler domain.
    for (int i = _settings.ambientOcclusionSamples - 1; i > 0; --i) {
        int j = static_cast<int>(
            domain
                .Chain(HdEmbreeSampleDomainKey::AmbientOcclusionShuffle, i)
                .Draw1D() * (i + 1));
        j = std::min(j, i);
        std::swap(samples[i], samples[j]);
    }
    for (int i = 0; i < _settings.ambientOcclusionSamples; ++i) {
        samples[i][1] =
            (static_cast<float>(i) + yJitter[i]) /
            _settings.ambientOcclusionSamples;
    }

    // Ambient visibility is the fraction of the hemisphere that is unoccluded
    // when rays are traced to infinity.
    const GfVec3f rayOrigin = ty::OffsetRayOrigin(
        positionWld, normalGeomWldExt, normalShdWldOut, 1e-4f);
    for (int i = 0; i < _settings.ambientOcclusionSamples; i++)
    {
        // Sample in the hemisphere centered on normalShdWldOut. Use
        // cosine-weighting to favor directions with more influence on AO.
        GfVec3f shadowDir = basis * _CosineWeightedDirection(samples[i]);

        // Trace shadow ray, using the fast interface (rtcOccluded) since
        // we only care about intersection status, not intersection id.
        RTCRay shadow;
        shadow.flags = 0;
        ty::PopulateRay(&shadow, rayOrigin, shadowDir, 1e-4f);
        {
          rtcOccluded1(_scene, &shadow);
        }

        // Record this AO ray's contribution to ambient visibility.
        // Since we use cosine-weighted hemisphere sampling (PDF ∝ cos θ),
        // the Monte Carlo estimator for the Lambertian ambient integral
        // reduces to a simple visibility average: 1 if visible, 0 if
        // occluded.
        // rtcOccluded1 sets shadow.tfar to -inf on occlusion, so tfar > 0
        // means the sampled direction is unoccluded.
        if (shadow.tfar > 0.0f)
            visibility += 1.0f;
    }
    // Average the ambient-visibility samples.
    visibility /= _settings.ambientOcclusionSamples;

    return visibility;
}

PXR_NAMESPACE_CLOSE_SCOPE
