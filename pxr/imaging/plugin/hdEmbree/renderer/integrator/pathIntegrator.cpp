//
// Copyright 2018 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
// Main multi-bounce path integration loop.

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

GfVec3f
HdEmbreeRenderer::_WeightPathRadiance(
    GfVec3f const& value, _PathState const& state) const
{
    if (!state.hero.active) {
        return GfCompMult(state.throughput, value);
    }
    const _HeroWavelengthState hero{
        true, state.hero.wavelengthNm, state.hero.pdf};
    return _SpectralValueToRgb(
        state.spectralThroughput *
            _RgbToSpectralValue(value, hero, _renderColorSpace),
        hero,
        _renderColorSpace);
}

void
HdEmbreeRenderer::_AddPathRadiance(
    GfVec3f contribution, _PathState* state) const
{
    if (!state) {
        return;
    }
    if (state->currentPathIsCaustic) {
        contribution = _ClampFireflyContribution(
            contribution,
            _causticsClampThreshold,
            _materialEvalServices.luminanceCoefficients);
    }
    state->radiance += _ClampFireflyContribution(
        contribution,
        _fireflyClampThreshold,
        _materialEvalServices.luminanceCoefficients);
}

HdEmbreeRenderer::_PixelSampleResult
HdEmbreeRenderer::_IntegratePath(
    GfVec3f const& origin,
    GfVec3f const& dir,
    HdEmbreeRayDifferential const& rayDiff,
    HdEmbreeSampleDomain const& domain) const
{
    // Initialize accumulated output and mutable transport state. The
    // primary hit is captured once for AOVs while this state advances.
    _PixelSampleResult result;
    _PathState path;
    path.rayOrigin = origin;
    path.rayDir = dir;
    path.rayDiff = rayDiff;

    // Retain surface derivatives until the BSDF sample is known. Specular
    // continuations use them to propagate the camera-ray footprint.
    _SurfaceDifferentials surfaceDifferentials;

    const int maxBounces = std::max(0, _maxBounces);

    // bounce counts real scatters; pathEvent advances every iteration so
    // pass-through events still get distinct deterministic sample domains.
    // One extra iteration is allowed to resolve an emitter only.
    for (int bounce = 0, pathEvent = 0;
         bounce <= maxBounces + 1;
         ++pathEvent) {
        const bool emitterOnlyBounce = bounce > maxBounces;
        const HdEmbreeSampleDomain bounceDomain =
            domain.Chain(HdEmbreeSampleDomainKey::PathBounce, pathEvent);

        // Find the pending segment endpoint. Synthetic SSS exits already
        // carry Embree identity/barycentrics; other segments trace the scene.
        RTCRayHit rayHit;
        rayHit.ray.flags = 0;
        const bool syntheticLambertianHit = path.useSyntheticLambertian;
        if (syntheticLambertianHit) {
            if (!_PopulateSssExitRayHit(&rayHit, path.syntheticLambertianExit)) {
                break;
            }
        } else {
            _PopulateRayHit(&rayHit, path.rayOrigin, path.rayDir,
                            path.isFirstBounce ? 0.0f : 1e-4f,
                            std::numeric_limits<float>::max(),
                            emitterOnlyBounce
                                ? HdEmbree_RayMask::Light
                                : HdEmbree_RayMask::Camera);
            rtcIntersect1(_scene, &rayHit);
        }

        // Preserve the camera result once for geometric AOVs; later path
        // intersections must not replace it.
        if (pathEvent == 0) {
            result.primaryHit = rayHit;
            if (_IsEdgeOnlyWireframeHit(rayHit)) {
                // Wireframe-only is a display diagnostic, not a shaded
                // surface. Retain the hit for wire coverage and geometric
                // AOVs, but skip materials, lights, volumes, and path bounces.
                return result;
            }
        }

        // Resolve competing endpoints. Analytic finite lights are searched
        // on indirect rays and compete with Embree geometry by distance.
        HdEmbreeLightSampler::LightSample finiteLightHit{};
        TfToken finiteLightLink;
        const bool hitLightGeometry =
            !syntheticLambertianHit &&
            _EvaluateLightGeometryHit(
                rayHit, path.rayOrigin, path.rayDir, &finiteLightHit, &finiteLightLink);
        const float surfaceDist =
            rayHit.hit.geomID != RTC_INVALID_GEOMETRY_ID && !hitLightGeometry
                ? rayHit.ray.tfar
                : std::numeric_limits<float>::infinity();
        const bool hasAnalyticFiniteLightHit =
            !syntheticLambertianHit &&
            !hitLightGeometry &&
            !path.isFirstBounce &&
            _FindNearestFiniteLightHit(
                path.rayOrigin,
                path.rayDir,
                surfaceDist,
                &finiteLightHit,
                &finiteLightLink);
        const bool hasFiniteLightHit =
            hitLightGeometry || hasAnalyticFiniteLightHit;
        const float finiteLightDist =
            hasFiniteLightHit
                ? finiteLightHit.dist
                : std::numeric_limits<float>::infinity();

        // Transport through the active medium before processing either
        // endpoint: free flight may absorb or scatter before the surface.
        if (path.medium.active) {
            _VolumeTransmissionInput volumeInput;
            volumeInput.surfaceDist = surfaceDist;
            volumeInput.hasFiniteLightHit = hasFiniteLightHit;
            volumeInput.finiteLightHit = finiteLightHit;
            volumeInput.finiteLightLink = finiteLightLink;
            volumeInput.finiteLightDist = finiteLightDist;
            volumeInput.bounce = bounce;

            const _VolumeTransmissionResult volumeResult =
                _TraceVolumeTransmission(volumeInput, bounceDomain, &path);
            if (volumeResult == _VolumeTransmissionResult::Terminate) {
                break;
            }
            if (volumeResult == _VolumeTransmissionResult::ContinueRay) {
                ++bounce;
                continue;
            }
        }

        // A closer finite emitter terminates the segment. Indirect hits
        // enforce linking and MIS-weight BSDF versus explicit light sampling.
        if (hasFiniteLightHit && finiteLightDist < surfaceDist) {
            if (!path.isFirstBounce && !finiteLightLink.IsEmpty() &&
                (!path.lastScatterCategories ||
                 !HdEmbreeMatchesLink(
                     finiteLightLink, *path.lastScatterCategories))) {
                break;
            }
            GfVec3f lightContrib = finiteLightHit.Li;
            if (path.lastBsdfPdf > 0.0f && finiteLightHit.invPdfW > 0.0f) {
                const float lightPdf = 1.0f / finiteLightHit.invPdfW;
                const float effectiveLightPdf = _GetMultiSampleMisLightPdf(
                    lightPdf,
                    _lightSamplesPerHit);
                if (effectiveLightPdf > 0.0f) {
                    lightContrib *= mxcpp::Bsdf::PowerHeuristic(
                        path.lastBsdfPdf,
                        effectiveLightPdf);
                }
            }

            _AddPathRadiance(
                _WeightPathRadiance(lightContrib, path), &path);
            break;
        }

        // A miss is terminal: camera rays resolve background policy, while
        // indirect rays evaluate linked distant and dome emitters with MIS.
        if (rayHit.hit.geomID == RTC_INVALID_GEOMETRY_ID) {
            _AccumulateEnvironment(&path);
            break;
        }

        // The post-budget iteration may collect only an emitter; an
        // ordinary surface cannot start another shading event.
        if (emitterOnlyBounce) {
            break;
        }

        // -----------------------------------------------------------------
        // Ordinary surface: recover instance/prototype data, construct
        // shading geometry, evaluate material, add radiance, and continue.
        // -----------------------------------------------------------------
        const GfVec3f wo = -path.rayDir;
        _SurfaceInteraction interaction;
        HdEmbreeInstanceContext const* instanceContext = nullptr;
        HdEmbreePrototypeContext const* prototypeContext = nullptr;
        if (!_TryBuildSurfaceInteraction(
                rayHit,
                wo,
                &interaction,
                &instanceContext,
                &prototypeContext)) {
            break;
        }

        // Ng remains outward and immutable. Material evaluation uses the
        // separate incident-facing base normal on both mesh sides.
        const GfVec3f hitPos = interaction.p;
        const GfVec3f Ng = interaction.Ng;
        const GfVec3f incidentNg =
            interaction.GetIncidentGeometricNormal();
        GfVec3f normal = interaction.GetIncidentBaseNormal();
        const GfVec3f differentialNormal = normal;
        HdEmbreeDisplacedSubdivFrame& displacedFrame =
            interaction.displacedFrame;

        // Build material inputs: interpolated primvars, texture derivatives,
        // tangent frame, and normal derivatives needed after sampling.
        mxcpp::ShadingContext ctx = _BuildShadingContext(
            rayHit, path.rayDiff,
            instanceContext, prototypeContext, interaction,
            &surfaceDifferentials.dndu, &surfaceDifferentials.dndv);
        HdEmbreePrimvarLookup cbData{
            &prototypeContext->primvarMapByString,
            rayHit.hit.primID, rayHit.hit.u, rayHit.hit.v};
        ctx.geomPropLookup = &HdEmbreeSamplePrimvar;
        ctx.geomPropUserData = &cbData;
        ctx.uniformProps = &prototypeContext->uniformPrimvarMap;

        // Preserve differential geometry beyond the temporary context so a
        // specular continuation can propagate the ray footprint.
        surfaceDifferentials.dpdx = _ToGf(ctx.dPdx);
        surfaceDifferentials.dpdy = _ToGf(ctx.dPdy);
        surfaceDifferentials.dudx = ctx.dudx;
        surfaceDifferentials.dvdx = ctx.dvdx;
        surfaceDifferentials.dudy = ctx.dudy;
        surfaceDifferentials.dvdy = ctx.dvdy;
        surfaceDifferentials.baseNormalStatus = prototypeContext->displaced
            ? _BaseNormalDerivativeStatus::Deferred
            : _BaseNormalDerivativeStatus::Ready;
        surfaceDifferentials.resolvedNormalProvenance =
            prototypeContext->displaced
                ? _ResolvedNormalDerivativeProvenance::None
                : _ResolvedNormalDerivativeProvenance::BaseApproximation;

        GfVec3f tangent = _ToGf(ctx.tangent);
        GfVec3f bitangent = _ToGf(ctx.bitangent);

        // -----------------------------------------------------------------
        // Evaluate the graph into one closure. Failure falls through to the
        // display-color fallback used by direct lighting.
        // -----------------------------------------------------------------
        mxcpp::EvalGraph* surfaceGraph = prototypeContext->material
            ? prototypeContext->material->surfaceGraph
            : nullptr;

        mxcpp::SurfaceClosure closure;
        bool hasClosure = false;

        if (surfaceGraph && !path.useSyntheticLambertian) {
            mxcpp::EvalOptions evalOptions;
            evalOptions.useAdobeOpenPBR = _useAdobeOpenPBR;
            closure = surfaceGraph->Evaluate(ctx, evalOptions);
            hasClosure = true;
        }

        // SSS exit synthesis: at the exit-side surface hit immediately
        // following HdEmbreeRandomWalkSSS, replace the evaluated closure
        // with a weight-1.0 Lambertian so the random-walk's albedo isn't
        // double-counted. Cycles uses the same trick.
        if (path.useSyntheticLambertian) {
            const bool hitOwner =
                (rayHit.hit.instID[0] ==
                 path.syntheticLambertianExit.exitInstanceId) &&
                (rayHit.hit.geomID == path.syntheticLambertianExit.exitGeomId);
            if (hitOwner) {
                closure = mxcpp::SurfaceClosure{};
                closure.baseColor = mxcpp::Vec3f(1.0f);
                closure.roughness = 1.0f;
                // SurfaceClosure defaults include a dielectric specular
                // lobe. Clear it so the SSS exit matches Cycles'
                // DiffuseBsdf replacement instead of adding Fresnel loss.
                closure.specular = 0.0f;
                closure.specularColor = mxcpp::Vec3f(0.0f);
                closure.opacity = 1.0f;
                closure.presence = 1.0f;
                hasClosure = true;
            }
            // Always clear flag; even on mismatch we don't want it to linger.
            path.useSyntheticLambertian = false;
            path.syntheticLambertianExit = HdEmbreeSssOutput{};
        }

        // Resolve every material normal in the incident frame. Invalid,
        // degenerate, or wrong-geometric-hemisphere values deterministically
        // fall back to the incident base normal; they are never negated.
        bool resolvedNormalUsesBase = true;
        mxcpp::Vec3f resolvedNormal;
        if (hasClosure &&
            closure.ResolveNormal(
                _ToMx(tangent),
                _ToMx(bitangent),
                _ToMx(normal),
                &resolvedNormal)) {
            GfVec3f candidate;
            const bool valid =
                _TryNormalizeDirection(_ToGf(resolvedNormal), &candidate) &&
                GfDot(candidate, incidentNg) > 0.0f &&
                GfDot(candidate, wo) > 0.0f;
            if (valid) {
                resolvedNormalUsesBase = GfIsClose(candidate, normal, 1e-6f);
                normal = candidate;
                if (!resolvedNormalUsesBase) {
                    surfaceDifferentials.resolvedNormalProvenance =
                        _ResolvedNormalDerivativeProvenance::None;
                }
            } else {
                ++_invalidMaterialNormalCount;
            }
        }

        // Resolve presence stochastically. A rejected interaction advances
        // unchanged and refunds the bounce because it is a null event.
        if (hasClosure && closure.presence < 1.0f) {
            const auto advancePastHit = [&]() {
                const float advance = rayHit.ray.tfar + 1e-4f;
                path.rayOrigin = hitPos + path.rayDir * 1e-4f;
                if (path.rayDiff.hasDifferentials) {
                    path.rayDiff.rxOrigin += path.rayDir * advance;
                    path.rayDiff.ryOrigin += path.rayDir * advance;
                }
                // Null presence pass-through is not a scattering event. Keep
                // MIS / first-bounce state from the previous real interaction.
            };

            const float presence = _Clamp01(closure.presence);
            if (presence <= 0.0f) {
                advancePastHit();
                continue;
            }
            // Treat presence as the probability of interaction. The strict
            // u < presence test keeps endpoint-zero sampler values from
            // interacting when the surface is fully absent.
            if (bounceDomain
                    .Fork(HdEmbreeSampleDomainKey::Presence)
                    .Draw1D() >= presence) {
                advancePastHit();
                continue;
            }
            // We chose to interact; clear the stochastic presence term so
            // EvalSurface / SampleSurface don't attenuate a second time.
            closure.presence = 1.0f;
        }

        // Classify transport: medium-only boundaries bypass BSDF work, and
        // disabled caustics prune disallowed lobes before sampling.
        mxcpp::SurfaceClosure causticPrunedClosure;
        const bool volumeOnlyBoundary =
            hasClosure && _IsVolumeOnlyBoundary(closure);
        const mxcpp::SurfaceClosure* bsdfClosure =
            hasClosure ? &closure : nullptr;
        bool hasBsdfClosure = hasClosure && !volumeOnlyBoundary;
        if (hasClosure && path.hasDiffuseLikeAncestor && !_enableCaustics) {
            causticPrunedClosure =
                mxcpp::Bsdf::PruneCausticClassLobes(closure);
            // A tree-based material can prune down to no remaining BSDF
            // lobes. Keep the material closure for emission/opacity state,
            // but do not fall back to the legacy summary BSDF in that case.
            if (closure.HasBsdfTree() && !causticPrunedClosure.HasBsdfTree()) {
                bsdfClosure = nullptr;
                hasBsdfClosure = false;
            } else {
                bsdfClosure = &causticPrunedClosure;
            }
        }

        // Enter hero-wavelength mode at the first dispersive closure and
        // retain that wavelength/PDF for every later segment.
        if (hasBsdfClosure && bsdfClosure->HasDispersion() && !path.hero.active) {
            path.hero.active = true;
            path.hero.wavelengthNm =
                mxcpp::Spectral::SampleHeroWavelength(
                    bounceDomain
                        .Fork(HdEmbreeSampleDomainKey::Wavelength)
                        .Draw1D());
            path.hero.pdf = mxcpp::Spectral::HeroWavelengthPdf();
            path.spectralThroughput = _RgbToSpectralValue(
                path.throughput, path.hero, _renderColorSpace);
        }

        // Every lobe consumes the same incident-facing shading normal.
        // Interface side and transport classification remain separate state.
        const GfVec3f bsdfNormal = normal;

        mxcpp::AdobeOpenPbrPreparedSurface adobeOpenPbrSurface;
        if (hasBsdfClosure) {
            adobeOpenPbrSurface = mxcpp::PrepareAdobeOpenPbrSurface(
                *bsdfClosure,
                _ToMx(interaction.frontFacing
                    ? bsdfNormal
                    : -bsdfNormal),
                _ToMx(wo));
        }

        // Sample once for possible continuation, independently of direct
        // lighting and subsurface random draws.
        mxcpp::Bsdf::BsdfSample bs;
        bool hasBsdfSample = false;
        if (hasBsdfClosure) {
            const GfVec3f bsdfSample =
                bounceDomain
                    .Fork(HdEmbreeSampleDomainKey::BsdfSample)
                    .Draw3D();
            if (adobeOpenPbrSurface.valid) {
                bs = mxcpp::SamplePreparedAdobeOpenPbrSurface(
                    adobeOpenPbrSurface,
                    bsdfSample[0],
                    bsdfSample[1],
                    bsdfSample[2]);
            } else {
                bs = mxcpp::Bsdf::SampleSurface(
                    *bsdfClosure, _ToMx(bsdfNormal), _ToMx(wo),
                    bsdfSample[0], bsdfSample[1], bsdfSample[2],
                    path.hero.wavelengthNm,
                    interaction.frontFacing);
            }
            hasBsdfSample = bs.isSubsurface || bs.pdf > 0.0f;
        }

        // A selected SSS event delegates entry validation, random-walk
        // transport, throughput updates, statistics, and exit scheduling.
        if (hasBsdfClosure &&
            hasBsdfSample &&
            bounce < maxBounces &&
            bs.isSubsurface &&
            bsdfClosure->HasSubsurfaceScattering()) {
            _SubsurfaceInput subsurfaceInput;
            subsurfaceInput.rayHit = &rayHit;
            subsurfaceInput.instanceContext = instanceContext;
            subsurfaceInput.closure = bsdfClosure;
            subsurfaceInput.hitPos = hitPos;
            subsurfaceInput.normal = normal;
            subsurfaceInput.faceNormal = incidentNg;
            subsurfaceInput.wo = wo;
            subsurfaceInput.sampledEntryDirection = _ToGf(bs.wi);
            subsurfaceInput.entryWeight = _ToGf(bs.f);
            subsurfaceInput.hasSampledEntryDirection =
                bs.hasSubsurfaceEntryDirection;

            if (_TraceSubsurface(
                    subsurfaceInput, bounceDomain, &path) ==
                _SubsurfaceResult::Terminate) {
                break;
            }

            // The synthetic exit is part of this same surface event, so it
            // continues without advancing the scattering depth.
            continue;
        }

        // Add surface emission under incoming throughput, independently of
        // whether the closure can continue the path.
        if (hasClosure) {
            _AddPathRadiance(
                _WeightPathRadiance(_ToGf(closure.emissiveColor), path),
                &path);
        }

        // Estimate direct light; NEE handles linking, visibility, medium
        // transmittance, and MIS against BSDF sampling.
        GfVec3f direct(0.0f);
        if (hasBsdfClosure) {
            direct = _ComputeDirectLightingMIS(
                hitPos,
                bsdfNormal,
                Ng,
                wo,
                bounceDomain.Fork(HdEmbreeSampleDomainKey::DirectLighting),
                interaction.frontFacing,
                true,
                bsdfClosure,
                instanceContext->categories,
                path.medium,
                path.hero.active,
                path.hero.wavelengthNm,
                path.hero.pdf,
                adobeOpenPbrSurface.valid ? &adobeOpenPbrSurface : nullptr);
        } else if (!hasClosure) {
            // Missing/failed materials use diffuse display color so
            // unmaterialized geometry remains visible.
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
                Ng,
                wo,
                bounceDomain.Fork(HdEmbreeSampleDomainKey::DirectLighting),
                interaction.frontFacing,
                false,
                &fallback,
                instanceContext->categories,
                path.medium,
                path.hero.active,
                path.hero.wavelengthNm,
                path.hero.pdf);
        }
        if (path.hero.active) {
            _AddPathRadiance(direct * path.spectralThroughput, &path);
        } else {
            _AddPathRadiance(GfCompMult(path.throughput, direct), &path);
        }

        // Cross a medium-only boundary without scattering: update medium
        // ownership, advance through the surface, and refund the bounce.
        if (volumeOnlyBoundary) {
            const float wiDotNg = GfDot(path.rayDir, Ng);
            _UpdatePathMedium(
                closure,
                prototypeContext,
                instanceContext->categories,
                wiDotNg,
                &path);

            const float advance = rayHit.ray.tfar + 1e-4f;
            const float bias = wiDotNg > 0.0f ? 1e-4f : -1e-4f;
            path.rayOrigin = hitPos + Ng * bias;
            if (path.rayDiff.hasDifferentials) {
                path.rayDiff.rxOrigin += path.rayDir * advance;
                path.rayDiff.ryOrigin += path.rayDir * advance;
            }

            // Medium-only boundaries are not scattering events.
            continue;
        }

        // Enforce the bounce budget. A valid final sample may launch one
        // emitter-only segment so terminal emission is not discarded.
        const bool traceEmitterOnlySample =
            bounce >= maxBounces &&
            hasBsdfClosure &&
            hasBsdfSample &&
            !bs.isSubsurface;
        if (bounce >= maxBounces && !traceEmitterOnlySample) {
            break;
        }

        // Continuation requires a valid non-SSS sample; SSS already
        // scheduled its own exit above.
        if (!hasBsdfClosure || !hasBsdfSample || bs.isSubsurface) break;

        const GfVec3f wi = _ToGf(bs.wi);
        const float woDotNg = GfDot(wo, Ng);
        const float wiDotNg = GfDot(wi, Ng);
        const bool crossesBoundary =
            (woDotNg > 0.0f && wiDotNg < 0.0f) ||
            (woDotNg < 0.0f && wiDotNg > 0.0f);
        const bool shadingReflection =
            GfDot(wo, bsdfNormal) * GfDot(wi, bsdfNormal) > 0.0f;
        const bool geometricReflection = woDotNg * wiDotNg > 0.0f;
        if (shadingReflection != geometricReflection) {
            break;
        }

        // Preserve the sampled interface as an explicit absolute IOR pair.
        // The current single-owner model has air outside; future medium
        // tracking can replace this resolver without changing propagation.
        float etaIncident = 1.0f;
        float etaTransmitted = 1.0f;
        if (crossesBoundary && bs.eta > 0.0f && bs.eta != 1.0f) {
            if (interaction.frontFacing) {
                etaTransmitted = 1.0f / bs.eta;
            } else {
                etaIncident = bs.eta;
            }
        }
        // Classify caustics from ancestry plus this event. Boundary
        // crossings count alongside explicitly specular samples.
        const bool sampledCausticEvent =
            path.hasDiffuseLikeAncestor && (bs.isSpecular || crossesBoundary);
        // Closure pruning above should keep these events out of the sampling
        // distribution when caustics are disabled. Keep this guard for cases
        // that are only visible after sampling, such as normal-map boundary
        // changes, backend-specific lobe labels, or future medium variants.
        if (sampledCausticEvent && !_enableCaustics) {
            break;
        }
        path.currentPathIsCaustic =
            path.currentPathIsCaustic || sampledCausticEvent;

        // Apply the continuation estimator: delta samples contain their
        // coefficient; finite-PDF samples require cosine divided by PDF.
        if (path.hero.active) {
            float bsdfContrib = 0.0f;
            if (bs.isSpecular) {
                bsdfContrib = mxcpp::Spectral::RgbToSpectralValue(
                    bs.f,
                    path.hero.wavelengthNm,
                    _renderColorSpace ==
                            HdEmbreeRenderColorSpace::LinearAP1
                        ? mxcpp::Spectral::RgbColorSpace::LinearAP1
                        : mxcpp::Spectral::RgbColorSpace::LinearRec709);
            } else {
                const float cosTheta =
                    std::abs(GfDot(bsdfNormal, _ToGf(bs.wi)));
                bsdfContrib = mxcpp::Spectral::RgbToSpectralValue(
                    bs.f,
                    path.hero.wavelengthNm,
                    _renderColorSpace ==
                            HdEmbreeRenderColorSpace::LinearAP1
                        ? mxcpp::Spectral::RgbColorSpace::LinearAP1
                        : mxcpp::Spectral::RgbColorSpace::LinearRec709) *
                    cosTheta / bs.pdf;
            }

            if (!std::isfinite(bsdfContrib) || bsdfContrib < 0.0f) {
                bsdfContrib = 0.0f;
            }
            path.spectralThroughput *= bsdfContrib;
        } else {
            GfVec3f bsdfContrib;
            if (bs.isSpecular) {
                // Delta distribution (e.g. thin-surface transmission):
                // f already contains the throughput coefficient; no cosine
                // or pdf division needed.
                bsdfContrib = _ToGf(bs.f);
            } else {
                float cosTheta = std::abs(GfDot(bsdfNormal, _ToGf(bs.wi)));
                bsdfContrib = _ToGf(bs.f) * cosTheta / bs.pdf;
            }

            for (int i = 0; i < 3; ++i) {
                if (!std::isfinite(bsdfContrib[i]) || bsdfContrib[i] < 0.0f)
                    bsdfContrib[i] = 0.0f;
            }

            path.throughput = GfCompMult(path.throughput, bsdfContrib);
        }

        // Publish this scatter for next-segment MIS, linking, environment
        // sampling, and caustic ancestry.
        path.lastBsdfPdf = bs.isSpecular ? 0.0f : bs.pdf;
        path.lastScatterWasMedium = false;
        path.lastScatterCategories = &instanceContext->categories;
        path.lastLightSamplingMode =
            (!bs.isSpecular && _IsReflectionOnlyClosure(closure))
                ? HdEmbreeLightSampler::SamplingMode::ReflectionHemisphere
                : HdEmbreeLightSampler::SamplingMode::FullSphere;
        path.lastLightSamplingNormal = bsdfNormal;
        if (bs.isDiffuseLike) {
            path.hasDiffuseLikeAncestor = true;
        }
        path.isFirstBounce = false;
        // Transmission enters or leaves the interior medium; only one
        // active owner is currently modeled.
        if (crossesBoundary && hasClosure && !closure.thinWalled &&
            bs.eta > 0.0f && bs.eta != 1.0f) {
            _UpdatePathMedium(
                closure,
                prototypeContext,
                instanceContext->categories,
                wiDotNg,
                &path);
        }

        // Russian roulette ends low-throughput paths without bias; survivors
        // divide by their probability. Terminal emitter rays skip it.
        if (!traceEmitterOnlySample && bounce >= _minBouncesBeforeRR) {
            float q = path.hero.active
                ? std::max({
                    _SpectralScalarToRgb(
                        path.spectralThroughput,
                        path.hero,
                        _renderColorSpace)[0],
                    _SpectralScalarToRgb(
                        path.spectralThroughput,
                        path.hero,
                        _renderColorSpace)[1],
                    _SpectralScalarToRgb(
                        path.spectralThroughput,
                        path.hero,
                        _renderColorSpace)[2]})
                : std::max({path.throughput[0], path.throughput[1], path.throughput[2]});
            q = std::min(q, 0.95f);
            if (q <= 0.0f ||
                bounceDomain
                    .Fork(HdEmbreeSampleDomainKey::RussianRoulette)
                    .Draw1D() > q) {
                break;
            }
            if (path.hero.active) {
                path.spectralThroughput /= q;
            } else {
                path.throughput /= q;
            }
        }

        // Displaced normal curvature is only needed when a deterministic
        // delta continuation will carry an active camera footprint. Complete
        // the cached C/U/V frame here, after all path-termination decisions,
        // so other displaced hits retain the existing three graph samples.
        if (prototypeContext->displaced &&
            displacedFrame.valid &&
            path.rayDiff.hasDifferentials &&
            bs.isSpecular &&
            !traceEmitterOnlySample) {
            GfVec3f displacedDndu;
            GfVec3f displacedDndv;
            if (_TryComputeDisplacedSubdivNormalDerivativesToWorld(
                    prototypeContext,
                    instanceContext,
                    instanceContext->rootScene,
                    rayHit.hit.geomID,
                    displacedFrame,
                    differentialNormal,
                    &displacedDndu,
                    &displacedDndv)) {
                surfaceDifferentials.dndu = displacedDndu;
                surfaceDifferentials.dndv = displacedDndv;
                surfaceDifferentials.baseNormalStatus =
                    _BaseNormalDerivativeStatus::Ready;
                surfaceDifferentials.resolvedNormalProvenance =
                    resolvedNormalUsesBase
                        ? _ResolvedNormalDerivativeProvenance::BaseApproximation
                        : _ResolvedNormalDerivativeProvenance::None;
            } else {
                surfaceDifferentials.baseNormalStatus =
                    _BaseNormalDerivativeStatus::Failed;
                surfaceDifferentials.resolvedNormalProvenance =
                    _ResolvedNormalDerivativeProvenance::None;
            }
        }

        // Propagate the camera footprint through delta events; diffuse or
        // glossy scattering invalidates the deterministic differential map.
        _PropagateRayDifferential(
            surfaceDifferentials,
            hitPos,
            bsdfNormal,
            wo,
            wi,
            etaIncident / etaTransmitted,
            bs.isSpecular,
            &path.rayDiff);

        // Bias onto the sampled side of the geometric surface, then publish
        // the sampled direction as the next pending segment.
        float bias = (GfDot(wi, Ng) > 0.0f) ? 1e-4f : -1e-4f;
        path.rayOrigin = hitPos + Ng * bias;
        path.rayDir = wi;
        ++bounce;
    }

    // Pack finite, non-negative display radiance; alpha remains opaque.
    result.color = GfVec4f(
        std::max(0.0f, path.radiance[0]),
        std::max(0.0f, path.radiance[1]),
        std::max(0.0f, path.radiance[2]),
        1.0f);
    return result;
}

PXR_NAMESPACE_CLOSE_SCOPE
