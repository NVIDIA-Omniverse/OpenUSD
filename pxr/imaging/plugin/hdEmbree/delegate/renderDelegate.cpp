//
// Copyright 2017 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/imaging/plugin/hdEmbree/delegate/renderDelegate.h"

#include "pxr/imaging/plugin/hdEmbree/renderer/config.h"
#include "pxr/imaging/plugin/hdEmbree/delegate/instancer.h"
#include "pxr/imaging/plugin/hdEmbree/delegate/light.h"
#include "pxr/imaging/plugin/hdEmbree/delegate/renderParam.h"
#include "pxr/imaging/plugin/hdEmbree/delegate/renderPass.h"

#include "pxr/imaging/hd/extComputation.h"
#include "pxr/imaging/hd/resourceRegistry.h"
#include "pxr/imaging/hd/tokens.h"

#include "pxr/base/tf/diagnostic.h"

#include "pxr/imaging/plugin/hdEmbree/delegate/mesh.h"
#include "pxr/imaging/plugin/hdEmbree/delegate/material.h"
//XXX: Add other Rprim types later
#include "pxr/imaging/hd/camera.h"
//XXX: Add other Sprim types later
#include "pxr/imaging/hd/bprim.h"
//XXX: Add bprim types

#include <algorithm>
#include <string>

PXR_NAMESPACE_OPEN_SCOPE

TF_DEFINE_PUBLIC_TOKENS(HdEmbreeRenderSettingsTokens, HDEMBREE_RENDER_SETTINGS_TOKENS);

static const TfToken _materialRenderContextMtlxToken(
    "mtlx", TfToken::Immortal);
static const TfToken _materialRenderContextDefaultToken(
    "default", TfToken::Immortal);

static std::string
_GetMaterialRenderContextSetting(const HdRenderDelegate& renderDelegate)
{
    const VtValue value = renderDelegate.GetRenderSetting(
        HdEmbreeRenderSettingsTokens->materialRenderContext);
    if (value.IsHolding<std::string>()) {
        return value.UncheckedGet<std::string>();
    }
    if (value.IsHolding<TfToken>()) {
        return value.UncheckedGet<TfToken>().GetString();
    }
    return HdEmbreeConfig::GetInstance().materialRenderContext;
}

const TfTokenVector HdEmbreeRenderDelegate::SUPPORTED_RPRIM_TYPES =
{
    HdPrimTypeTokens->mesh,
};

const TfTokenVector HdEmbreeRenderDelegate::SUPPORTED_SPRIM_TYPES =
{
    HdPrimTypeTokens->camera,
    HdPrimTypeTokens->extComputation,
    HdPrimTypeTokens->material,
    HdPrimTypeTokens->cylinderLight,
    HdPrimTypeTokens->diskLight,
    HdPrimTypeTokens->distantLight,
    HdPrimTypeTokens->domeLight,
    HdPrimTypeTokens->rectLight,
    HdPrimTypeTokens->sphereLight,
};

const TfTokenVector HdEmbreeRenderDelegate::SUPPORTED_BPRIM_TYPES =
{
    HdPrimTypeTokens->renderBuffer,
};

std::mutex HdEmbreeRenderDelegate::_mutexResourceRegistry;
std::atomic_int HdEmbreeRenderDelegate::_counterResourceRegistry;
HdResourceRegistrySharedPtr HdEmbreeRenderDelegate::_resourceRegistry;

/* static */
void HdEmbreeRenderDelegate::HandleRtcError (void* userPtr, RTCError code, const char* msg)
{
    // Forward RTC error messages through to hydra logging.
    switch (code) {
        case RTC_ERROR_UNKNOWN:
            TF_CODING_ERROR("Embree unknown error: %s", msg);
            break;
        case RTC_ERROR_INVALID_ARGUMENT:
            TF_CODING_ERROR("Embree invalid argument: %s", msg);
            break;
        case RTC_ERROR_INVALID_OPERATION:
            TF_CODING_ERROR("Embree invalid operation: %s", msg);
            break;
        case RTC_ERROR_OUT_OF_MEMORY:
            TF_CODING_ERROR("Embree out of memory: %s", msg);
            break;
        case RTC_ERROR_UNSUPPORTED_CPU:
            TF_CODING_ERROR("Embree unsupported CPU: %s", msg);
            break;
        case RTC_ERROR_CANCELLED:
            TF_CODING_ERROR("Embree cancelled: %s", msg);
            break;
        default:
            TF_CODING_ERROR("Embree invalid error code: %s", msg);
            break;
    }
}

static void _RenderCallback(HdEmbreeRenderer *renderer,
                            HdRenderThread *renderThread)
{
    renderer->ResetAccumulation();
    renderer->Render(renderThread);
}

HdEmbreeRenderDelegate::HdEmbreeRenderDelegate()
    : HdRenderDelegate()
{
    _Initialize();
}

HdEmbreeRenderDelegate::HdEmbreeRenderDelegate(
    HdRenderSettingsMap const& settingsMap)
    : HdRenderDelegate(settingsMap)
{
    _Initialize();
}

void
HdEmbreeRenderDelegate::_Initialize()
{
    // Initialize the settings and settings descriptors.
    const HdEmbreeConfig &config = HdEmbreeConfig::GetInstance();
    _settingDescriptors = {
        { "Enable Scene Colors",
            HdEmbreeRenderSettingsTokens->enableSceneColors,
            VtValue(config.enableSceneColors) },
        { "Enable Ambient Occlusion",
            HdEmbreeRenderSettingsTokens->enableAmbientOcclusion,
            VtValue(config.enableAmbientOcclusion) },
        { "Enable Scene Lighting",
            HdEmbreeRenderSettingsTokens->enableLighting,
            VtValue(config.enableLighting) },
        { "Ambient Occlusion Samples",
            HdEmbreeRenderSettingsTokens->ambientOcclusionSamples,
            VtValue(int(config.ambientOcclusionSamples)) },
        { "Samples To Convergence",
            HdEmbreeRenderSettingsTokens->convergedSamplesPerPixel,
            VtValue(int(config.samplesToConvergence)) },
        { "Random Number Seed",
            HdEmbreeRenderSettingsTokens->randomNumberSeed,
            VtValue(config.randomNumberSeed) },
        { "Sampler Sequence",
            HdEmbreeRenderSettingsTokens->samplerSequence,
            VtValue(config.samplerSequence) },
        { "Dome Light Camera Visibility",
            HdRenderSettingsTokens->domeLightCameraVisibility,
            VtValue(config.domeLightCameraVisibility) },
        { "Enable Exposure Compensation",
            HdEmbreeRenderSettingsTokens->enableExposureCompensation,
            VtValue(true) },
        { "Enable Adaptive Sampling",
            HdEmbreeRenderSettingsTokens->enableAdaptiveSampling,
            VtValue(config.enableAdaptiveSampling) },
        { "Adaptive Threshold",
            HdEmbreeRenderSettingsTokens->adaptiveThreshold,
            VtValue(config.adaptiveThreshold) },
        { "Min Samples Before Adaptive",
            HdEmbreeRenderSettingsTokens->minSamplesBeforeAdaptive,
            VtValue(config.minSamplesBeforeAdaptive) },
        { "Max Bounces",
            HdEmbreeRenderSettingsTokens->maxBounces,
            VtValue(config.maxBounces) },
        { "Min Bounces Before Russian Roulette",
            HdEmbreeRenderSettingsTokens->minBouncesBeforeRR,
            VtValue(config.minBouncesBeforeRR) },
        { "Light Samples Per Hit",
            HdEmbreeRenderSettingsTokens->lightSamplesPerHit,
            VtValue(config.lightSamplesPerHit) },
        { "Stratify Light Samples",
            HdEmbreeRenderSettingsTokens->stratifyLightSamples,
            VtValue(config.stratifyLightSamples) },
        { "Show Adaptive Heatmap",
            HdEmbreeRenderSettingsTokens->showAdaptiveHeatmap,
            VtValue(config.showAdaptiveHeatmap) },
        { "Firefly Clamp Threshold",
            HdEmbreeRenderSettingsTokens->fireflyClampThreshold,
            VtValue(config.fireflyClampThreshold) },
        { "Enable Caustics",
            HdEmbreeRenderSettingsTokens->enableCaustics,
            VtValue(config.enableCaustics) },
        { "Caustics Clamp Threshold",
            HdEmbreeRenderSettingsTokens->causticsClampThreshold,
            VtValue(config.causticsClampThreshold) },
        { "Approximate Transparent Shadows",
            HdEmbreeRenderSettingsTokens->approxTransparentShadows,
            VtValue(config.approxTransparentShadows) },
        { "Disable Shadows",
            HdEmbreeRenderSettingsTokens->disableShadows,
            VtValue(config.disableShadows) },
        { "Enable GGX Microfacet Multiple Scattering",
            HdEmbreeRenderSettingsTokens->enableGgxMicrofacetMultipleScattering,
            VtValue(config.enableGgxMicrofacetMultipleScattering) },
        { "Material Render Context",
            HdEmbreeRenderSettingsTokens->materialRenderContext,
            VtValue(config.materialRenderContext) },
        { "Use Adobe OpenPBR",
            HdEmbreeRenderSettingsTokens->useAdobeOpenPBR,
            VtValue(config.useAdobeOpenPBR) },
        { "Dielectric Layer Throughput Mode",
            HdEmbreeRenderSettingsTokens->dielectricLayerThroughputMode,
            VtValue(config.dielectricLayerThroughputMode) },
        { "Texture Cache Size (MB)",
            HdEmbreeRenderSettingsTokens->textureCacheSize,
            VtValue(int(config.textureCacheSizeMB)) },
    };
    _PopulateDefaultSettings(_settingDescriptors);

    // Initialize the embree library handle (_rtcDevice).
    _rtcDevice = rtcNewDevice(nullptr);

    // Register our error message callback.
    rtcSetDeviceErrorFunction(_rtcDevice,HandleRtcError,NULL);

    // Create the top-level scene.
    //
    // RTC_SCENE_DYNAMIC indicates we'll be updating the scene between draw
    // calls. RTC_INTERSECT1 indicates we'll be casting single rays, and
    // RTC_INTERPOLATE indicates we'll be storing primvars in embree objects
    // and querying them with rtcInterpolate.
    //
    // XXX: Investigate ray packets.
    _rtcScene = rtcNewScene(_rtcDevice);

    // RTC_SCENE_FLAG_DYNAMIC: Provides better build performance for dynamic
    // scenes (but also higher memory consumption).
    rtcSetSceneFlags(_rtcScene, RTC_SCENE_FLAG_DYNAMIC);

    // RTC_BUILD_QUALITY_LOW: Create lower quality data structures,
    // e.g. for dynamic scenes. A two-level spatial index structure is built
    // when enabling this mode, which supports fast partial scene updates,
    // and allows for setting a per-geometry build quality through
    // the rtcSetGeometryBuildQuality function.
    rtcSetSceneBuildQuality(_rtcScene, RTC_BUILD_QUALITY_LOW);

    // std::atomic does not default-initialize; do so here.
    _sceneVersion.store(0);

    // Store top-level embree objects inside a render param that can be
    // passed to prims during Sync(). Also pass a handle to the render thread.
    _renderParam = std::make_shared<HdEmbreeRenderParam>(
        _rtcDevice, _rtcScene, &_renderThread, &_renderer, &_sceneVersion);

    // Pass the scene handle to the renderer.
    _renderer.SetScene(_rtcScene);

    // Set the background render thread's rendering entrypoint to
    // HdEmbreeRenderer::Render.
    _renderThread.SetRenderCallback(
        std::bind(_RenderCallback, &_renderer, &_renderThread));
    // Start the background render thread.
    _renderThread.StartThread();

    // Initialize one resource registry for all embree plugins
    std::lock_guard<std::mutex> guard(_mutexResourceRegistry);

    if (_counterResourceRegistry.fetch_add(1) == 0) {
        _resourceRegistry = std::make_shared<HdResourceRegistry>();
    }
}

HdEmbreeRenderDelegate::~HdEmbreeRenderDelegate()
{
    // Clean the resource registry only when it is the last Embree delegate
    {
        std::lock_guard<std::mutex> guard(_mutexResourceRegistry);
        if (_counterResourceRegistry.fetch_sub(1) == 1) {
            _resourceRegistry.reset();
        }
    }

    _renderThread.StopThread();

    // Destroy embree library and scene state.
    _renderParam.reset();
    rtcReleaseScene(_rtcScene);
    rtcReleaseDevice (_rtcDevice);
}

HdRenderSettingDescriptorList
HdEmbreeRenderDelegate::GetRenderSettingDescriptors() const
{
    return _settingDescriptors;
}

TfTokenVector
HdEmbreeRenderDelegate::GetRenderSettingsNamespaces() const
{
    static const TfTokenVector namespaces = {
        TfToken("ty", TfToken::Immortal),
        TfToken()
    };
    return namespaces;
}

TfTokenVector
HdEmbreeRenderDelegate::GetMaterialRenderContexts() const
{
    const std::string context = _GetMaterialRenderContextSetting(*this);

    if (context == _materialRenderContextMtlxToken.GetString()) {
        return {_materialRenderContextMtlxToken, TfToken()};
    }
    if (context == _materialRenderContextDefaultToken.GetString()) {
        return {TfToken(), _materialRenderContextMtlxToken};
    }

    TF_WARN("hdEmbree material render context '%s' is unknown; falling back "
            "to 'mtlx' with 'default' as the secondary context.",
            context.c_str());
    return {_materialRenderContextMtlxToken, TfToken()};
}

HdRenderParam*
HdEmbreeRenderDelegate::GetRenderParam() const
{
    return _renderParam.get();
}

void
HdEmbreeRenderDelegate::CommitResources(HdChangeTracker *tracker)
{
}

TfTokenVector const&
HdEmbreeRenderDelegate::GetSupportedRprimTypes() const
{
    return SUPPORTED_RPRIM_TYPES;
}

TfTokenVector const&
HdEmbreeRenderDelegate::GetSupportedSprimTypes() const
{
    return SUPPORTED_SPRIM_TYPES;
}

TfTokenVector const&
HdEmbreeRenderDelegate::GetSupportedBprimTypes() const
{
    return SUPPORTED_BPRIM_TYPES;
}

HdResourceRegistrySharedPtr
HdEmbreeRenderDelegate::GetResourceRegistry() const
{
    return _resourceRegistry;
}

HdAovDescriptor
HdEmbreeRenderDelegate::GetDefaultAovDescriptor(TfToken const& name) const
{
    if (name == HdAovTokens->color) {
        return HdAovDescriptor(HdFormatFloat32Vec4, true,
                               VtValue(GfVec4f(0.0f)));
    } else if (name == HdAovTokens->normal || name == HdAovTokens->Neye) {
        return HdAovDescriptor(HdFormatFloat32Vec3, false,
                               VtValue(GfVec3f(-1.0f)));
    } else if (name == HdAovTokens->depth) {
        return HdAovDescriptor(HdFormatFloat32, false, VtValue(1.0f));
    } else if (name == HdAovTokens->cameraDepth) {
        return HdAovDescriptor(HdFormatFloat32, false, VtValue(0.0f));
    } else if (name == HdAovTokens->primId ||
               name == HdAovTokens->instanceId ||
               name == HdAovTokens->elementId) {
        return HdAovDescriptor(HdFormatInt32, false, VtValue(-1));
    } else if (name == HdEmbreeAovTokens->adaptiveHeatmap) {
        return HdAovDescriptor(HdFormatFloat32Vec4, true,
                               VtValue(GfVec4f(0.0f)));
    } else {
        HdParsedAovToken aovId(name);
        if (aovId.isPrimvar) {
            return HdAovDescriptor(HdFormatFloat32Vec3, false,
                                   VtValue(GfVec3f(0.0f)));
        }
    }

    return HdAovDescriptor();
}

VtDictionary
HdEmbreeRenderDelegate::GetRenderStats() const
{
    VtDictionary stats;
    const uint64_t sssCalls = _renderer.GetSssCallCount();
    const uint64_t sssSuccesses = _renderer.GetSssSuccessCount();
    const uint64_t sssWalkSteps = _renderer.GetSssWalkStepCount();
    const uint64_t sssIntersections = _renderer.GetSssIntersectionCount();

    stats[HdPerfTokens->numCompletedSamples.GetString()] =
        _renderer.GetCompletedSamples();
    stats["renderTimeSeconds"] = _renderer.GetRenderElapsedSeconds();
    stats["sssCallCount"] = static_cast<int64_t>(sssCalls);
    stats["sssSuccessCount"] = static_cast<int64_t>(sssSuccesses);
    stats["sssWalkStepCount"] = static_cast<int64_t>(sssWalkSteps);
    stats["sssIntersectionCount"] = static_cast<int64_t>(sssIntersections);
    stats["sssSuccessRate"] = (sssCalls > 0)
        ? static_cast<double>(sssSuccesses) / static_cast<double>(sssCalls)
        : 0.0;
    stats["sssAvgStepsPerCall"] = (sssCalls > 0)
        ? static_cast<double>(sssWalkSteps) / static_cast<double>(sssCalls)
        : 0.0;
    stats["sssAvgStepsPerSuccess"] = (sssSuccesses > 0)
        ? static_cast<double>(sssWalkSteps) / static_cast<double>(sssSuccesses)
        : 0.0;
    return stats;
}

bool
HdEmbreeRenderDelegate::IsPauseSupported() const
{
    return true;
}

bool
HdEmbreeRenderDelegate::Pause()
{
    _renderThread.PauseRender();
    return true;
}

bool
HdEmbreeRenderDelegate::Resume()
{
    _renderThread.ResumeRender();
    return true;
}

HdRenderPassSharedPtr
HdEmbreeRenderDelegate::CreateRenderPass(HdRenderIndex *index,
                            HdRprimCollection const& collection)
{
    return HdRenderPassSharedPtr(new HdEmbreeRenderPass(
        index, collection, &_renderThread, &_renderer, &_sceneVersion));
}

HdInstancer *
HdEmbreeRenderDelegate::CreateInstancer(HdSceneDelegate *delegate,
                                        SdfPath const& id)
{
    return new HdEmbreeInstancer(delegate, id);
}

void
HdEmbreeRenderDelegate::DestroyInstancer(HdInstancer *instancer)
{
    delete instancer;
}

HdRprim *
HdEmbreeRenderDelegate::CreateRprim(TfToken const& typeId,
                                    SdfPath const& rprimId)
{
    if (typeId == HdPrimTypeTokens->mesh) {
        auto* mesh = new HdEmbreeMesh(rprimId);
        {
            std::lock_guard<std::mutex> lock(_meshRegistryMutex);
            _meshes.push_back(mesh);
        }
        return mesh;
    } else {
        TF_CODING_ERROR("Unknown Rprim Type %s", typeId.GetText());
    }

    return nullptr;
}

void
HdEmbreeRenderDelegate::DestroyRprim(HdRprim *rPrim)
{
    if (auto* mesh = dynamic_cast<HdEmbreeMesh*>(rPrim)) {
        std::lock_guard<std::mutex> lock(_meshRegistryMutex);
        auto it = std::find(_meshes.begin(), _meshes.end(), mesh);
        if (it != _meshes.end()) {
            _meshes.erase(it);
        }
    }
    delete rPrim;
}

bool
HdEmbreeRenderDelegate::UpdateAdaptiveSubdivision(
    GfMatrix4d const& viewMatrix,
    GfMatrix4d const& projectionMatrix,
    GfRect2i const& dataWindow,
    bool forceDisplacementRebuild)
{
    bool changed = false;
    std::lock_guard<std::mutex> lock(_meshRegistryMutex);
    for (HdEmbreeMesh* mesh : _meshes) {
        changed |= mesh->UpdateSubdivisionLevels(
            viewMatrix, projectionMatrix, dataWindow,
            forceDisplacementRebuild);
    }
    return changed;
}

HdSprim *
HdEmbreeRenderDelegate::CreateSprim(TfToken const& typeId,
                                    SdfPath const& sprimId)
{
    if (typeId == HdPrimTypeTokens->camera) {
        return new HdCamera(sprimId);
    } else if (typeId == HdPrimTypeTokens->extComputation) {
        return new HdExtComputation(sprimId);
    } else if (typeId == HdPrimTypeTokens->material) {
        return new HdEmbreeMaterial(sprimId);
    } else if (typeId == HdPrimTypeTokens->light ||
               typeId == HdPrimTypeTokens->distantLight ||
               typeId == HdPrimTypeTokens->diskLight ||
               typeId == HdPrimTypeTokens->distantLight ||
               typeId == HdPrimTypeTokens->domeLight ||
               typeId == HdPrimTypeTokens->rectLight ||
               typeId == HdPrimTypeTokens->sphereLight ||
               typeId == HdPrimTypeTokens->cylinderLight) {
        return new HdEmbree_Light(sprimId, typeId);
    } else {
        TF_CODING_ERROR("Unknown Sprim Type %s", typeId.GetText());
    }

    return nullptr;
}

HdSprim *
HdEmbreeRenderDelegate::CreateFallbackSprim(TfToken const& typeId)
{
    // For fallback sprims, create objects with an empty scene path.
    // They'll use default values and won't be updated by a scene delegate.
    if (typeId == HdPrimTypeTokens->camera) {
        return new HdCamera(SdfPath::EmptyPath());
    } else if (typeId == HdPrimTypeTokens->extComputation) {
        return new HdExtComputation(SdfPath::EmptyPath());
    } else if (typeId == HdPrimTypeTokens->material) {
        return new HdEmbreeMaterial(SdfPath::EmptyPath());
    } else if (typeId == HdPrimTypeTokens->light ||
               typeId == HdPrimTypeTokens->distantLight ||
               typeId == HdPrimTypeTokens->diskLight ||
               typeId == HdPrimTypeTokens->distantLight ||
               typeId == HdPrimTypeTokens->domeLight ||
               typeId == HdPrimTypeTokens->rectLight ||
               typeId == HdPrimTypeTokens->sphereLight ||
               typeId == HdPrimTypeTokens->cylinderLight) {
        return new HdEmbree_Light(SdfPath::EmptyPath(), typeId);
    } else {
        TF_CODING_ERROR("Unknown Sprim Type %s", typeId.GetText());
    }

    return nullptr;
}

void
HdEmbreeRenderDelegate::DestroySprim(HdSprim *sPrim)
{
    delete sPrim;
}

HdBprim *
HdEmbreeRenderDelegate::CreateBprim(TfToken const& typeId,
                                    SdfPath const& bprimId)
{
    if (typeId == HdPrimTypeTokens->renderBuffer) {
        return new HdEmbreeRenderBuffer(bprimId);
    } else {
        TF_CODING_ERROR("Unknown Bprim Type %s", typeId.GetText());
    }
    return nullptr;
}

HdBprim *
HdEmbreeRenderDelegate::CreateFallbackBprim(TfToken const& typeId)
{
    if (typeId == HdPrimTypeTokens->renderBuffer) {
        return new HdEmbreeRenderBuffer(SdfPath::EmptyPath());
    } else {
        TF_CODING_ERROR("Unknown Bprim Type %s", typeId.GetText());
    }
    return nullptr;
}

void
HdEmbreeRenderDelegate::DestroyBprim(HdBprim *bPrim)
{
    delete bPrim;
}

PXR_NAMESPACE_CLOSE_SCOPE
