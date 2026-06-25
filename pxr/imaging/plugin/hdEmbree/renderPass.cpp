//
// Copyright 2016 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/imaging/hd/camera.h"
#include "pxr/imaging/hd/renderPassState.h"
#include "pxr/imaging/hd/material.h"
#include "pxr/imaging/hd/renderSettingsSchema.h"
#include "pxr/imaging/hd/sceneGlobalsSchema.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/hd/utils.h"
#include "pxr/imaging/plugin/hdEmbree/config.h"
#include "pxr/imaging/plugin/hdEmbree/material.h"
#include "pxr/imaging/plugin/hdEmbree/renderDelegate.h"
#include "pxr/imaging/plugin/hdEmbree/renderPass.h"
#include "pxr/base/tf/diagnostic.h"

#include <cmath>
#include <string>

PXR_NAMESPACE_OPEN_SCOPE

static TfToken
_GetTokenRenderSetting(
    HdRenderDelegate const *renderDelegate,
    TfToken const &key,
    TfToken const &defaultValue)
{
    const VtValue value = renderDelegate->GetRenderSetting(key);
    if (value.IsHolding<TfToken>()) {
        return value.UncheckedGet<TfToken>();
    }
    if (value.IsHolding<std::string>()) {
        return TfToken(value.UncheckedGet<std::string>());
    }
    return defaultValue;
}

using _RenderSettingsMap =
    TfHashMap<TfToken, VtValue, TfToken::HashFunctor>;

static void
_AddNamespacedRenderSettings(
    HdSampledDataSourceContainerSchema const &namespacedSettings,
    std::string const &prefix,
    _RenderSettingsMap *renderSettings)
{
    HdContainerDataSourceHandle container = namespacedSettings.GetContainer();
    if (!container) {
        return;
    }

    for (const TfToken &name : container->GetNames()) {
        const std::string nameString = name.GetString();
        if (nameString.compare(0, prefix.size(), prefix) != 0) {
            continue;
        }

        if (auto ds = container->Get(name)) {
            if (auto sampled = HdSampledDataSource::Cast(ds)) {
                VtValue value = sampled->GetValue(0);
                if (!value.IsEmpty()) {
                    (*renderSettings)[name] = value;
                }
            }
        }
    }
}

static _RenderSettingsMap
_GetNamespacedRenderSettings(HdRenderSettingsSchema const &rsSchema)
{
    _RenderSettingsMap renderSettings;
    if (!rsSchema.IsDefined()) {
        return renderSettings;
    }

    HdSampledDataSourceContainerSchema namespacedSettings =
        rsSchema.GetNamespacedSettings();
    _AddNamespacedRenderSettings(
        namespacedSettings, "ty:", &renderSettings);
    return renderSettings;
}

static bool
_RenderSettingsEqual(
    _RenderSettingsMap const &a,
    _RenderSettingsMap const &b)
{
    if (a.size() != b.size()) {
        return false;
    }

    for (const auto &entry : a) {
        const auto it = b.find(entry.first);
        if (it == b.end() || it->second != entry.second) {
            return false;
        }
    }
    return true;
}

static bool
_GetRenderSettingDefault(
    HdRenderDelegate const *delegate,
    TfToken const &key,
    VtValue *value)
{
    for (HdRenderSettingDescriptor const &descriptor :
             delegate->GetRenderSettingDescriptors()) {
        if (descriptor.key == key) {
            *value = descriptor.defaultValue;
            return true;
        }
    }
    return false;
}

HdEmbreeRenderPass::HdEmbreeRenderPass(HdRenderIndex *index,
                                       HdRprimCollection const &collection,
                                       HdRenderThread *renderThread,
                                       HdEmbreeRenderer *renderer,
                                       std::atomic<int> *sceneVersion)
    : HdRenderPass(index, collection)
    , _renderThread(renderThread)
    , _renderer(renderer)
    , _sceneVersion(sceneVersion)
    , _lastSceneVersion(0)
    , _lastSettingsVersion(0)
    , _lastRenderSettingsPrimPath()
    , _hasAppliedRenderSettingsPrim(false)
    , _lastBridgedRenderSettings()
    , _lastMaterialRenderContexts()
    , _lastFrame(0.0)
    , _lastTime(0.0)
    , _viewMatrix(1.0f) // == identity
    , _projMatrix(1.0f) // == identity
    , _cameraExposureScale(1.0f)
    , _cameraDepthOfField()
    , _aovBindings()
    , _colorBuffer(SdfPath::EmptyPath())
    , _depthBuffer(SdfPath::EmptyPath())
    , _converged(false)
{
}

HdEmbreeRenderPass::~HdEmbreeRenderPass()
{
    // Make sure the render thread's not running, in case it's writing
    // to _colorBuffer/_depthBuffer.
    _renderThread->StopRender();
}

bool
HdEmbreeRenderPass::IsConverged() const
{
    // If the aov binding array is empty, the render thread is rendering into
    // _colorBuffer and _depthBuffer.  _converged is set to their convergence
    // state just before blit, so use that as our answer.
    if (_aovBindings.size() == 0) {
        return _converged;
    }

    // Otherwise, check the convergence of all attachments.
    for (size_t i = 0; i < _aovBindings.size(); ++i) {
        if (_aovBindings[i].renderBuffer &&
            !_aovBindings[i].renderBuffer->IsConverged()) {
            return false;
        }
    }
    return true;
}

static
GfRect2i
_GetDataWindow(HdRenderPassStateSharedPtr const& renderPassState)
{
    const CameraUtilFraming &framing = renderPassState->GetFraming();
    if (framing.IsValid()) {
        return framing.dataWindow;
    } else {
        // For applications that use the old viewport API instead of
        // the new camera framing API.
        const GfVec4f vp = renderPassState->GetViewport();
        return GfRect2i(GfVec2i(0), int(vp[2]), int(vp[3]));        
    }
}

static float
_GetCameraExposureScale(
    HdRenderPassStateSharedPtr const& renderPassState,
    HdRenderDelegate const *renderDelegate)
{
    HdCamera const * const camera = renderPassState->GetCamera();
    const bool enableExposureCompensation =
        renderDelegate->GetRenderSetting<bool>(
            HdEmbreeRenderSettingsTokens->enableExposureCompensation,
            renderPassState->GetEnableExposureCompensation());
    if (camera && enableExposureCompensation) {
        return camera->GetLinearExposureScale();
    }
    return 1.0f;
}

static float
_FiniteOrZero(float value)
{
    return std::isfinite(value) ? value : 0.0f;
}

static HdEmbreeCameraDepthOfField
_GetCameraDepthOfField(HdRenderPassStateSharedPtr const& renderPassState)
{
    HdCamera const * const camera = renderPassState->GetCamera();
    if (!camera) {
        return HdEmbreeCameraDepthOfField();
    }

    HdEmbreeCameraDepthOfField result;
    result.fStop = _FiniteOrZero(camera->GetFStop());
    result.focusDistance = _FiniteOrZero(camera->GetFocusDistance());
    result.focalLength = _FiniteOrZero(camera->GetFocalLength());
    return result;
}

static void
_GetSceneFrameAndTime(const HdSceneIndexBaseRefPtr &si,
                      double *frame,
                      double *time)
{
    *frame = 0.0;
    *time = 0.0;

    if (!si) {
        return;
    }

    const HdSceneGlobalsSchema sgSchema =
        HdSceneGlobalsSchema::GetFromSceneIndex(si);
    if (!sgSchema) {
        return;
    }

    double currentFrame = 0.0;
    if (auto frameHandle = sgSchema.GetCurrentFrame()) {
        const double value = frameHandle->GetTypedValue(0);
        if (std::isfinite(value)) {
            currentFrame = value;
        }
    }

    double timeCodesPerSecond = 1.0;
    if (auto tcpsHandle = sgSchema.GetTimeCodesPerSecond()) {
        const double value = tcpsHandle->GetTypedValue(0);
        if (std::isfinite(value) && value > 0.0) {
            timeCodesPerSecond = value;
        }
    }

    *frame = currentFrame;
    *time = currentFrame / timeCodesPerSecond;
}

static void
_ResyncMaterialNetworksForRenderContextChange(HdRenderIndex *index)
{
    if (!index || !index->IsSprimTypeSupported(HdPrimTypeTokens->material)) {
        return;
    }

    HdRenderParam *renderParam = index->GetRenderDelegate()->GetRenderParam();
    for (const SdfPath &path :
             index->GetSprimSubtree(
                 HdPrimTypeTokens->material,
                 SdfPath::AbsoluteRootPath())) {
        HdEmbreeMaterial *material = dynamic_cast<HdEmbreeMaterial *>(
            index->GetSprim(HdPrimTypeTokens->material, path));
        if (material) {
            material->ResyncForRenderContextChange(renderParam);
        }
    }
}

bool
HdEmbreeRenderPass::_UpdateRenderSettingsFromActiveRenderSettingsPrim()
{
    HdRenderIndex *index = GetRenderIndex();
    HdRenderDelegate *delegate = index->GetRenderDelegate();
    HdSceneIndexBaseRefPtr si = index->GetTerminalSceneIndex();

    SdfPath rsPath;
    const bool hasActiveRenderSettingsPrim =
        HdUtils::HasActiveRenderSettingsPrim(si, &rsPath);

    _RenderSettingsMap currentRenderSettings;
    if (hasActiveRenderSettingsPrim) {
        HdSceneIndexPrim prim = si->GetPrim(rsPath);
        currentRenderSettings = _GetNamespacedRenderSettings(
            HdRenderSettingsSchema::GetFromParent(prim.dataSource));
    }

    const bool activeRenderSettingsPrimChanged =
        hasActiveRenderSettingsPrim != _hasAppliedRenderSettingsPrim ||
        (hasActiveRenderSettingsPrim &&
         rsPath != _lastRenderSettingsPrimPath);
    const bool renderSettingsChanged =
        !_RenderSettingsEqual(
            currentRenderSettings, _lastBridgedRenderSettings);

    if (!activeRenderSettingsPrimChanged && !renderSettingsChanged) {
        return false;
    }

    const unsigned int oldVersion = delegate->GetRenderSettingsVersion();
    _RenderSettingsMap newBridgedRenderSettings;

    for (const auto &previous : _lastBridgedRenderSettings) {
        const TfToken &key = previous.first;
        const auto currentIt = currentRenderSettings.find(key);
        const VtValue delegateValue = delegate->GetRenderSetting(key);
        const bool bridgeOwnsKey = delegateValue == previous.second;

        if (currentIt == currentRenderSettings.end()) {
            if (bridgeOwnsKey) {
                VtValue defaultValue;
                if (_GetRenderSettingDefault(delegate, key, &defaultValue)) {
                    delegate->SetRenderSetting(key, defaultValue);
                } else {
                    delegate->SetRenderSetting(key, VtValue());
                }
            }
            continue;
        }

        if (bridgeOwnsKey) {
            delegate->SetRenderSetting(key, currentIt->second);
            newBridgedRenderSettings[key] = currentIt->second;
        } else if (delegateValue == currentIt->second) {
            newBridgedRenderSettings[key] = currentIt->second;
        }
    }

    for (const auto &current : currentRenderSettings) {
        const TfToken &key = current.first;
        if (_lastBridgedRenderSettings.find(key) !=
            _lastBridgedRenderSettings.end()) {
            continue;
        }

        const VtValue delegateValue = delegate->GetRenderSetting(key);
        if (delegateValue == current.second) {
            newBridgedRenderSettings[key] = current.second;
            continue;
        }

        VtValue defaultValue;
        const bool hasDefaultValue =
            _GetRenderSettingDefault(delegate, key, &defaultValue);
        if (delegateValue.IsEmpty() ||
            !hasDefaultValue ||
            delegateValue == defaultValue) {
            delegate->SetRenderSetting(key, current.second);
            newBridgedRenderSettings[key] = current.second;
        }
    }

    _lastRenderSettingsPrimPath =
        hasActiveRenderSettingsPrim ? rsPath : SdfPath();
    _hasAppliedRenderSettingsPrim = hasActiveRenderSettingsPrim;
    _lastBridgedRenderSettings.swap(newBridgedRenderSettings);

    return delegate->GetRenderSettingsVersion() != oldVersion;
}

void
HdEmbreeRenderPass::_Execute(HdRenderPassStateSharedPtr const& renderPassState,
                             TfTokenVector const &renderTags)
{
    // XXX: Add collection and renderTags support.
    // XXX: Add clip planes support.

    // Determine whether the scene has changed since the last time we rendered.
    bool needStartRender = false;
    if (_UpdateRenderSettingsFromActiveRenderSettingsPrim()) {
        needStartRender = true;
    }

    int currentSceneVersion = _sceneVersion->load();
    if (_lastSceneVersion != currentSceneVersion) {
        needStartRender = true;
        _lastSceneVersion = currentSceneVersion;
    }

    {
        HdRenderIndex *index = GetRenderIndex();
        const HdSceneIndexBaseRefPtr si = index->GetTerminalSceneIndex();
        double currentFrame = 0.0;
        double currentTime = 0.0;
        _GetSceneFrameAndTime(si, &currentFrame, &currentTime);
        if (_lastFrame != currentFrame || _lastTime != currentTime) {
            _renderThread->StopRender();
            _renderer->SetSceneFrameAndTime(
                static_cast<float>(currentFrame),
                static_cast<float>(currentTime));
            _lastFrame = currentFrame;
            _lastTime = currentTime;
            needStartRender = true;
        }
    }

    // Likewise the render settings.
    HdRenderDelegate *renderDelegate = GetRenderIndex()->GetRenderDelegate();
    int currentSettingsVersion = renderDelegate->GetRenderSettingsVersion();
    if (_lastSettingsVersion != currentSettingsVersion) {
        _renderThread->StopRender();
        _lastSettingsVersion = currentSettingsVersion;

        bool materialRenderContextsChanged = false;
        const TfTokenVector materialRenderContexts =
            renderDelegate->GetMaterialRenderContexts();
        if (_lastMaterialRenderContexts.empty()) {
            _lastMaterialRenderContexts = materialRenderContexts;
        } else if (_lastMaterialRenderContexts != materialRenderContexts) {
            _lastMaterialRenderContexts = materialRenderContexts;
            materialRenderContextsChanged = true;
        }

        const HdEmbreeConfig &config = HdEmbreeConfig::GetInstance();

        _renderer->SetSamplesToConvergence(
            renderDelegate->GetRenderSetting<int>(
                HdEmbreeRenderSettingsTokens->convergedSamplesPerPixel,
                config.samplesToConvergence));

        bool enableLighting =
            renderDelegate->GetRenderSetting<bool>(
                HdEmbreeRenderSettingsTokens->enableLighting,
                config.enableLighting);
        if (enableLighting) {
            _renderer->SetEnableLighting(true);
            _renderer->SetAmbientOcclusionSamples(0);
        } else {
            _renderer->SetEnableLighting(false);
            bool enableAmbientOcclusion =
                renderDelegate->GetRenderSetting<bool>(
                    HdEmbreeRenderSettingsTokens->enableAmbientOcclusion,
                    config.enableAmbientOcclusion);
            if (enableAmbientOcclusion) {
                _renderer->SetAmbientOcclusionSamples(
                    renderDelegate->GetRenderSetting<int>(
                        HdEmbreeRenderSettingsTokens->ambientOcclusionSamples,
                        config.ambientOcclusionSamples));
            } else {
                _renderer->SetAmbientOcclusionSamples(0);
            }
        }

        _renderer->SetDomeLightCameraVisibility(
            renderDelegate->GetRenderSetting<bool>(
                HdEmbreeRenderSettingsTokens->domeLightCameraVisibility,
                config.domeLightCameraVisibility));

        _renderer->SetEnableSceneColors(
            renderDelegate->GetRenderSetting<bool>(
                HdEmbreeRenderSettingsTokens->enableSceneColors,
                config.enableSceneColors));

        _renderer->SetRandomNumberSeed(
            renderDelegate->GetRenderSetting<int>(
                HdEmbreeRenderSettingsTokens->randomNumberSeed,
                config.randomNumberSeed));

        HdEmbreeSamplerSequence defaultSamplerSequence =
            HdEmbreeGetSamplerSequenceFromToken(TfToken(config.samplerSequence));
        TfToken defaultSamplerSequenceToken =
            HdEmbreeGetSamplerSequenceToken(defaultSamplerSequence);
        if (defaultSamplerSequenceToken != TfToken(config.samplerSequence)) {
            defaultSamplerSequence =
                HdEmbreeGetDefaultSamplerSequence();
            defaultSamplerSequenceToken =
                HdEmbreeGetSamplerSequenceToken(defaultSamplerSequence);
        }
        TfToken samplerSequenceToken = _GetTokenRenderSetting(
            renderDelegate,
            HdEmbreeRenderSettingsTokens->samplerSequence,
            defaultSamplerSequenceToken);
        HdEmbreeSamplerSequence samplerSequence =
            HdEmbreeGetSamplerSequenceFromToken(samplerSequenceToken);
        if (HdEmbreeGetSamplerSequenceToken(samplerSequence) !=
            samplerSequenceToken) {
            TF_WARN("hdEmbree sampler sequence '%s' is unknown; "
                    "falling back to '%s'.",
                    samplerSequenceToken.GetText(),
                    defaultSamplerSequenceToken.GetText());
            samplerSequence = defaultSamplerSequence;
        }
        _renderer->SetSamplerSequence(samplerSequence);

        _renderer->SetEnableAdaptiveSampling(
            renderDelegate->GetRenderSetting<bool>(
                HdEmbreeRenderSettingsTokens->enableAdaptiveSampling,
                config.enableAdaptiveSampling));
        _renderer->SetAdaptiveThreshold(
            renderDelegate->GetRenderSetting<float>(
                HdEmbreeRenderSettingsTokens->adaptiveThreshold,
                config.adaptiveThreshold));
        _renderer->SetMinSamplesBeforeAdaptive(
            renderDelegate->GetRenderSetting<int>(
                HdEmbreeRenderSettingsTokens->minSamplesBeforeAdaptive,
                config.minSamplesBeforeAdaptive));
        _renderer->SetMaxBounces(
            renderDelegate->GetRenderSetting<int>(
                HdEmbreeRenderSettingsTokens->maxBounces,
                config.maxBounces));
        _renderer->SetMinBouncesBeforeRR(
            renderDelegate->GetRenderSetting<int>(
                HdEmbreeRenderSettingsTokens->minBouncesBeforeRR,
                config.minBouncesBeforeRR));
        _renderer->SetLightSamplesPerHit(
            renderDelegate->GetRenderSetting<int>(
                HdEmbreeRenderSettingsTokens->lightSamplesPerHit,
                config.lightSamplesPerHit));
        _renderer->SetStratifyLightSamples(
            renderDelegate->GetRenderSetting<bool>(
                HdEmbreeRenderSettingsTokens->stratifyLightSamples,
                config.stratifyLightSamples));
        _renderer->SetShowAdaptiveHeatmap(
            renderDelegate->GetRenderSetting<bool>(
                HdEmbreeRenderSettingsTokens->showAdaptiveHeatmap,
                config.showAdaptiveHeatmap));
        _renderer->SetFireflyClampThreshold(
            renderDelegate->GetRenderSetting<float>(
                HdEmbreeRenderSettingsTokens->fireflyClampThreshold,
                config.fireflyClampThreshold));
        _renderer->SetEnableCaustics(
            renderDelegate->GetRenderSetting<bool>(
                HdEmbreeRenderSettingsTokens->enableCaustics,
                config.enableCaustics));
        _renderer->SetCausticsClampThreshold(
            renderDelegate->GetRenderSetting<float>(
                HdEmbreeRenderSettingsTokens->causticsClampThreshold,
                config.causticsClampThreshold));
        _renderer->SetApproxTransparentShadows(
            renderDelegate->GetRenderSetting<bool>(
                HdEmbreeRenderSettingsTokens->approxTransparentShadows,
                config.approxTransparentShadows));
        _renderer->SetEnableGgxMicrofacetMultipleScattering(
            renderDelegate->GetRenderSetting<bool>(
                HdEmbreeRenderSettingsTokens
                    ->enableGgxMicrofacetMultipleScattering,
                config.enableGgxMicrofacetMultipleScattering));
        _renderer->SetDielectricLayerThroughputMode(
            _GetTokenRenderSetting(
                renderDelegate,
                HdEmbreeRenderSettingsTokens->dielectricLayerThroughputMode,
                TfToken(config.dielectricLayerThroughputMode)));
        _renderer->SetUseAdobeOpenPBR(
            renderDelegate->GetRenderSetting<bool>(
                HdEmbreeRenderSettingsTokens->useAdobeOpenPBR,
                config.useAdobeOpenPBR));

        if (materialRenderContextsChanged) {
            _ResyncMaterialNetworksForRenderContextChange(GetRenderIndex());
        }

        needStartRender = true;
    }

    // Determine whether we need to update the renderer camera.
    const GfMatrix4d view = renderPassState->GetWorldToViewMatrix();
    const GfMatrix4d proj = renderPassState->GetProjectionMatrix();
    const float cameraExposureScale =
        _GetCameraExposureScale(renderPassState, renderDelegate);
    const HdEmbreeCameraDepthOfField cameraDepthOfField =
        _GetCameraDepthOfField(renderPassState);
    if (_viewMatrix != view || _projMatrix != proj ||
        _cameraExposureScale != cameraExposureScale ||
        _cameraDepthOfField != cameraDepthOfField) {
        _viewMatrix = view;
        _projMatrix = proj;
        _cameraExposureScale = cameraExposureScale;
        _cameraDepthOfField = cameraDepthOfField;

        _renderThread->StopRender();
        _renderer->SetCamera(_viewMatrix, _projMatrix);
        _renderer->SetCameraExposureScale(_cameraExposureScale);
        _renderer->SetCameraDepthOfField(_cameraDepthOfField);
        _renderer->ResetAccumulation();
        needStartRender = true;
    }

    const GfRect2i dataWindow = _GetDataWindow(renderPassState);

    if (_dataWindow != dataWindow) {
        _dataWindow = dataWindow;

        _renderThread->StopRender();
        _renderer->SetDataWindow(dataWindow);
        _renderer->ResetAccumulation();

        if (!renderPassState->GetFraming().IsValid()) {
            // Support clients that do not use the new framing API
            // and do not use AOVs.
            //
            // Note that we do not support the case of using the
            // new camera framing API without using AOVs.
            //
            const GfVec3i dimensions(_dataWindow.GetWidth(),
                                     _dataWindow.GetHeight(),
                                     1);

            _colorBuffer.Allocate(
                dimensions,
                HdFormatUNorm8Vec4,
                /*multiSampled=*/true);
            
            _depthBuffer.Allocate(
                dimensions,
                HdFormatFloat32,
                /*multiSampled=*/false);
        }

        needStartRender = true;
    }

    // Determine whether we need to update the renderer AOV bindings.
    //
    // It's possible for the passed in bindings to be empty, but that's
    // never a legal state for the renderer, so if that's the case we add
    // a color and depth aov.
    //
    // If the renderer AOV bindings are empty, force a bindings update so that
    // we always get a chance to add color/depth on the first time through.
    HdRenderPassAovBindingVector aovBindings =
        renderPassState->GetAovBindings();
    if (_aovBindings != aovBindings || _renderer->GetAovBindings().empty()) {
        _aovBindings = aovBindings;

        _renderThread->StopRender();
        if (aovBindings.empty()) {
            HdRenderPassAovBinding colorAov;
            colorAov.aovName = HdAovTokens->color;
            colorAov.renderBuffer = &_colorBuffer;
            colorAov.clearValue =
                VtValue(GfVec4f(0.0707f, 0.0707f, 0.0707f, 1.0f));
            aovBindings.push_back(colorAov);
            HdRenderPassAovBinding depthAov;
            depthAov.aovName = HdAovTokens->depth;
            depthAov.renderBuffer = &_depthBuffer;
            depthAov.clearValue = VtValue(1.0f);
            aovBindings.push_back(depthAov);
        }
        _renderer->SetAovBindings(aovBindings);
        // Preserve the last resolved image across restarts and only reset
        // progressive accumulation state on the new attachments.
        _renderer->ResetAccumulation();
        needStartRender = true;
    }

    TF_VERIFY(!_aovBindings.empty(), "No aov bindings to render into");

    // Only start a new render if something in the scene has changed.
    if (needStartRender) {
        _converged = false;
        _renderer->MarkAovBuffersUnconverged();
        _renderThread->StartRender();
    }
}

PXR_NAMESPACE_CLOSE_SCOPE
