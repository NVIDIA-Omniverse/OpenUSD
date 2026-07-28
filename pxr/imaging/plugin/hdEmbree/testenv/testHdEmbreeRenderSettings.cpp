//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/imaging/plugin/hdEmbree/delegate/renderDelegate.h"
#include "pxr/imaging/plugin/hdEmbree/delegate/renderBuffer.h"
#include "pxr/imaging/plugin/hdEmbree/renderer/colorManagement.h"

#include "pxr/base/gf/colorSpace.h"
#include "pxr/base/gf/color.h"
#include "pxr/base/gf/math.h"
#include "pxr/base/gf/vec2i.h"
#include "pxr/imaging/hd/renderIndex.h"
#include "pxr/imaging/hd/renderPass.h"
#include "pxr/imaging/hd/renderPassState.h"
#include "pxr/imaging/hd/renderSettingsSchema.h"
#include "pxr/imaging/hd/renderProductSchema.h"
#include "pxr/imaging/hd/retainedDataSource.h"
#include "pxr/imaging/hd/retainedSceneIndex.h"
#include "pxr/imaging/hd/sceneGlobalsSchema.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/base/arch/fileSystem.h"
#include "pxr/base/arch/systemInfo.h"
#include "pxr/base/plug/registry.h"
#include "pxr/base/tf/stringUtils.h"
#include "pxr/usd/sdf/types.h"
#include "pxr/usd/usd/attribute.h"
#include "pxr/usd/usd/primDefinition.h"
#include "pxr/usd/usd/schemaRegistry.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usd/tokens.h"
#include "pxr/usd/usdRender/settings.h"
#include "pxr/usd/usdRender/spec.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

bool
_Contains(TfTokenVector const& tokens, TfToken const& token)
{
    return std::find(tokens.begin(), tokens.end(), token) != tokens.end();
}

bool
_HasDescriptor(HdRenderSettingDescriptorList const& descriptors,
               TfToken const& key)
{
    return std::find_if(
        descriptors.begin(), descriptors.end(),
        [&key](HdRenderSettingDescriptor const& descriptor) {
            return descriptor.key == key;
        }) != descriptors.end();
}

template <class T>
bool
_HasSettingValue(VtDictionary const& settings,
                 std::string const& key,
                 T const& expectedValue)
{
    const auto it = settings.find(key);
    if (it == settings.end()) {
        std::printf("missing namespaced setting: %s\n", key.c_str());
        return false;
    }

    if (!it->second.IsHolding<T>()) {
        std::printf("unexpected value type for namespaced setting: %s\n",
                    key.c_str());
        return false;
    }

    if (it->second.UncheckedGet<T>() != expectedValue) {
        std::printf("unexpected value for namespaced setting: %s\n",
                    key.c_str());
        return false;
    }

    return true;
}

bool
_TestRenderDelegateSettings()
{
    HdEmbreeRenderDelegate delegate;

    const TfTokenVector namespaces = delegate.GetRenderSettingsNamespaces();
    const TfTokenVector expectedNamespaces = {
        TfToken("ty"),
        TfToken()
    };
    if (namespaces != expectedNamespaces) {
        std::printf("unexpected render settings namespaces\n");
        return false;
    }

    const HdRenderSettingDescriptorList descriptors =
        delegate.GetRenderSettingDescriptors();
    const TfTokenVector expectedKeys = {
        HdRenderSettingsPrimTokens->renderingColorSpace,
        HdEmbreeRenderSettingsTokens->enableSceneColors,
        HdEmbreeRenderSettingsTokens->enableAmbientOcclusion,
        HdEmbreeRenderSettingsTokens->enableLighting,
        HdEmbreeRenderSettingsTokens->ambientOcclusionSamples,
        HdEmbreeRenderSettingsTokens->convergedSamplesPerPixel,
        HdEmbreeRenderSettingsTokens->randomNumberSeed,
        HdEmbreeRenderSettingsTokens->samplerSequence,
        HdRenderSettingsTokens->domeLightCameraVisibility,
        HdEmbreeRenderSettingsTokens->enableExposureCompensation,
        HdEmbreeRenderSettingsTokens->enableAdaptiveSampling,
        HdEmbreeRenderSettingsTokens->adaptiveThreshold,
        HdEmbreeRenderSettingsTokens->minSamplesBeforeAdaptive,
        HdEmbreeRenderSettingsTokens->maxBounces,
        HdEmbreeRenderSettingsTokens->minBouncesBeforeRR,
        HdEmbreeRenderSettingsTokens->lightSamplesPerHit,
        HdEmbreeRenderSettingsTokens->stratifyLightSamples,
        HdEmbreeRenderSettingsTokens->showAdaptiveHeatmap,
        HdEmbreeRenderSettingsTokens->fireflyClampThreshold,
        HdEmbreeRenderSettingsTokens->enableCaustics,
        HdEmbreeRenderSettingsTokens->causticsClampThreshold,
        HdEmbreeRenderSettingsTokens->approxTransparentShadows,
        HdEmbreeRenderSettingsTokens->disableShadows,
        HdEmbreeRenderSettingsTokens->enableGgxMicrofacetMultipleScattering,
        HdEmbreeRenderSettingsTokens->materialRenderContext,
        HdEmbreeRenderSettingsTokens->useAdobeOpenPBR,
        HdEmbreeRenderSettingsTokens->dielectricLayerThroughputMode,
        HdEmbreeRenderSettingsTokens->dynamicSubdvTesselation,
        HdEmbreeRenderSettingsTokens->textureCacheSize
    };

    if (descriptors.size() != expectedKeys.size()) {
        std::printf("expected %zu descriptors, got %zu\n",
                    expectedKeys.size(), descriptors.size());
        return false;
    }

    for (TfToken const& key : expectedKeys) {
        if (!_HasDescriptor(descriptors, key)) {
            std::printf("missing render setting descriptor: %s\n",
                        key.GetText());
            return false;
        }
    }

    const VtValue dynamicTessellationDefault = delegate.GetRenderSetting(
        HdEmbreeRenderSettingsTokens->dynamicSubdvTesselation);
    if (!dynamicTessellationDefault.IsHolding<bool>() ||
        dynamicTessellationDefault.UncheckedGet<bool>()) {
        std::printf("dynamicSubdvTesselation delegate default is not false\n");
        return false;
    }

    if (!delegate.GetRenderSetting(
            TfToken("ty:domeLightCameraVisibility")).IsEmpty()) {
        std::printf("old ty dome light render setting exists\n");
        return false;
    }

    const VtValue renderingColorSpaceDefault = delegate.GetRenderSetting(
        HdRenderSettingsPrimTokens->renderingColorSpace);
    if (!renderingColorSpaceDefault.IsHolding<TfToken>() ||
        renderingColorSpaceDefault.UncheckedGet<TfToken>() !=
            GfColorSpaceNames->LinearRec709) {
        std::printf("renderingColorSpace default is not lin_rec709_scene\n");
        return false;
    }

    for (HdRenderSettingDescriptor const& descriptor : descriptors) {
        const std::string key = descriptor.key.GetString();
        if (delegate.GetRenderSetting(descriptor.key) !=
            descriptor.defaultValue) {
            std::printf("default setting missing for descriptor: %s\n",
                        descriptor.key.GetText());
            return false;
        }

        if (descriptor.key ==
                HdRenderSettingsTokens->domeLightCameraVisibility ||
            descriptor.key ==
                HdRenderSettingsPrimTokens->renderingColorSpace) {
            continue;
        }

        if (key.compare(0, 3, "ty:") != 0) {
            std::printf("non-ty render setting descriptor: %s\n",
                        descriptor.key.GetText());
            return false;
        }

        const std::string unprefixedName = key.substr(3);
        const TfToken unprefixedKey(unprefixedName);
        if (!delegate.GetRenderSetting(unprefixedKey).IsEmpty()) {
            std::printf("unprefixed render setting exists: %s\n",
                        unprefixedKey.GetText());
            return false;
        }

        const TfToken oldLongNamespaceKey(
            std::string("typhoon") + ":" + unprefixedName);
        if (!delegate.GetRenderSetting(oldLongNamespaceKey).IsEmpty()) {
            std::printf("old typhoon render setting exists: %s\n",
                        oldLongNamespaceKey.GetText());
            return false;
        }

        const TfToken oldHdEmbreeNamespaceKey(
            std::string("hdEmbree") + ":" + unprefixedName);
        if (!delegate.GetRenderSetting(oldHdEmbreeNamespaceKey).IsEmpty()) {
            std::printf("old hdEmbree render setting exists: %s\n",
                        oldHdEmbreeNamespaceKey.GetText());
            return false;
        }
    }

    return true;
}

bool
_TestColorManagementUtilities()
{
    HdEmbreeRenderColorSpace parsed =
        HdEmbreeRenderColorSpace::LinearRec709;
    if (!HdEmbreeParseRenderColorSpace(
            GfColorSpaceNames->LinearAP1, &parsed) ||
        parsed != HdEmbreeRenderColorSpace::LinearAP1 ||
        !HdEmbreeParseRenderColorSpace(
            GfColorSpaceNames->Raw, &parsed) ||
        parsed != HdEmbreeRenderColorSpace::Raw ||
        HdEmbreeParseRenderColorSpace(TfToken("acescg"), &parsed)) {
        std::printf("rendering color-space token parsing failed\n");
        return false;
    }

    const GfVec3f authored(0.25f, 0.5f, 0.75f);
    GfVec3f raw = authored;
    if (!HdEmbreeConvertToRenderColorSpace(
            GfColorSpaceNames->SRGBRec709.GetString(),
            HdEmbreeRenderColorSpace::Raw,
            &raw) ||
        !GfIsClose(raw, authored, 1.0e-6f)) {
        std::printf("raw did not bypass a texture color transform\n");
        return false;
    }

    GfVec3f converted = authored;
    if (!HdEmbreeConvertToRenderColorSpace(
            GfColorSpaceNames->LinearRec709.GetString(),
            HdEmbreeRenderColorSpace::LinearAP1,
            &converted)) {
        std::printf("Linear Rec.709 to AP1 conversion failed\n");
        return false;
    }
    const GfVec3f expected = GfColorSpace(
        GfColorSpaceNames->LinearAP1).Convert(
            GfColorSpace(GfColorSpaceNames->LinearRec709), authored).GetRGB();
    if (!GfIsClose(converted, expected, 1.0e-6f)) {
        std::printf("Linear Rec.709 to AP1 conversion was incorrect\n");
        return false;
    }

    const GfVec3f ap1Luminance = HdEmbreeGetLuminanceCoefficients(
        HdEmbreeRenderColorSpace::LinearAP1);
    // Gf's lin_ap1_scene primaries are Bradford-preadapted to D65.
    if (!GfIsClose(
            ap1Luminance,
            GfVec3f(0.26767218f, 0.67433995f, 0.05798787f),
            1.0e-5f)) {
        std::printf(
            "unexpected AP1 luminance coefficients: (%f, %f, %f)\n",
            ap1Luminance[0], ap1Luminance[1], ap1Luminance[2]);
        return false;
    }

    return true;
}

bool
_FileExists(const char *filename)
{
    if (FILE *file = std::fopen(filename, "rb")) {
        std::fclose(file);
        return true;
    }
    return false;
}

bool
_RemoveIfExists(const char *filename)
{
    if (!_FileExists(filename)) {
        return true;
    }
    if (std::remove(filename) == 0) {
        return true;
    }
    std::printf("failed to remove temporary RenderProduct: %s\n", filename);
    return false;
}

bool
_RunRenderProductOutputCase(const char *filename,
                            bool setInteractive,
                            bool interactive,
                            bool expectOutput)
{
    if (!_RemoveIfExists(filename)) {
        return false;
    }

    const SdfPath renderSettingsPath("/RenderSettings");
    HdRetainedSceneIndexRefPtr sceneIndex = HdRetainedSceneIndex::New();

    HdSceneGlobalsSchema::Builder sceneGlobalsBuilder;
    sceneGlobalsBuilder.SetActiveRenderSettingsPrim(
        HdRetainedTypedSampledDataSource<SdfPath>::New(renderSettingsPath));

    HdDataSourceBaseHandle products[] = {
        HdRenderProductSchema::Builder()
            .SetPath(HdRetainedTypedSampledDataSource<SdfPath>::New(
                SdfPath("/RenderProduct")))
            .SetType(HdRetainedTypedSampledDataSource<TfToken>::New(
                TfToken("raster")))
            .SetName(HdRetainedTypedSampledDataSource<TfToken>::New(
                TfToken(filename)))
            .SetResolution(HdRetainedTypedSampledDataSource<GfVec2i>::New(
                GfVec2i(1, 1)))
            .Build()
    };
    HdRenderSettingsSchema::Builder renderSettingsBuilder;
    renderSettingsBuilder.SetRenderProducts(
        HdRetainedSmallVectorDataSource::New(1, products));

    sceneIndex->AddPrims({
        {SdfPath::AbsoluteRootPath(), TfToken(),
         HdRetainedContainerDataSource::New(
             HdSceneGlobalsSchema::GetSchemaToken(),
             sceneGlobalsBuilder.Build())},
        {renderSettingsPath, HdPrimTypeTokens->renderSettings,
         HdRetainedContainerDataSource::New(
             HdRenderSettingsSchema::GetSchemaToken(),
             renderSettingsBuilder.Build())}
    });

    HdEmbreeRenderDelegate delegate;
    delegate.SetRenderSetting(
        HdEmbreeRenderSettingsTokens->convergedSamplesPerPixel, VtValue(1));
    if (setInteractive) {
        delegate.SetRenderSetting(
            HdRenderSettingsTokens->enableInteractive,
            VtValue(interactive));
    }

    std::unique_ptr<HdRenderIndex> renderIndex(
        HdRenderIndex::NewForBackendEmulation(
            &delegate, HdDriverVector(), sceneIndex));
    if (!renderIndex) {
        std::printf("failed to create render-product test index\n");
        return false;
    }

    HdEmbreeRenderBuffer colorBuffer(SdfPath("/ColorBuffer"));
    if (!colorBuffer.Allocate(
            GfVec3i(1, 1, 1), HdFormatFloat32Vec4,
            /*multiSampled=*/false)) {
        std::printf("failed to allocate render-product test buffer\n");
        return false;
    }

    HdRenderPassSharedPtr renderPass = delegate.CreateRenderPass(
        renderIndex.get(), HdRprimCollection());
    HdRenderPassStateSharedPtr renderPassState =
        delegate.CreateRenderPassState();
    renderPassState->SetViewport(GfVec4d(0.0, 0.0, 1.0, 1.0));

    HdRenderPassAovBinding colorAov;
    colorAov.aovName = HdAovTokens->color;
    colorAov.renderBuffer = &colorBuffer;
    colorAov.clearValue = VtValue(GfVec4f(0.0f));
    renderPassState->SetAovBindings({colorAov});

    renderPass->Execute(renderPassState, TfTokenVector());

    bool converged = false;
    for (int i = 0; i != 500; ++i) {
        if (renderPass->IsConverged()) {
            converged = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    const bool outputExists = _FileExists(filename);
    if (!_RemoveIfExists(filename)) {
        return false;
    }

    if (!converged) {
        std::printf("render-product test did not converge\n");
        return false;
    }
    if (outputExists != expectOutput) {
        std::printf("RenderProduct output existence mismatch for %s\n",
                    filename);
        return false;
    }

    return true;
}

bool
_TestRenderProductOutputPolicy()
{
    const std::string interactiveFilename = ArchMakeTmpFileName(
        "testHdEmbreeInteractiveRenderProduct", ".png");
    const std::string offlineFilename = ArchMakeTmpFileName(
        "testHdEmbreeOfflineRenderProduct", ".png");
    return _RunRenderProductOutputCase(
               interactiveFilename.c_str(),
               /*setInteractive=*/false,
               /*interactive=*/true,
               /*expectOutput=*/false) &&
           _RunRenderProductOutputCase(
               offlineFilename.c_str(),
               /*setInteractive=*/true,
               /*interactive=*/false,
               /*expectOutput=*/true);
}

bool
_TestAuthoredNamespacedSettings()
{
    UsdStageRefPtr stage = UsdStage::CreateInMemory();
    UsdRenderSettings settings =
        UsdRenderSettings::Define(stage, SdfPath("/RenderSettings"));

    if (!settings.GetResolutionAttr().Set(GfVec2i(64, 64))) {
        std::printf("failed to author built-in resolution\n");
        return false;
    }

    UsdAttribute maxBouncesAttr =
        settings.GetPrim().GetAttribute(TfToken("ty:maxBounces"));
    if (!maxBouncesAttr || !maxBouncesAttr.Set(8)) {
        std::printf("failed to author ty:maxBounces\n");
        return false;
    }

    UsdAttribute dynamicTessellationAttr = settings.GetPrim().GetAttribute(
        TfToken("ty:dynamicSubdvTesselation"));
    if (!dynamicTessellationAttr || !dynamicTessellationAttr.Set(true)) {
        std::printf("failed to author ty:dynamicSubdvTesselation\n");
        return false;
    }

    UsdAttribute disableShadowsAttr =
        settings.GetPrim().GetAttribute(TfToken("ty:disableShadows"));
    if (!disableShadowsAttr || !disableShadowsAttr.Set(true)) {
        std::printf("failed to author ty:disableShadows\n");
        return false;
    }

    UsdAttribute domeVisibilityAttr = settings.GetPrim().CreateAttribute(
        HdRenderSettingsTokens->domeLightCameraVisibility,
        SdfValueTypeNames->Bool);
    if (!domeVisibilityAttr || !domeVisibilityAttr.Set(false)) {
        std::printf("failed to author domeLightCameraVisibility\n");
        return false;
    }

    const VtDictionary namespacedSettings =
        UsdRenderComputeNamespacedSettings(
            settings.GetPrim(), {TfToken("ty")});
    if (!_HasSettingValue<int>(
            namespacedSettings, "ty:maxBounces", 8) ||
        !_HasSettingValue<bool>(
            namespacedSettings, "ty:disableShadows", true) ||
        !_HasSettingValue<bool>(
            namespacedSettings, "ty:dynamicSubdvTesselation", true)) {
        return false;
    }

    if (namespacedSettings.find("domeLightCameraVisibility") !=
        namespacedSettings.end()) {
        std::printf("ty namespace request included generic dome setting\n");
        return false;
    }

    for (const auto& entry : namespacedSettings) {
        if (entry.first.compare(0, 3, "ty:") != 0) {
            std::printf("non-ty authored render setting: %s\n",
                        entry.first.c_str());
            return false;
        }
    }

    const VtDictionary allCustomSettings =
        UsdRenderComputeNamespacedSettings(
            settings.GetPrim(), TfTokenVector());
    if (!_HasSettingValue<int>(
            allCustomSettings, "ty:maxBounces", 8) ||
        !_HasSettingValue<bool>(
            allCustomSettings, "ty:disableShadows", true) ||
        !_HasSettingValue<bool>(
            allCustomSettings, "ty:dynamicSubdvTesselation", true)) {
        return false;
    }
    // An empty namespace request means all namespaced custom settings, not
    // unnamespaced Hydra settings. The explicit request below covers the
    // generic dome-light key.
    if (allCustomSettings.find("domeLightCameraVisibility") !=
            allCustomSettings.end()) {
        std::printf("all namespaced settings included generic dome setting\n");
        return false;
    }

    if (allCustomSettings.find("resolution") != allCustomSettings.end()) {
        std::printf("all custom settings included built-in resolution\n");
        return false;
    }

    const VtDictionary requestedSettings =
        UsdRenderComputeNamespacedSettings(
            settings.GetPrim(), {TfToken("ty"), TfToken()});
    if (!_HasSettingValue<int>(
            requestedSettings, "ty:maxBounces", 8) ||
        !_HasSettingValue<bool>(
            requestedSettings, "ty:disableShadows", true) ||
        !_HasSettingValue<bool>(
            requestedSettings, "ty:dynamicSubdvTesselation", true)) {
        return false;
    }
    if (requestedSettings.find("domeLightCameraVisibility") !=
            requestedSettings.end()) {
        std::printf("namespace request included generic dome setting\n");
        return false;
    }

    if (requestedSettings.find("resolution") != requestedSettings.end()) {
        std::printf("requested settings included built-in resolution\n");
        return false;
    }

    return true;
}

bool
_TestActiveRenderSettingsPrimBridge()
{
    const SdfPath renderSettingsPath("/RenderSettings");
    HdRetainedSceneIndexRefPtr sceneIndex = HdRetainedSceneIndex::New();

    HdSceneGlobalsSchema::Builder sceneGlobalsBuilder;
    sceneGlobalsBuilder.SetActiveRenderSettingsPrim(
        HdRetainedTypedSampledDataSource<SdfPath>::New(renderSettingsPath));

    const TfToken unprefixedMaxBounces(
        HdEmbreeRenderSettingsTokens->maxBounces.GetString().substr(3));

    HdEmbreeRenderDelegate delegate;
    const bool defaultDomeLightCameraVisibility =
        delegate.GetRenderSetting<bool>(
            HdRenderSettingsTokens->domeLightCameraVisibility, false);
    const bool authoredDomeLightCameraVisibility =
        !defaultDomeLightCameraVisibility;
    const bool defaultDisableShadows =
        delegate.GetRenderSetting<bool>(
            HdEmbreeRenderSettingsTokens->disableShadows, false);
    const bool authoredDisableShadows = !defaultDisableShadows;

    HdRenderSettingsSchema::Builder renderSettingsBuilder;
    renderSettingsBuilder.SetRenderingColorSpace(
        HdRetainedTypedSampledDataSource<TfToken>::New(
            GfColorSpaceNames->LinearAP1));
    renderSettingsBuilder.SetNamespacedSettings(
        HdRetainedContainerDataSource::New(
            HdEmbreeRenderSettingsTokens->maxBounces,
            HdRetainedSampledDataSource::New(VtValue(3)),
            HdEmbreeRenderSettingsTokens->disableShadows,
            HdRetainedSampledDataSource::New(
                VtValue(authoredDisableShadows)),
            HdEmbreeRenderSettingsTokens->dynamicSubdvTesselation,
            HdRetainedSampledDataSource::New(VtValue(true)),
            TfToken("ty:domeLightCameraVisibility"),
            HdRetainedSampledDataSource::New(VtValue(true)),
            HdRenderSettingsTokens->domeLightCameraVisibility,
            HdRetainedSampledDataSource::New(
                VtValue(authoredDomeLightCameraVisibility)),
            unprefixedMaxBounces,
            HdRetainedSampledDataSource::New(VtValue(99))));

    sceneIndex->AddPrims({
        {SdfPath::AbsoluteRootPath(), TfToken(),
         HdRetainedContainerDataSource::New(
             HdSceneGlobalsSchema::GetSchemaToken(),
             sceneGlobalsBuilder.Build())},
        {renderSettingsPath, HdPrimTypeTokens->renderSettings,
         HdRetainedContainerDataSource::New(
             HdRenderSettingsSchema::GetSchemaToken(),
             renderSettingsBuilder.Build())}
    });

    std::unique_ptr<HdRenderIndex> renderIndex(
        HdRenderIndex::NewForBackendEmulation(
            &delegate, HdDriverVector(), sceneIndex));
    if (!renderIndex) {
        std::printf("failed to create render index\n");
        return false;
    }

    HdEmbreeRenderBuffer colorBuffer(SdfPath("/ColorBuffer"));
    if (!colorBuffer.Allocate(
            GfVec3i(1, 1, 1), HdFormatFloat32Vec4,
            /*multiSampled=*/false)) {
        std::printf("failed to allocate test color buffer\n");
        return false;
    }

    HdRenderPassSharedPtr renderPass = delegate.CreateRenderPass(
        renderIndex.get(), HdRprimCollection());
    HdRenderPassStateSharedPtr renderPassState =
        delegate.CreateRenderPassState();
    renderPassState->SetViewport(GfVec4d(0.0, 0.0, 1.0, 1.0));

    HdRenderPassAovBinding colorAov;
    colorAov.aovName = HdAovTokens->color;
    colorAov.renderBuffer = &colorBuffer;
    colorAov.clearValue = VtValue(GfVec4f(0.0f));
    renderPassState->SetAovBindings({colorAov});

    renderPass->Execute(renderPassState, TfTokenVector());

    const int maxBounces = delegate.GetRenderSetting<int>(
        HdEmbreeRenderSettingsTokens->maxBounces, 0);
    if (maxBounces != 3) {
        std::printf("active RenderSettings ty:maxBounces was not bridged: %d\n",
                    maxBounces);
        return false;
    }

    const TfToken renderingColorSpace = delegate.GetRenderSetting<TfToken>(
        HdRenderSettingsPrimTokens->renderingColorSpace, TfToken());
    if (renderingColorSpace != GfColorSpaceNames->LinearAP1) {
        std::printf(
            "active RenderSettings renderingColorSpace was not bridged\n");
        return false;
    }

    const bool disableShadows = delegate.GetRenderSetting<bool>(
        HdEmbreeRenderSettingsTokens->disableShadows, defaultDisableShadows);
    if (disableShadows != authoredDisableShadows) {
        std::printf("active RenderSettings ty:disableShadows was not bridged\n");
        return false;
    }

    if (!delegate.GetRenderSetting<bool>(
            HdEmbreeRenderSettingsTokens->dynamicSubdvTesselation, false)) {
        std::printf("active RenderSettings ty:dynamicSubdvTesselation was not bridged\n");
        return false;
    }

    if (!delegate.GetRenderSetting(unprefixedMaxBounces).IsEmpty()) {
        std::printf("active RenderSettings created an unprefixed maxBounces\n");
        return false;
    }

    if (!delegate.GetRenderSetting(
            TfToken("ty:domeLightCameraVisibility")).IsEmpty()) {
        std::printf("active RenderSettings bridged old ty dome setting\n");
        return false;
    }

    const bool domeLightCameraVisibility = delegate.GetRenderSetting<bool>(
        HdRenderSettingsTokens->domeLightCameraVisibility,
        defaultDomeLightCameraVisibility);
    if (domeLightCameraVisibility != authoredDomeLightCameraVisibility) {
        std::printf("active RenderSettings domeLightCameraVisibility was not bridged\n");
        return false;
    }

    return true;
}

bool
_TestTyphoonRenderSettingsAPI()
{
    PlugRegistry::GetInstance().RegisterPlugins(
        TfGetPathName(ArchGetExecutablePath()));
    if (!PlugRegistry::GetInstance().GetPluginWithName("hdEmbree")) {
        std::printf("hdEmbree plugInfo was not registered\n");
        return false;
    }

    const TfToken apiName("TyphoonRenderSettingsAPI");
    const UsdPrimDefinition* apiDef =
        UsdSchemaRegistry::GetInstance().FindAppliedAPIPrimDefinition(apiName);
    if (!apiDef) {
        std::printf("missing TyphoonRenderSettingsAPI prim definition\n");
        return false;
    }

    const TfTokenVector expectedProperties = {
        TfToken("ty:enableSceneColors"),
        TfToken("ty:enableAmbientOcclusion"),
        TfToken("ty:enableLighting"),
        TfToken("ty:ambientOcclusionSamples"),
        TfToken("ty:convergedSamplesPerPixel"),
        TfToken("ty:randomNumberSeed"),
        TfToken("ty:samplerSequence"),
        TfToken("ty:enableExposureCompensation"),
        TfToken("ty:enableAdaptiveSampling"),
        TfToken("ty:adaptiveThreshold"),
        TfToken("ty:minSamplesBeforeAdaptive"),
        TfToken("ty:maxBounces"),
        TfToken("ty:minBouncesBeforeRR"),
        TfToken("ty:lightSamplesPerHit"),
        TfToken("ty:stratifyLightSamples"),
        TfToken("ty:showAdaptiveHeatmap"),
        TfToken("ty:fireflyClampThreshold"),
        TfToken("ty:enableCaustics"),
        TfToken("ty:causticsClampThreshold"),
        TfToken("ty:approxTransparentShadows"),
        TfToken("ty:disableShadows"),
        TfToken("ty:enableGgxMicrofacetMultipleScattering"),
        TfToken("ty:materialRenderContext"),
        TfToken("ty:useAdobeOpenPBR"),
        TfToken("ty:dielectricLayerThroughputMode"),
        TfToken("ty:dynamicSubdvTesselation"),
        TfToken("ty:textureCacheSize")
    };

    const TfTokenVector apiProperties = apiDef->GetPropertyNames();
    if (apiProperties.size() != expectedProperties.size()) {
        std::printf("expected %zu schema properties, got %zu\n",
                    expectedProperties.size(), apiProperties.size());
        return false;
    }

    for (TfToken const& property : expectedProperties) {
        if (!_Contains(apiProperties, property)) {
            std::printf("missing schema property: %s\n", property.GetText());
            return false;
        }
    }

    for (TfToken const& property : apiProperties) {
        const std::string name = property.GetString();
        if (name.compare(0, 3, "ty:") != 0) {
            std::printf("non-ty schema property: %s\n", property.GetText());
            return false;
        }
    }

    TfToken samplerFallback;
    if (!apiDef->GetAttributeDefinition(TfToken("ty:samplerSequence"))
            .GetFallbackValue(&samplerFallback) ||
        samplerFallback != TfToken("openqmc_sobolbn")) {
        std::printf("unexpected samplerSequence fallback\n");
        return false;
    }

    bool dynamicTessellationFallback = true;
    if (!apiDef->GetAttributeDefinition(
            TfToken("ty:dynamicSubdvTesselation"))
            .GetFallbackValue(&dynamicTessellationFallback) ||
        dynamicTessellationFallback != false) {
        std::printf("unexpected dynamicSubdvTesselation fallback\n");
        return false;
    }

    bool disableShadowsFallback = true;
    if (!apiDef->GetAttributeDefinition(TfToken("ty:disableShadows"))
            .GetFallbackValue(&disableShadowsFallback) ||
        disableShadowsFallback != false) {
        std::printf("unexpected disableShadows fallback\n");
        return false;
    }

    const auto& autoApply = UsdSchemaRegistry::GetAutoApplyAPISchemas();
    auto autoApplyIt = autoApply.find(apiName);
    if (autoApplyIt == autoApply.end() ||
        !_Contains(autoApplyIt->second, TfToken("RenderSettings"))) {
        std::printf("TyphoonRenderSettingsAPI is not auto-applied to RenderSettings\n");
        return false;
    }

    const TfTokenVector canApply =
        UsdSchemaRegistry::GetAPISchemaCanOnlyApplyToTypeNames(apiName);
    if (!_Contains(canApply, TfToken("RenderSettings"))) {
        std::printf("TyphoonRenderSettingsAPI is not constrained to RenderSettings\n");
        return false;
    }

    const UsdPrimDefinition* renderSettingsDef =
        UsdSchemaRegistry::GetInstance().FindConcretePrimDefinition(
            TfToken("RenderSettings"));
    if (!renderSettingsDef ||
        !_Contains(renderSettingsDef->GetAppliedAPISchemas(), apiName)) {
        std::printf("RenderSettings does not auto-apply Typhoon settings\n");
        return false;
    }

    const TfTokenVector renderSettingsProperties =
        renderSettingsDef->GetPropertyNames();
    for (TfToken const& property : expectedProperties) {
        if (!_Contains(renderSettingsProperties, property)) {
            std::printf("RenderSettings missing auto-applied property: %s\n",
                        property.GetText());
            return false;
        }
    }

    for (TfToken const& property : renderSettingsProperties) {
        const std::string name = property.GetString();
        if (name.find(':') != std::string::npos &&
            name.compare(0, 3, "ty:") != 0) {
            std::printf("RenderSettings has non-ty auto-applied property: %s\n",
                        property.GetText());
            return false;
        }
    }

    return true;
}

}

int
main()
{
    if (!_TestColorManagementUtilities()) {
        return 1;
    }
    if (!_TestRenderDelegateSettings()) {
        return 1;
    }
    if (!_TestRenderProductOutputPolicy()) {
        return 1;
    }
    if (!_TestTyphoonRenderSettingsAPI()) {
        return 1;
    }
    if (!_TestActiveRenderSettingsPrimBridge()) {
        return 1;
    }
    if (!_TestAuthoredNamespacedSettings()) {
        return 1;
    }
    return 0;
}
