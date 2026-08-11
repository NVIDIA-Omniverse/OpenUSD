//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include <delegate/renderBuffer.h>
#include <delegate/renderDelegate.h>
#include <renderer/colorManagement.h>
#include <renderer/renderSettings.h>

#include "pxr/base/arch/fileSystem.h"
#include "pxr/base/arch/systemInfo.h"
#include "pxr/base/gf/color.h"
#include "pxr/base/gf/colorSpace.h"
#include "pxr/base/gf/math.h"
#include "pxr/base/gf/vec2i.h"
#include "pxr/base/plug/registry.h"
#include "pxr/base/tf/stringUtils.h"
#include "pxr/imaging/hd/camera.h"
#include "pxr/imaging/hd/renderIndex.h"
#include "pxr/imaging/hd/renderPass.h"
#include "pxr/imaging/hd/renderPassState.h"
#include "pxr/imaging/hd/renderProductSchema.h"
#include "pxr/imaging/hd/renderSettingsSchema.h"
#include "pxr/imaging/hd/retainedDataSource.h"
#include "pxr/imaging/hd/retainedSceneIndex.h"
#include "pxr/imaging/hd/sceneGlobalsSchema.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/hd/unitTestDelegate.h"
#include "pxr/usd/sdf/schema.h"
#include "pxr/usd/sdf/types.h"
#include "pxr/usd/usd/attribute.h"
#include "pxr/usd/usd/primDefinition.h"
#include "pxr/usd/usd/schemaRegistry.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usd/tokens.h"
#include "pxr/usd/usdRender/settings.h"
#include "pxr/usd/usdRender/spec.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <utility>
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
        HdEmbreeRenderSettingsTokens->convergedSamplesPerPixel,
        HdEmbreeRenderSettingsTokens->randomNumberSeed,
        HdEmbreeRenderSettingsTokens->tileSize,
        HdRenderSettingsTokens->domeLightCameraVisibility,
        HdRenderSettingsTokens->enableExposureCompensation,
        HdEmbreeRenderSettingsTokens->adaptiveThreshold,
        HdEmbreeRenderSettingsTokens->minSamplesBeforeAdaptive,
        HdEmbreeRenderSettingsTokens->maxBounces,
        HdEmbreeRenderSettingsTokens->minBouncesBeforeRR,
        HdEmbreeRenderSettingsTokens->lightSamplesPerHit,
        HdEmbreeRenderSettingsTokens->fireflyClampThreshold,
        HdEmbreeRenderSettingsTokens->enableCaustics,
        HdEmbreeRenderSettingsTokens->causticsClampThreshold,
        HdEmbreeRenderSettingsTokens->disableShadows,
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

    const VtValue exposureCompensationDefault = delegate.GetRenderSetting(
        HdRenderSettingsTokens->enableExposureCompensation);
    if (!exposureCompensationDefault.IsHolding<bool>() ||
        !exposureCompensationDefault.UncheckedGet<bool>()) {
        std::printf("enableExposureCompensation delegate default is not true\n");
        return false;
    }

    if (!delegate.GetRenderSetting(
            TfToken("ty:enableExposureCompensation")).IsEmpty()) {
        std::printf("old ty exposure compensation render setting exists\n");
        return false;
    }

    if (!delegate.GetRenderSetting(
            TfToken("ty:domeLightCameraVisibility")).IsEmpty()) {
        std::printf("old ty dome light render setting exists\n");
        return false;
    }

    if (!delegate.GetRenderSetting(
            TfToken("ty:enableSceneColors")).IsEmpty()) {
        std::printf("removed enableSceneColors render setting exists\n");
        return false;
    }

    for (TfToken const& removedSetting : {
            TfToken("ty:enableAdaptiveSampling"),
            TfToken("ty:showAdaptiveHeatmap")}) {
        if (!delegate.GetRenderSetting(removedSetting).IsEmpty()) {
            std::printf("removed adaptive render setting exists: %s\n",
                        removedSetting.GetText());
            return false;
        }
    }

    const VtValue renderingColorSpaceDefault = delegate.GetRenderSetting(
        HdRenderSettingsPrimTokens->renderingColorSpace);
    if (!renderingColorSpaceDefault.IsHolding<std::string>() ||
        renderingColorSpaceDefault.UncheckedGet<std::string>() !=
            GfColorSpaceNames->LinearRec709.GetString()) {
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
                HdRenderSettingsTokens->enableExposureCompensation ||
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

    const HdAovDescriptor ambientOcclusion =
        delegate.GetDefaultAovDescriptor(ty::AovTokens->ambocc);
    if (ambientOcclusion.format != HdFormatFloat32Vec3 ||
        !ambientOcclusion.multiSampled ||
        ambientOcclusion.clearValue != VtValue(GfVec3f(0.0f))) {
        std::printf("unexpected ambocc AOV descriptor\n");
        return false;
    }

    return true;
}

bool
_TestColorManagementUtilities()
{
    ty::RenderColorSpace parsed =
        ty::RenderColorSpace::LinearRec709;
    if (!ty::ParseRenderColorSpace(
            GfColorSpaceNames->LinearAP1, &parsed) ||
        parsed != ty::RenderColorSpace::LinearAP1 ||
        !ty::ParseRenderColorSpace(
            GfColorSpaceNames->Data, &parsed) ||
        parsed != ty::RenderColorSpace::Data ||
        ty::ParseRenderColorSpace(GfColorSpaceNames->Raw, &parsed) ||
        ty::ParseRenderColorSpace(TfToken("acescg"), &parsed)) {
        std::printf("rendering color-space token parsing failed\n");
        return false;
    }

    const GfVec3f authored(0.25f, 0.5f, 0.75f);
    GfVec3f data = authored;
    if (!ty::ConvertToRenderColorSpace(
            GfColorSpaceNames->SRGBRec709.GetString(),
            ty::RenderColorSpace::Data,
            &data) ||
        !GfIsClose(data, authored, 1.0e-6f)) {
        std::printf("data did not bypass a texture color transform\n");
        return false;
    }

    TfToken resolvedColorSpace;
    if (ty::ResolveColorSpace(
            "acescg", &resolvedColorSpace) !=
            ty::ColorSpaceResolution::Unsupported ||
        ty::ResolveColorSpace(
            "lin_ap1", &resolvedColorSpace) !=
            ty::ColorSpaceResolution::Unsupported ||
        ty::ResolveColorSpace(
            "srgb_texture", &resolvedColorSpace) !=
            ty::ColorSpaceResolution::Unsupported ||
        ty::ResolveColorSpace(
            "LIN_AP1_SCENE", &resolvedColorSpace) !=
            ty::ColorSpaceResolution::Unsupported ||
        ty::ResolveColorSpace(
            GfColorSpaceNames->LinearAP1.GetString(),
            &resolvedColorSpace) !=
            ty::ColorSpaceResolution::Transform ||
        resolvedColorSpace != GfColorSpaceNames->LinearAP1) {
        std::printf("canonical color-space name resolution failed\n");
        return false;
    }

    GfVec3f converted = authored;
    if (!ty::ConvertToRenderColorSpace(
            GfColorSpaceNames->LinearRec709.GetString(),
            ty::RenderColorSpace::LinearAP1,
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

    const GfVec3f ap1Luminance = ty::GetLuminanceCoefficients(
        ty::RenderColorSpace::LinearAP1);
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
_WaitForConvergence(HdRenderPassSharedPtr const& renderPass)
{
    for (int i = 0; i != 500; ++i) {
        if (renderPass->IsConverged()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

bool
_RunRenderProductOutputCase(const char *filename,
                            bool setInteractive,
                            bool interactive,
                            bool invalidateSetup,
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
        HdRenderIndex::New(&delegate, HdDriverVector()));
    if (!renderIndex) {
        std::printf("failed to create render-product test index\n");
        return false;
    }
    renderIndex->InsertSceneIndex(
        sceneIndex, SdfPath::AbsoluteRootPath(), false);

    HdEmbreeRenderBuffer colorBuffer(SdfPath("/ColorBuffer"));
    if (!colorBuffer.Allocate(
            GfVec3i(1, 1, 1), HdFormatFloat32Vec4,
            /*multiSampled=*/false)) {
        std::printf("failed to allocate render-product test buffer\n");
        return false;
    }
    HdEmbreeRenderBuffer invalidBuffer(SdfPath("/InvalidBuffer"));
    if (invalidateSetup &&
        !invalidBuffer.Allocate(
            GfVec3i(1, 1, 1), HdFormatFloat32,
            /*multiSampled=*/false)) {
        std::printf("failed to allocate invalid render-product test buffer\n");
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
    HdRenderPassAovBindingVector aovBindings{colorAov};
    if (invalidateSetup) {
        // Keep color writable so the old product gate would emit a bogus
        // image, while an invalid primId format forces renderer setup failure.
        HdRenderPassAovBinding invalidAov;
        invalidAov.aovName = HdAovTokens->primId;
        invalidAov.renderBuffer = &invalidBuffer;
        invalidAov.clearValue = VtValue(0);
        aovBindings.push_back(invalidAov);
    }
    renderPassState->SetAovBindings(aovBindings);

    renderPass->Execute(renderPassState, TfTokenVector());

    const bool converged = _WaitForConvergence(renderPass);

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
_TestExposureCompensationRenderPassState()
{
    HdEmbreeRenderDelegate delegate;
    delegate.SetRenderSetting(
        HdEmbreeRenderSettingsTokens->convergedSamplesPerPixel, VtValue(1));
    delegate.SetRenderSetting(
        HdEmbreeRenderSettingsTokens->randomNumberSeed, VtValue(1));
    // The render pass must use the pass state, not read this delegate value
    // directly. HdxRenderSetupTask owns the standard delegate-to-state bridge.
    delegate.SetRenderSetting(
        HdRenderSettingsTokens->enableExposureCompensation, VtValue(false));

    std::unique_ptr<HdRenderIndex> renderIndex(
        HdRenderIndex::New(&delegate, HdDriverVector()));
    if (!renderIndex) {
        std::printf("failed to create exposure render index\n");
        return false;
    }

    HdUnitTestDelegate sceneDelegate(
        renderIndex.get(), SdfPath::AbsoluteRootPath());
    const SdfPath cameraId("/Camera");
    sceneDelegate.AddCamera(cameraId);
    sceneDelegate.UpdateCamera(
        cameraId, HdCameraTokens->linearExposureScale, VtValue(2.0f));

    HdSprim* const cameraSprim = renderIndex->GetSprim(
        HdPrimTypeTokens->camera, cameraId);
    if (!cameraSprim) {
        std::printf("failed to create exposure test camera\n");
        return false;
    }
    HdDirtyBits cameraBits = cameraSprim->GetInitialDirtyBitsMask();
    cameraSprim->Sync(
        &sceneDelegate, delegate.GetRenderParam(), &cameraBits);
    HdCamera const* const camera = static_cast<HdCamera const*>(cameraSprim);
    if (camera->GetLinearExposureScale() != 2.0f) {
        std::printf("unexpected exposure test camera scale\n");
        return false;
    }

    HdEmbreeRenderBuffer colorBuffer(SdfPath("/ExposureColorBuffer"));
    if (!colorBuffer.Allocate(
            GfVec3i(1, 1, 1), HdFormatFloat32Vec4,
            /*multiSampled=*/false)) {
        std::printf("failed to allocate exposure test color buffer\n");
        return false;
    }

    HdRenderPassSharedPtr renderPass = delegate.CreateRenderPass(
        renderIndex.get(), HdRprimCollection());
    HdRenderPassStateSharedPtr renderPassState =
        delegate.CreateRenderPassState();
    renderPassState->SetCamera(camera);
    renderPassState->SetViewport(GfVec4d(0.0, 0.0, 1.0, 1.0));

    HdRenderPassAovBinding colorAov;
    colorAov.aovName = HdAovTokens->color;
    colorAov.renderBuffer = &colorBuffer;
    colorAov.clearValue = VtValue(GfVec4f(0.125f, 0.25f, 0.375f, 0.8f));
    renderPassState->SetAovBindings({colorAov});

    const auto renderAndRead = [&]() {
        renderPass->Execute(renderPassState, TfTokenVector());
        if (!_WaitForConvergence(renderPass)) {
            std::printf("exposure render did not converge\n");
            return GfVec4f(-1.0f);
        }
        const float* const data =
            static_cast<float const*>(colorBuffer.Map());
        if (!data) {
            colorBuffer.Unmap();
            std::printf("failed to map exposure test color buffer\n");
            return GfVec4f(-1.0f);
        }
        const GfVec4f color(data[0], data[1], data[2], data[3]);
        colorBuffer.Unmap();
        return color;
    };

    if (!renderPassState->GetEnableExposureCompensation()) {
        std::printf("render-pass-state exposure default is not true\n");
        return false;
    }
    const GfVec4f enabledColor = renderAndRead();
    if (!GfIsClose(
            enabledColor, GfVec4f(0.25f, 0.5f, 0.75f, 1.0f), 1.0e-6f)) {
        std::printf("enabled exposure output was incorrect\n");
        return false;
    }

    renderPassState->SetEnableExposureCompensation(false);
    const GfVec4f disabledColor = renderAndRead();
    if (!GfIsClose(
            disabledColor,
            GfVec4f(0.125f, 0.25f, 0.375f, 1.0f),
            1.0e-6f)) {
        std::printf("disabled exposure output was incorrect\n");
        return false;
    }

    renderPassState->SetEnableExposureCompensation(true);
    const GfVec4f reenabledColor = renderAndRead();
    if (!GfIsClose(reenabledColor, enabledColor, 1.0e-6f)) {
        std::printf("re-enabled exposure output retained stale accumulation\n");
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
    const std::string failedFilename = ArchMakeTmpFileName(
        "testHdEmbreeFailedRenderProduct", ".png");
    return _RunRenderProductOutputCase(
               interactiveFilename.c_str(),
               /*setInteractive=*/false,
               /*interactive=*/true,
               /*invalidateSetup=*/false,
               /*expectOutput=*/false) &&
           _RunRenderProductOutputCase(
               offlineFilename.c_str(),
               /*setInteractive=*/true,
               /*interactive=*/false,
               /*invalidateSetup=*/false,
               /*expectOutput=*/true) &&
           _RunRenderProductOutputCase(
               failedFilename.c_str(),
               /*setInteractive=*/true,
               /*interactive=*/false,
               /*invalidateSetup=*/true,
               /*expectOutput=*/false);
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
    const bool defaultEnableExposureCompensation =
        delegate.GetRenderSetting<bool>(
            HdRenderSettingsTokens->enableExposureCompensation, false);
    const bool authoredEnableExposureCompensation =
        !defaultEnableExposureCompensation;
    const bool defaultDisableShadows =
        delegate.GetRenderSetting<bool>(
            HdEmbreeRenderSettingsTokens->disableShadows, false);
    const bool authoredDisableShadows = !defaultDisableShadows;

    HdRenderSettingsSchema::Builder renderSettingsBuilder;
    renderSettingsBuilder.SetRenderingColorSpace(
        HdRetainedTypedSampledDataSource<TfToken>::New(
            GfColorSpaceNames->LinearAP1));
    const std::array<TfToken, 8> settingNames = {
        HdEmbreeRenderSettingsTokens->maxBounces,
        HdEmbreeRenderSettingsTokens->disableShadows,
        HdEmbreeRenderSettingsTokens->dynamicSubdvTesselation,
        TfToken("ty:domeLightCameraVisibility"),
        HdRenderSettingsTokens->domeLightCameraVisibility,
        TfToken("ty:enableExposureCompensation"),
        HdRenderSettingsTokens->enableExposureCompensation,
        unprefixedMaxBounces
    };
    const std::array<HdDataSourceBaseHandle, 8> settingValues = {
        HdRetainedSampledDataSource::New(VtValue(3)),
        HdRetainedSampledDataSource::New(
            VtValue(authoredDisableShadows)),
        HdRetainedSampledDataSource::New(VtValue(true)),
        HdRetainedSampledDataSource::New(VtValue(true)),
        HdRetainedSampledDataSource::New(
            VtValue(authoredDomeLightCameraVisibility)),
        HdRetainedSampledDataSource::New(VtValue(true)),
        HdRetainedSampledDataSource::New(
            VtValue(authoredEnableExposureCompensation)),
        HdRetainedSampledDataSource::New(VtValue(99))
    };
    renderSettingsBuilder.SetNamespacedSettings(
        HdRetainedContainerDataSource::New(
            settingNames.size(), settingNames.data(), settingValues.data()));

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
        HdRenderIndex::New(&delegate, HdDriverVector()));
    if (!renderIndex) {
        std::printf("failed to create render index\n");
        return false;
    }
    renderIndex->InsertSceneIndex(
        sceneIndex, SdfPath::AbsoluteRootPath(), false);

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

    if (!delegate.GetRenderSetting(
            TfToken("ty:enableExposureCompensation")).IsEmpty()) {
        std::printf("active RenderSettings bridged old ty exposure setting\n");
        return false;
    }

    const bool enableExposureCompensation = delegate.GetRenderSetting<bool>(
        HdRenderSettingsTokens->enableExposureCompensation,
        defaultEnableExposureCompensation);
    if (enableExposureCompensation != authoredEnableExposureCompensation) {
        std::printf("active RenderSettings exposure setting was not bridged\n");
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
        TfToken("ty:convergedSamplesPerPixel"),
        TfToken("ty:randomNumberSeed"),
        TfToken("ty:tileSize"),
        TfToken("ty:adaptiveThreshold"),
        TfToken("ty:minSamplesBeforeAdaptive"),
        TfToken("ty:maxBounces"),
        TfToken("ty:minBouncesBeforeRR"),
        TfToken("ty:lightSamplesPerHit"),
        TfToken("ty:fireflyClampThreshold"),
        TfToken("ty:enableCaustics"),
        TfToken("ty:causticsClampThreshold"),
        TfToken("ty:disableShadows"),
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

TfToken
_GetTokenValue(VtValue const& value)
{
    if (value.IsHolding<TfToken>()) {
        return value.UncheckedGet<TfToken>();
    }
    if (value.IsHolding<std::string>()) {
        return TfToken(value.UncheckedGet<std::string>());
    }
    return TfToken();
}

bool
_TestRenderSettingDefaultParity()
{
    const UsdPrimDefinition* apiDef =
        UsdSchemaRegistry::GetInstance().FindAppliedAPIPrimDefinition(
            TfToken("TyphoonRenderSettingsAPI"));
    if (!apiDef) {
        std::printf("missing schema for render-setting parity test\n");
        return false;
    }

    const ty::RenderSettings defaults;
    const std::vector<std::pair<TfToken, VtValue>> expected = {
        {HdEmbreeRenderSettingsTokens->convergedSamplesPerPixel,
         VtValue(defaults.samplesToConvergence)},
        {HdEmbreeRenderSettingsTokens->randomNumberSeed,
         VtValue(defaults.randomNumberSeed)},
        {HdEmbreeRenderSettingsTokens->tileSize,
         VtValue(defaults.tileSize)},
        {HdEmbreeRenderSettingsTokens->dynamicSubdvTesselation,
         VtValue(ty::DefaultDynamicSubdvTesselation)},
        {HdEmbreeRenderSettingsTokens->adaptiveThreshold,
         VtValue(defaults.adaptiveThreshold)},
        {HdEmbreeRenderSettingsTokens->minSamplesBeforeAdaptive,
         VtValue(defaults.minSamplesBeforeAdaptive)},
        {HdEmbreeRenderSettingsTokens->maxBounces,
         VtValue(defaults.maxBounces)},
        {HdEmbreeRenderSettingsTokens->minBouncesBeforeRR,
         VtValue(defaults.minBouncesBeforeRR)},
        {HdEmbreeRenderSettingsTokens->lightSamplesPerHit,
         VtValue(defaults.lightSamplesPerHit)},
        {HdEmbreeRenderSettingsTokens->fireflyClampThreshold,
         VtValue(defaults.fireflyClampThreshold)},
        {HdEmbreeRenderSettingsTokens->enableCaustics,
         VtValue(defaults.enableCaustics)},
        {HdEmbreeRenderSettingsTokens->causticsClampThreshold,
         VtValue(defaults.causticsClampThreshold)},
        {HdEmbreeRenderSettingsTokens->disableShadows,
         VtValue(defaults.disableShadows)},
        {HdEmbreeRenderSettingsTokens->materialRenderContext,
         VtValue(TfToken(ty::DefaultMaterialRenderContext))},
        {HdEmbreeRenderSettingsTokens->useAdobeOpenPBR,
         VtValue(defaults.useAdobeOpenPBR)},
        {HdEmbreeRenderSettingsTokens->dielectricLayerThroughputMode,
         VtValue(ty::GetDielectricLayerThroughputModeToken(
             defaults.dielectricLayerThroughputMode))},
        {HdEmbreeRenderSettingsTokens->textureCacheSize,
         VtValue(defaults.textureCacheSizeMB)}
    };

    const TfTokenVector schemaProperties = apiDef->GetPropertyNames();
    if (schemaProperties.size() != expected.size()) {
        std::printf("schema/default count mismatch: %zu vs %zu\n",
                    schemaProperties.size(), expected.size());
        return false;
    }

    for (std::pair<TfToken, VtValue> const& entry : expected) {
        VtValue fallback;
        if (!apiDef->GetAttributeFallbackValue(entry.first, &fallback) ||
            fallback != entry.second) {
            std::printf("schema/default mismatch: %s\n",
                        entry.first.GetText());
            return false;
        }
    }

    const HdEmbreeRenderDelegate delegate;
    const HdRenderSettingDescriptorList descriptors =
        delegate.GetRenderSettingDescriptors();
    for (HdRenderSettingDescriptor const& descriptor : descriptors) {
        if (descriptor.key ==
            HdRenderSettingsPrimTokens->renderingColorSpace) {
            continue;
        }
        if (descriptor.key ==
            HdRenderSettingsTokens->domeLightCameraVisibility) {
            if (descriptor.defaultValue !=
                VtValue(defaults.domeLightCameraVisibility)) {
                std::printf("dome descriptor/default mismatch\n");
                return false;
            }
            continue;
        }
        if (descriptor.key ==
            HdRenderSettingsTokens->enableExposureCompensation) {
            if (descriptor.defaultValue != VtValue(true)) {
                std::printf("exposure descriptor default is not true\n");
                return false;
            }
            continue;
        }
        const auto it = std::find_if(
            expected.begin(), expected.end(),
            [&descriptor](std::pair<TfToken, VtValue> const& entry) {
                return entry.first == descriptor.key;
            });
        if (it == expected.end()) {
            std::printf("descriptor has no schema default: %s\n",
                        descriptor.key.GetText());
            return false;
        }
        if (it->second.IsHolding<TfToken>()) {
            if (_GetTokenValue(descriptor.defaultValue) !=
                it->second.UncheckedGet<TfToken>()) {
                std::printf("token descriptor/default mismatch: %s\n",
                            descriptor.key.GetText());
                return false;
            }
        } else if (descriptor.defaultValue != it->second) {
            std::printf("descriptor/default mismatch: %s\n",
                        descriptor.key.GetText());
            return false;
        }
    }

    const TfToken tokenSettings[] = {
        HdEmbreeRenderSettingsTokens->materialRenderContext,
        HdEmbreeRenderSettingsTokens->dielectricLayerThroughputMode
    };
    for (TfToken const& tokenSetting : tokenSettings) {
        VtTokenArray allowedTokens;
        const UsdPrimDefinition::Attribute attribute =
            apiDef->GetAttributeDefinition(tokenSetting);
        VtValue fallback;
        if (!attribute ||
            !attribute.GetMetadata(SdfFieldKeys->AllowedTokens,
                                   &allowedTokens) ||
            !apiDef->GetAttributeFallbackValue(tokenSetting, &fallback) ||
            std::find(
                allowedTokens.begin(), allowedTokens.end(),
                _GetTokenValue(fallback)) == allowedTokens.end()) {
            std::printf("invalid token default or allowedTokens: %s\n",
                        tokenSetting.GetText());
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
    if (!_TestExposureCompensationRenderPassState()) {
        return 1;
    }
    if (!_TestTyphoonRenderSettingsAPI()) {
        return 1;
    }
    if (!_TestRenderSettingDefaultParity()) {
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
