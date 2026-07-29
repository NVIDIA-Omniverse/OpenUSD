//
// Copyright 2018 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
// Main multi-bounce path integration loop.

#include "closureClassification.h"
#include "transportPolicy.h"

#include <renderer/geometry/primvarSampling.h>
#include <renderer/geometry/surfaceDerivatives.h>
#include <renderer/heroWavelength.h>
#include <renderer/materials/MaterialXCpp/graph.h>
#include <renderer/materials/MaterialXCpp/materials/adobeOpenPbr.h>
#include <renderer/materials/MaterialXCpp/materials/bsdf.h>
#include <renderer/materials/MaterialXCpp/shadingContext.h>
#include <renderer/rayUtil.h>
#include <renderer/renderBuffer.h>
#include <renderer/renderer.h>
#include <renderer/rendererMath.h>

#include "pxr/base/work/loops.h"
#include "pxr/base/work/threadLimits.h"
#include "pxr/imaging/hd/perfLog.h"

#include <chrono>
#include <cstdio>
#include <thread>

PXR_NAMESPACE_OPEN_SCOPE

static bool
_PopulateSssExitRayHit(
    RTCRayHit* rayHit,
    HdEmbreeSssOutput const& sssOut)
{
    if (sssOut.exitInstanceId == RTC_INVALID_GEOMETRY_ID ||
        sssOut.exitGeomId == RTC_INVALID_GEOMETRY_ID ||
        sssOut.exitPrimId == RTC_INVALID_GEOMETRY_ID) {
        return false;
    }

    GfVec3f rayDir = -sssOut.directionExitWld;
    if (rayDir.GetLengthSq() <= 1.0e-20f) {
        return false;
    }
    rayDir.Normalize();

    ty::PopulateRayHit(rayHit, sssOut.positionExitWld, rayDir, 0.0f, 0.0f,
                       HdEmbree_RayMask::Camera);

    rayHit->hit.primID = sssOut.exitPrimId;
    rayHit->hit.geomID = sssOut.exitGeomId;
    rayHit->hit.instID[0] = sssOut.exitInstanceId;
    rayHit->hit.u = sssOut.coordinateParametricExitU;
    rayHit->hit.v = sssOut.coordinateParametricExitV;
    rayHit->hit.Ng_x = sssOut.normalGeomExitObjExt[0];
    rayHit->hit.Ng_y = sssOut.normalGeomExitObjExt[1];
    rayHit->hit.Ng_z = sssOut.normalGeomExitObjExt[2];
    return true;
}

GfVec3f
HdEmbreeRenderer::_WeightPathRadiance(GfVec3f const& radiance,
                                      _PathState const& state) const
{
    if (!state.hero.active) {
        return GfCompMult(state.throughputRgb, radiance);
    }
    const _HeroWavelengthState hero{
        true, state.hero.wavelengthNm, state.hero.pdf};
    return ty::SpectralValueToRgb(
        state.throughputSpectral *
            ty::RgbToSpectralValue(radiance, hero, _renderColorSpace),
        hero, _renderColorSpace);
}

void
HdEmbreeRenderer::_AddPathRadiance(GfVec3f radianceContribution,
                                   _PathState* state) const
{
    if (!state) {
        return;
    }
    if (state->currentPathIsCaustic) {
        radianceContribution = ty::ClampFireflyContribution(
            radianceContribution, _settings.causticsClampThreshold,
            _materialEvalServices.luminanceCoefficients);
    }
    state->radianceAccumulated +=
        ty::ClampFireflyContribution(
            radianceContribution, _settings.fireflyClampThreshold,
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
    path.positionRayOriginWld = origin;
    path.directionRayWld = dir;
    path.rayDifferential = rayDiff;

    // Retain surface derivatives until the BSDF sample is known. Specular
    // continuations use them to propagate the camera-ray footprint.
    _SurfaceDifferentials surfaceDifferentials;

    const int maxBounces = std::max(0, _settings.maxBounces);

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
            if (!_PopulateSssExitRayHit(
                    &rayHit, path.syntheticLambertianExit)) {
                break;
            }
        } else {
            ty::PopulateRayHit(
                &rayHit,
                path.positionRayOriginWld,
                path.directionRayWld,
                path.isFirstBounce ? 0.0f : 1e-4f,
                std::numeric_limits<float>::max(),
                emitterOnlyBounce ? HdEmbree_RayMask::Light
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
            _EvaluateLightGeometryHit(rayHit, path.positionRayOriginWld,
                                      path.directionRayWld, &finiteLightHit,
                                      &finiteLightLink);
        const float surfaceDist =
            rayHit.hit.geomID != RTC_INVALID_GEOMETRY_ID && !hitLightGeometry
                ? rayHit.ray.tfar
                : std::numeric_limits<float>::infinity();
        const bool hasAnalyticFiniteLightHit =
            !syntheticLambertianHit && !hitLightGeometry &&
            !path.isFirstBounce &&
            _FindNearestFiniteLightHit(path.positionRayOriginWld,
                                       path.directionRayWld, surfaceDist,
                                       &finiteLightHit, &finiteLightLink);
        const bool hasFiniteLightHit =
            hitLightGeometry || hasAnalyticFiniteLightHit;
        const float finiteLightDist =
            hasFiniteLightHit ? finiteLightHit.distanceWld
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
            GfVec3f radianceLight = finiteLightHit.radianceIn;
            if (path.lastBsdfPdf > 0.0f &&
                finiteLightHit.pdfSolidAngleInverse > 0.0f) {
                const float lightPdf =
                    1.0f / finiteLightHit.pdfSolidAngleInverse;
                const float effectiveLightPdf = ty::GetMultiSampleMisLightPdf(
                    lightPdf,
                    _settings.lightSamplesPerHit);
                if (effectiveLightPdf > 0.0f) {
                    radianceLight *= mxcpp::Bsdf::PowerHeuristic(
                        path.lastBsdfPdf, effectiveLightPdf);
                }
            }

            _AddPathRadiance(_WeightPathRadiance(radianceLight, path), &path);
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
        const GfVec3f omegaOutWld = -path.directionRayWld;
        _SurfaceInteraction interaction;
        HdEmbreeInstanceContext const* instanceContext = nullptr;
        HdEmbreePrototypeContext const* prototypeContext = nullptr;
        if (!_TryBuildSurfaceInteraction(rayHit, omegaOutWld, &interaction,
                                         &instanceContext, &prototypeContext)) {
            break;
        }

        // Keep the authored exterior geometric normal immutable. Material
        // evaluation uses the separate exitant-facing surface normal.
        const GfVec3f positionHitWld = interaction.positionHitWld;
        const GfVec3f normalGeomWldExt = interaction.normalGeomWldExt;
        const GfVec3f normalGeomWldOut = interaction.GetNormalGeomWldOut();
        const GfVec3f normalSrfWldOut = interaction.GetNormalSrfWldOut();
        GfVec3f normalShdWldOut = normalSrfWldOut;
        HdEmbreeDisplacedSubdivFrame& displacedFrame =
            interaction.displacedFrame;

        // Build material inputs: interpolated primvars, texture derivatives,
        // tangent frame, and normal derivatives needed after sampling.
        mxcpp::ShadingContext ctx = _BuildShadingContext(
            rayHit, path.rayDifferential, instanceContext, prototypeContext,
            interaction, &surfaceDifferentials.dndu,
            &surfaceDifferentials.dndv);
        HdEmbreePrimvarLookup cbData{
            &prototypeContext->geomPropSamplers,
            rayHit.hit.primID, rayHit.hit.u, rayHit.hit.v};
        ctx.geomPropLookup = &HdEmbreeSamplePrimvar;
        ctx.geomPropUserData = &cbData;
        ctx.uniformProps = &prototypeContext->geomPropUniformValues;

        // Preserve differential geometry beyond the temporary context so a
        // specular continuation can propagate the ray footprint.
        surfaceDifferentials.dpdx = ty::ToGf(ctx.dPdx);
        surfaceDifferentials.dpdy = ty::ToGf(ctx.dPdy);
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

        GfVec3f tangent = ty::ToGf(ctx.tangent);
        GfVec3f bitangent = ty::ToGf(ctx.bitangent);

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
            evalOptions.useAdobeOpenPBR = _settings.useAdobeOpenPBR;
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

        // Resolve every material normal in the exitant frame. Invalid,
        // degenerate, or wrong-geometric-hemisphere values deterministically
        // fall back to the exitant-facing surface normal; they are never
        // negated.
        bool resolvedNormalUsesBase = true;
        mxcpp::Vec3f resolvedNormal;
        if (hasClosure &&
            closure.ResolveNormal(ty::ToMx(tangent), ty::ToMx(bitangent),
                                  ty::ToMx(normalShdWldOut), &resolvedNormal)) {
            GfVec3f candidate;
            const bool valid =
                ty::TryNormalizeDirection(
                    ty::ToGf(resolvedNormal), &candidate) &&
                GfDot(candidate, normalGeomWldOut) > 0.0f &&
                GfDot(candidate, omegaOutWld) > 0.0f;
            if (valid) {
                resolvedNormalUsesBase =
                    GfIsClose(candidate, normalShdWldOut, 1e-6f);
                normalShdWldOut = candidate;
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
                path.positionRayOriginWld =
                    positionHitWld + path.directionRayWld * 1e-4f;
                if (path.rayDifferential.hasDifferentials) {
                    path.rayDifferential.rxOrigin +=
                        path.directionRayWld * advance;
                    path.rayDifferential.ryOrigin +=
                        path.directionRayWld * advance;
                }
                // Null presence pass-through is not a scattering event. Keep
                // MIS / first-bounce state from the previous real interaction.
            };

            const float presence = ty::Clamp01(closure.presence);
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
            hasClosure && ty::IsVolumeOnlyBoundary(closure);
        const mxcpp::SurfaceClosure* bsdfClosure =
            hasClosure ? &closure : nullptr;
        bool hasBsdfClosure = hasClosure && !volumeOnlyBoundary;
        if (hasClosure &&
            path.hasDiffuseLikeAncestor &&
            !_settings.enableCaustics) {
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
        if (hasBsdfClosure &&
            bsdfClosure->HasDispersion() &&
            !path.hero.active) {
            path.hero.active = true;
            path.hero.wavelengthNm =
                mxcpp::Spectral::SampleHeroWavelength(
                    bounceDomain
                        .Fork(HdEmbreeSampleDomainKey::Wavelength)
                        .Draw1D());
            path.hero.pdf = mxcpp::Spectral::HeroWavelengthPdf();
            path.throughputSpectral =
                ty::RgbToSpectralValue(
                    path.throughputRgb, path.hero, _renderColorSpace);
        }

        // Every lobe consumes the same exitant-facing shading normal.
        // Interface side and transport classification remain separate state.
        mxcpp::AdobeOpenPbrPreparedSurface adobeOpenPbrSurface;
        if (hasBsdfClosure) {
            adobeOpenPbrSurface = mxcpp::PrepareAdobeOpenPbrSurface(
                *bsdfClosure,
                ty::ToMx(interaction.frontFacing ? normalShdWldOut
                                                  : -normalShdWldOut),
                ty::ToMx(omegaOutWld));
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
                    *bsdfClosure,
                    ty::ToMx(normalShdWldOut),
                    ty::ToMx(omegaOutWld),
                    bsdfSample[0],
                    bsdfSample[1],
                    bsdfSample[2],
                    path.hero.wavelengthNm,
                    interaction.frontFacing);
            }
            hasBsdfSample = bs.isSubsurface || bs.pdfSolidAngle > 0.0f;
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
            subsurfaceInput.positionHitWld = positionHitWld;
            subsurfaceInput.normalShdWldOut = normalShdWldOut;
            subsurfaceInput.normalGeomWldOut = normalGeomWldOut;
            subsurfaceInput.omegaOutWld = omegaOutWld;
            subsurfaceInput.directionEntryWld = ty::ToGf(bs.omegaInWld);
            subsurfaceInput.entryWeight = ty::ToGf(bs.bsdfValue);
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
                _WeightPathRadiance(ty::ToGf(closure.emissiveColor), path),
                &path);
        }

        // Estimate direct light; NEE handles linking, visibility, medium
        // transmittance, and MIS against BSDF sampling.
        GfVec3f direct(0.0f);
        if (hasBsdfClosure) {
            direct = _ComputeDirectLightingMIS(
                positionHitWld, normalShdWldOut, normalGeomWldExt, omegaOutWld,
                bounceDomain.Fork(HdEmbreeSampleDomainKey::DirectLighting),
                interaction.frontFacing, true, bsdfClosure,
                instanceContext->categories, path.medium, path.hero.active,
                path.hero.wavelengthNm, path.hero.pdf,
                adobeOpenPbrSurface.valid ? &adobeOpenPbrSurface : nullptr);
        } else if (!hasClosure) {
            // Missing/failed materials use diffuse display color so
            // unmaterialized geometry remains visible.
            GfVec3f matColor = _settings.enableSceneColors
                ? ty::ToGf(ctx.displayColor) : GfVec3f(0.5f);
            mxcpp::SurfaceClosure fallback;
            fallback.baseColor = ty::ToMx(matColor);
            fallback.roughness = 1.0f;
            fallback.metallic = 0.0f;
            fallback.specular = 0.0f;
            fallback.specularColor = mxcpp::Vec3f(1.0f);
            fallback.specularIor = 1.5f;
            fallback.opacity = 1.0f;
            direct = _ComputeDirectLightingMIS(
                positionHitWld, normalShdWldOut, normalGeomWldExt, omegaOutWld,
                bounceDomain.Fork(HdEmbreeSampleDomainKey::DirectLighting),
                interaction.frontFacing, false, &fallback,
                instanceContext->categories, path.medium, path.hero.active,
                path.hero.wavelengthNm, path.hero.pdf);
        }
        if (path.hero.active) {
            _AddPathRadiance(direct * path.throughputSpectral, &path);
        } else {
            _AddPathRadiance(GfCompMult(path.throughputRgb, direct), &path);
        }

        // Cross a medium-only boundary without scattering: update medium
        // ownership, advance through the surface, and refund the bounce.
        if (volumeOnlyBoundary) {
            const float omegaInDotNormalGeom =
                GfDot(path.directionRayWld, normalGeomWldExt);
            _UpdatePathMedium(closure, prototypeContext,
                              instanceContext->categories, omegaInDotNormalGeom,
                              &path);

            const float advance = rayHit.ray.tfar + 1e-4f;
            const float bias = omegaInDotNormalGeom > 0.0f ? 1e-4f : -1e-4f;
            path.positionRayOriginWld =
                positionHitWld + normalGeomWldExt * bias;
            if (path.rayDifferential.hasDifferentials) {
                path.rayDifferential.rxOrigin += path.directionRayWld * advance;
                path.rayDifferential.ryOrigin += path.directionRayWld * advance;
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

        const GfVec3f omegaInWld = ty::ToGf(bs.omegaInWld);
        const float omegaOutDotNormalGeom =
            GfDot(omegaOutWld, normalGeomWldExt);
        const float omegaInDotNormalGeom = GfDot(omegaInWld, normalGeomWldExt);
        const bool crossesBoundary =
            (omegaOutDotNormalGeom > 0.0f && omegaInDotNormalGeom < 0.0f) ||
            (omegaOutDotNormalGeom < 0.0f && omegaInDotNormalGeom > 0.0f);
        const bool shadingReflection = GfDot(omegaOutWld, normalShdWldOut) *
                                           GfDot(omegaInWld, normalShdWldOut) >
                                       0.0f;
        const bool geometricReflection =
            omegaOutDotNormalGeom * omegaInDotNormalGeom > 0.0f;
        if (shadingReflection != geometricReflection) {
            break;
        }

        // Preserve the sampled interface as an explicit absolute IOR pair.
        // The current single-owner model has air outside; future medium
        // tracking can replace this resolver without changing propagation.
        float iorIn = 1.0f;
        float iorOut = 1.0f;
        if (crossesBoundary && bs.eta > 0.0f && bs.eta != 1.0f) {
            if (interaction.frontFacing) {
                iorOut = 1.0f / bs.eta;
            } else {
                iorIn = bs.eta;
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
        if (sampledCausticEvent && !_settings.enableCaustics) {
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
                    bs.bsdfValue,
                    path.hero.wavelengthNm,
                    _renderColorSpace ==
                            HdEmbreeRenderColorSpace::LinearAP1
                        ? mxcpp::Spectral::RgbColorSpace::LinearAP1
                        : mxcpp::Spectral::RgbColorSpace::LinearRec709);
            } else {
                const float cosTheta =
                    std::abs(GfDot(normalShdWldOut, ty::ToGf(bs.omegaInWld)));
                bsdfContrib = mxcpp::Spectral::RgbToSpectralValue(
                    bs.bsdfValue,
                    path.hero.wavelengthNm,
                    _renderColorSpace ==
                            HdEmbreeRenderColorSpace::LinearAP1
                        ? mxcpp::Spectral::RgbColorSpace::LinearAP1
                        : mxcpp::Spectral::RgbColorSpace::LinearRec709) *
                    cosTheta / bs.pdfSolidAngle;
            }

            if (!std::isfinite(bsdfContrib) || bsdfContrib < 0.0f) {
                bsdfContrib = 0.0f;
            }
            path.throughputSpectral *= bsdfContrib;
        } else {
            GfVec3f bsdfContrib;
            if (bs.isSpecular) {
                // Delta distribution (e.g. thin-surface transmission):
                // The sampled BSDF value already contains the throughput
                // coefficient; no cosine or PDF division is needed.
                bsdfContrib = ty::ToGf(bs.bsdfValue);
            } else {
                float cosTheta =
                    std::abs(GfDot(normalShdWldOut, ty::ToGf(bs.omegaInWld)));
                bsdfContrib =
                    ty::ToGf(bs.bsdfValue) * cosTheta / bs.pdfSolidAngle;
            }

            for (int i = 0; i < 3; ++i) {
                if (!std::isfinite(bsdfContrib[i]) || bsdfContrib[i] < 0.0f)
                    bsdfContrib[i] = 0.0f;
            }

            path.throughputRgb = GfCompMult(path.throughputRgb, bsdfContrib);
        }

        // Publish this scatter for next-segment MIS, linking, environment
        // sampling, and caustic ancestry.
        path.lastBsdfPdf = bs.isSpecular ? 0.0f : bs.pdfSolidAngle;
        path.lastScatterWasMedium = false;
        path.lastScatterCategories = &instanceContext->categories;
        path.lastLightSamplingMode =
            (!bs.isSpecular && ty::IsReflectionOnlyClosure(closure))
                ? HdEmbreeLightSampler::SamplingMode::ReflectionHemisphere
                : HdEmbreeLightSampler::SamplingMode::FullSphere;
        path.lastLightSamplingNormal = normalShdWldOut;
        if (bs.isDiffuseLike) {
            path.hasDiffuseLikeAncestor = true;
        }
        path.isFirstBounce = false;
        // Transmission enters or leaves the interior medium; only one
        // active owner is currently modeled.
        if (crossesBoundary && hasClosure && !closure.thinWalled &&
            bs.eta > 0.0f && bs.eta != 1.0f) {
            _UpdatePathMedium(closure, prototypeContext,
                              instanceContext->categories, omegaInDotNormalGeom,
                              &path);
        }

        // Russian roulette ends low-throughput paths without bias; survivors
        // divide by their probability. Terminal emitter rays skip it.
        if (!traceEmitterOnlySample && bounce >= _settings.minBouncesBeforeRR) {
            float q =
                path.hero.active
                    ? std::max({
                        ty::SpectralScalarToRgb(
                            path.throughputSpectral,
                            path.hero,
                            _renderColorSpace)[0],
                        ty::SpectralScalarToRgb(
                            path.throughputSpectral,
                            path.hero,
                            _renderColorSpace)[1],
                        ty::SpectralScalarToRgb(
                            path.throughputSpectral,
                            path.hero,
                            _renderColorSpace)[2]})
                    : std::max({path.throughputRgb[0], path.throughputRgb[1],
                                path.throughputRgb[2]});
            q = std::min(q, 0.95f);
            if (q <= 0.0f ||
                bounceDomain
                    .Fork(HdEmbreeSampleDomainKey::RussianRoulette)
                    .Draw1D() > q) {
                break;
            }
            if (path.hero.active) {
                path.throughputSpectral /= q;
            } else {
                path.throughputRgb /= q;
            }
        }

        // Displaced-normal curvature is only needed when a deterministic
        // delta continuation will carry an active camera footprint. Complete
        // the cached C/U/V frame here, after all path-termination decisions,
        // so other displaced hits retain the existing three graph samples.
        if (prototypeContext->displaced && displacedFrame.valid &&
            path.rayDifferential.hasDifferentials && bs.isSpecular &&
            !traceEmitterOnlySample) {
            GfVec3f displacedDndu;
            GfVec3f displacedDndv;
            if (ty::TryComputeDisplacedSubdivNormalDerivativesToWorld(
                    prototypeContext, instanceContext,
                    instanceContext->rootScene, rayHit.hit.geomID,
                    displacedFrame, normalSrfWldOut, &displacedDndu,
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
        const float eta = iorIn / iorOut;
        _PropagateRayDifferential(surfaceDifferentials, positionHitWld,
                                  normalShdWldOut, omegaOutWld, omegaInWld, eta,
                                  bs.isSpecular, &path.rayDifferential);

        // Bias onto the sampled side of the geometric surface, then publish
        // the sampled direction as the next pending segment.
        float bias =
            (GfDot(omegaInWld, normalGeomWldExt) > 0.0f) ? 1e-4f : -1e-4f;
        path.positionRayOriginWld = positionHitWld + normalGeomWldExt * bias;
        path.directionRayWld = omegaInWld;
        ++bounce;
    }

    // Pack finite, non-negative display radiance; alpha remains opaque.
    result.color = GfVec4f(std::max(0.0f, path.radianceAccumulated[0]),
                           std::max(0.0f, path.radianceAccumulated[1]),
                           std::max(0.0f, path.radianceAccumulated[2]), 1.0f);
    return result;
}

PXR_NAMESPACE_CLOSE_SCOPE
