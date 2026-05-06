//
// Copyright 2016 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/imaging/hd/renderPassState.h"
#include "pxr/imaging/hd/renderSettingsSchema.h"
#include "pxr/imaging/hd/sceneGlobalsSchema.h"
#include "pxr/imaging/hd/utils.h"
#include "pxr/imaging/plugin/hdEmbree/config.h"
#include "pxr/imaging/plugin/hdEmbree/renderDelegate.h"
#include "pxr/imaging/plugin/hdEmbree/renderPass.h"
#include "pxr/base/tf/diagnostic.h"

#include <cmath>

PXR_NAMESPACE_OPEN_SCOPE

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
    , _lastFrame(0.0)
    , _lastTime(0.0)
    , _viewMatrix(1.0f) // == identity
    , _projMatrix(1.0f) // == identity
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

void
HdEmbreeRenderPass::_Execute(HdRenderPassStateSharedPtr const& renderPassState,
                             TfTokenVector const &renderTags)
{
    // XXX: Add collection and renderTags support.
    // XXX: Add clip planes support.

    // Determine whether the scene has changed since the last time we rendered.
    bool needStartRender = false;
    int currentSceneVersion = _sceneVersion->load();
    if (_lastSceneVersion != currentSceneVersion) {
        needStartRender = true;
        _lastSceneVersion = currentSceneVersion;

        // Apply namespacedSettings from the active RenderSettings prim to
        // the delegate.  This bridges the gap between USD RenderSettings
        // prims and hdEmbree's render delegate settings map.
        // Only run when the scene changes (stage load / prim sync) so that
        // interactive GUI changes are not overwritten every frame.
        HdRenderIndex *index = GetRenderIndex();
        HdSceneIndexBaseRefPtr si = index->GetTerminalSceneIndex();
        SdfPath rsPath;
        if (HdUtils::HasActiveRenderSettingsPrim(si, &rsPath)) {
            HdSceneIndexPrim prim = si->GetPrim(rsPath);
            HdRenderSettingsSchema rsSchema =
                HdRenderSettingsSchema::GetFromParent(prim.dataSource);
            if (rsSchema.IsDefined()) {
                HdSampledDataSourceContainerSchema nsSettings =
                    rsSchema.GetNamespacedSettings();
                if (nsSettings.GetContainer()) {
                    TfTokenVector names =
                        nsSettings.GetContainer()->GetNames();
                    HdRenderDelegate *delegate =
                        index->GetRenderDelegate();
                    // The "hdEmbree:" namespace prefix to strip from keys.
                    static const std::string nsPrefix("hdEmbree:");
                    for (const TfToken &name : names) {
                        // Only process settings in our namespace.
                        const std::string &nameStr = name.GetString();
                        if (nameStr.substr(0, nsPrefix.size()) != nsPrefix){
                            continue;
                        }
                        // Strip the namespace prefix to get the
                        // render delegate setting token.
                        TfToken settingName(
                            nameStr.substr(nsPrefix.size()));
                        if (auto ds =
                                nsSettings.GetContainer()->Get(name)) {
                            if (auto sampled =
                                    HdSampledDataSource::Cast(ds)) {
                                delegate->SetRenderSetting(
                                    settingName, sampled->GetValue(0));
                            }
                        }
                    }
                }
            }
        }
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

        _renderer->SetSamplesToConvergence(
            renderDelegate->GetRenderSetting<int>(
                HdRenderSettingsTokens->convergedSamplesPerPixel, 1));

        bool enableLighting =
            renderDelegate->GetRenderSetting<bool>(
                HdEmbreeRenderSettingsTokens->enableLighting, false);
        if (enableLighting) {
            _renderer->SetEnableLighting(true);
            _renderer->SetAmbientOcclusionSamples(0);
        } else {
            _renderer->SetEnableLighting(false);
            bool enableAmbientOcclusion =
                renderDelegate->GetRenderSetting<bool>(
                    HdEmbreeRenderSettingsTokens->enableAmbientOcclusion,
                    false);
            if (enableAmbientOcclusion) {
                _renderer->SetAmbientOcclusionSamples(
                    renderDelegate->GetRenderSetting<int>(
                        HdEmbreeRenderSettingsTokens->ambientOcclusionSamples,
                        0));
            } else {
                _renderer->SetAmbientOcclusionSamples(0);
            }
        }

        _renderer->SetDomeLightCameraVisibility(
            renderDelegate->GetRenderSetting<bool>(
                HdRenderSettingsTokens->domeLightCameraVisibility, true));

        _renderer->SetEnableSceneColors(
            renderDelegate->GetRenderSetting<bool>(
                HdEmbreeRenderSettingsTokens->enableSceneColors, true));

        _renderer->SetRandomNumberSeed(
            renderDelegate->GetRenderSetting<unsigned int>(
                HdEmbreeRenderSettingsTokens->randomNumberSeed, (unsigned int)-1));

        const HdEmbreeSamplerSequence defaultSamplerSequence =
            HdEmbreeGetDefaultSamplerSequence(
                HdEmbreeConfig::GetInstance().useSobol);
        const TfToken defaultSamplerSequenceToken =
            HdEmbreeGetSamplerSequenceToken(defaultSamplerSequence);
        TfToken samplerSequenceToken =
            renderDelegate->GetRenderSetting<TfToken>(
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
        if (!HdEmbreeSamplerSequenceIsSupported(samplerSequence)) {
            const HdEmbreeSamplerSequence fallbackSamplerSequence =
                HdEmbreeGetDefaultSamplerSequence(true);
            const TfToken fallbackSamplerSequenceToken =
                HdEmbreeGetSamplerSequenceToken(fallbackSamplerSequence);
            TF_WARN("hdEmbree sampler sequence '%s' requires OpenQMC support; "
                    "falling back to '%s'.",
                    samplerSequenceToken.GetText(),
                    fallbackSamplerSequenceToken.GetText());
            samplerSequence = fallbackSamplerSequence;
        }
        _renderer->SetSamplerSequence(samplerSequence);

        _renderer->SetEnableAdaptiveSampling(
            renderDelegate->GetRenderSetting<bool>(
                HdEmbreeRenderSettingsTokens->enableAdaptiveSampling,
                HdEmbreeConfig::GetInstance().enableAdaptiveSampling));
        _renderer->SetAdaptiveThreshold(
            renderDelegate->GetRenderSetting<float>(
                HdEmbreeRenderSettingsTokens->adaptiveThreshold,
                HdEmbreeConfig::GetInstance().adaptiveThreshold));
        _renderer->SetMinSamplesBeforeAdaptive(
            renderDelegate->GetRenderSetting<int>(
                HdEmbreeRenderSettingsTokens->minSamplesBeforeAdaptive,
                HdEmbreeConfig::GetInstance().minSamplesBeforeAdaptive));
        _renderer->SetMaxBounces(
            renderDelegate->GetRenderSetting<int>(
                HdEmbreeRenderSettingsTokens->maxBounces,
                HdEmbreeDefaultMaxBounces));
        _renderer->SetMinBouncesBeforeRR(
            renderDelegate->GetRenderSetting<int>(
                HdEmbreeRenderSettingsTokens->minBouncesBeforeRR,
                HdEmbreeDefaultMinBouncesBeforeRR));
        _renderer->SetLightSamplesPerHit(
            renderDelegate->GetRenderSetting<int>(
                HdEmbreeRenderSettingsTokens->lightSamplesPerHit,
                HdEmbreeConfig::GetInstance().lightSamplesPerHit));
        _renderer->SetStratifyLightSamples(
            renderDelegate->GetRenderSetting<bool>(
                HdEmbreeRenderSettingsTokens->stratifyLightSamples,
                HdEmbreeConfig::GetInstance().stratifyLightSamples));
        _renderer->SetShowAdaptiveHeatmap(
            renderDelegate->GetRenderSetting<bool>(
                HdEmbreeRenderSettingsTokens->showAdaptiveHeatmap,
                HdEmbreeDefaultShowAdaptiveHeatmap));
        _renderer->SetUsePerChannelVariance(
            renderDelegate->GetRenderSetting<bool>(
                HdEmbreeRenderSettingsTokens->usePerChannelVariance,
                HdEmbreeDefaultUsePerChannelVariance));
        _renderer->SetFireflyClampThreshold(
            renderDelegate->GetRenderSetting<float>(
                HdEmbreeRenderSettingsTokens->fireflyClampThreshold,
                HdEmbreeDefaultFireflyClampThreshold));
        static const TfToken enableGgxMicrofacetMultipleScatteringToken(
            "enableGgxMicrofacetMultipleScattering", TfToken::Immortal);
        _renderer->SetEnableGgxMicrofacetMultipleScattering(
            renderDelegate->GetRenderSetting<bool>(
                enableGgxMicrofacetMultipleScatteringToken,
                true));
        static const TfToken dielectricLayerThroughputModeBsdlToken(
            "bsdl", TfToken::Immortal);
        _renderer->SetDielectricLayerThroughputMode(
            renderDelegate->GetRenderSetting<TfToken>(
                HdEmbreeRenderSettingsTokens->dielectricLayerThroughputMode,
                dielectricLayerThroughputModeBsdlToken));
        _renderer->SetUseAdobeOpenPBR(
            renderDelegate->GetRenderSetting<bool>(
                HdEmbreeRenderSettingsTokens->useAdobeOpenPBR,
                false));

        needStartRender = true;
    }

    // Determine whether we need to update the renderer camera.
    const GfMatrix4d view = renderPassState->GetWorldToViewMatrix();
    const GfMatrix4d proj = renderPassState->GetProjectionMatrix();
    if (_viewMatrix != view || _projMatrix != proj) {
        _viewMatrix = view;
        _projMatrix = proj;

        _renderThread->StopRender();
        _renderer->SetCamera(_viewMatrix, _projMatrix);
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
