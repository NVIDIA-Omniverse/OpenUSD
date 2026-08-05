//
// Copyright 2016 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "renderPass.h"
#include "material.h"
#include "renderDelegate.h"

#include <renderer/colorManagement.h>
#include <renderer/renderSettings.h>

#include "pxr/base/gf/colorSpace.h"
#include "pxr/base/gf/half.h"
#include "pxr/base/tf/diagnostic.h"
#include "pxr/base/tf/enum.h"
#include "pxr/imaging/hd/camera.h"
#include "pxr/imaging/hd/changeTracker.h"
#include "pxr/imaging/hd/material.h"
#include "pxr/imaging/hd/renderPassState.h"
#include "pxr/imaging/hd/renderProductSchema.h"
#include "pxr/imaging/hd/renderSettingsSchema.h"
#include "pxr/imaging/hd/renderVarSchema.h"
#include "pxr/imaging/hd/sceneGlobalsSchema.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/hd/types.h"
#include "pxr/imaging/hd/utils.h"
#include "pxr/imaging/hio/image.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

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

        if (name == TfToken("ty:domeLightCameraVisibility")) {
            continue;
        }

        if (HdDataSourceBaseHandle ds = container->Get(name)) {
            if (HdSampledDataSourceHandle sampled =
                    HdSampledDataSource::Cast(ds)) {
                VtValue value = sampled->GetValue(0);
                if (!value.IsEmpty()) {
                    (*renderSettings)[name] = value;
                }
            }
        }
    }
}

static void
_AddRenderSetting(
    HdSampledDataSourceContainerSchema const &namespacedSettings,
    TfToken const &key,
    _RenderSettingsMap *renderSettings)
{
    HdContainerDataSourceHandle container = namespacedSettings.GetContainer();
    if (!container) {
        return;
    }

    if (HdDataSourceBaseHandle ds = container->Get(key)) {
        if (HdSampledDataSourceHandle sampled =
                HdSampledDataSource::Cast(ds)) {
            VtValue value = sampled->GetValue(0);
            if (!value.IsEmpty()) {
                (*renderSettings)[key] = value;
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
    _AddRenderSetting(
        namespacedSettings,
        HdRenderSettingsTokens->domeLightCameraVisibility,
        &renderSettings);
    if (HdTokenDataSourceHandle colorSpace =
            rsSchema.GetRenderingColorSpace()) {
        const TfToken value = colorSpace->GetTypedValue(0);
        if (!value.IsEmpty()) {
            renderSettings[HdRenderSettingsPrimTokens->renderingColorSpace] =
                VtValue(value);
        }
    }
    return renderSettings;
}

static bool
_RenderSettingsEqual(
    _RenderSettingsMap const &settingsA,
    _RenderSettingsMap const &settingsB)
{
    if (settingsA.size() != settingsB.size()) {
        return false;
    }

    for (const auto &entry : settingsA) {
        const auto it = settingsB.find(entry.first);
        if (it == settingsB.end() || it->second != entry.second) {
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

static bool
_RenderVarRequestsColor(HdRenderVarSchema varSchema)
{
    TfToken sourceName;
    if (HdTokenDataSourceHandle handle = varSchema.GetSourceName()) {
        sourceName = handle->GetTypedValue(0);
    }

    if (sourceName != HdAovTokens->color && sourceName != TfToken("Ci")) {
        return false;
    }

    if (HdTokenDataSourceHandle handle = varSchema.GetSourceType()) {
        const TfToken sourceType = handle->GetTypedValue(0);
        if (!sourceType.IsEmpty() && sourceType != TfToken("raw")) {
            return false;
        }
    }

    return true;
}

static bool
_RenderProductRequestsColor(HdRenderProductSchema productSchema)
{
    HdRenderVarVectorSchema varsSchema = productSchema.GetRenderVars();
    if (!varsSchema || varsSchema.GetNumElements() == 0) {
        return true;
    }

    for (size_t i = 0; i < varsSchema.GetNumElements(); ++i) {
        if (HdRenderVarSchema varSchema = varsSchema.GetElement(i)) {
            if (_RenderVarRequestsColor(varSchema)) {
                return true;
            }
        }
    }

    return false;
}

static HdRenderBuffer *
_GetColorRenderBuffer(HdRenderPassAovBindingVector const &aovBindings,
                      HdEmbreeRenderBuffer *fallback)
{
    for (HdRenderPassAovBinding const &binding : aovBindings) {
        if (binding.aovName == HdAovTokens->color && binding.renderBuffer) {
            return binding.renderBuffer;
        }
    }

    if (aovBindings.empty() && fallback &&
        fallback->GetWidth() > 0 && fallback->GetHeight() > 0) {
        return fallback;
    }

    return nullptr;
}

static bool
_ReadFloatComponent(HdFormat format, const uint8_t *src,
                    size_t component, float *value)
{
    const size_t componentCount = HdGetComponentCount(format);
    if (component >= componentCount) {
        *value = component == 3 ? 1.0f : 0.0f;
        return true;
    }

    const HdFormat componentFormat = HdGetComponentFormat(format);
    if (componentFormat == HdFormatUNorm8) {
        *value = reinterpret_cast<const uint8_t *>(src)[component] / 255.0f;
        return true;
    }
    if (componentFormat == HdFormatSNorm8) {
        *value = std::max(
            reinterpret_cast<const int8_t *>(src)[component] / 127.0f,
            -1.0f);
        return true;
    }
    if (componentFormat == HdFormatFloat16) {
        GfHalf half;
        half.setBits(reinterpret_cast<const uint16_t *>(src)[component]);
        *value = static_cast<float>(half);
        return true;
    }
    if (componentFormat == HdFormatFloat32) {
        *value = reinterpret_cast<const float *>(src)[component];
        return true;
    }

    return false;
}

static bool
_CopyRenderBufferToFloatRgba(HdRenderBuffer *renderBuffer,
                             std::vector<float> *pixels)
{
    const HdFormat format = renderBuffer->GetFormat();
    const size_t pixelSize = HdDataSizeOfFormat(format);
    if (pixelSize == 0) {
        TF_WARN("Cannot write RenderProduct from unsupported color buffer "
                "format '%s'", TfEnum::GetName(format).c_str());
        return false;
    }

    renderBuffer->Resolve();

    const void *mapped = renderBuffer->Map();
    if (!mapped) {
        TF_WARN("Cannot write RenderProduct; failed to map color buffer");
        return false;
    }

    const unsigned int width = renderBuffer->GetWidth();
    const unsigned int height = renderBuffer->GetHeight();
    pixels->assign(static_cast<size_t>(width) * height * 4, 0.0f);

    const uint8_t *src = static_cast<const uint8_t *>(mapped);
    bool success = true;
    for (size_t pixel = 0; pixel < static_cast<size_t>(width) * height;
         ++pixel) {
        const uint8_t *srcPixel = src + pixel * pixelSize;
        for (size_t component = 0; component < 4; ++component) {
            float value = 0.0f;
            if (!_ReadFloatComponent(
                    format, srcPixel, component, &value)) {
                TF_WARN("Cannot write RenderProduct from unsupported color "
                        "buffer format '%s'", TfEnum::GetName(format).c_str());
                success = false;
                break;
            }
            (*pixels)[pixel * 4 + component] = value;
        }
        if (!success) {
            break;
        }
    }

    renderBuffer->Unmap();
    return success;
}

static bool
_WriteFloatRgbaImage(const std::string &filename,
                     unsigned int width,
                     unsigned int height,
                     std::vector<float> *pixels)
{
    HioImage::StorageSpec storage;
    storage.width = width;
    storage.height = height;
    storage.format = HioFormatFloat32Vec4;
    storage.flipped = true;
    storage.data = pixels->data();

    const HioImageSharedPtr image = HioImage::OpenForWriting(filename);
    if (!image || !image->Write(storage)) {
        TF_WARN("Failed to write RenderProduct image '%s'", filename.c_str());
        return false;
    }

    return true;
}

HdEmbreeRenderPass::HdEmbreeRenderPass(HdRenderIndex *index,
                                       HdRprimCollection const &collection,
                                       HdRenderThread *renderThread,
                                       ty::Renderer *renderer,
                                       std::atomic<int> *sceneVersion,
                                       std::atomic<int> *materialVersion)
    : HdRenderPass(index, collection)
    , _lastCollectionReprSelector(collection.GetReprSelector())
    , _lastCollectionForcedRepr(collection.IsForcedRepr())
    , _renderThread(renderThread)
    , _renderer(renderer)
    , _sceneVersion(sceneVersion)
    , _lastSceneVersion(0)
    , _materialVersion(materialVersion)
    , _lastMaterialVersion(0)
    , _lastSettingsVersion(0)
    , _hasAppliedRendererSettings(false)
    , _lastRenderSettingsPrimPath()
    , _hasAppliedRenderSettingsPrim(false)
    , _lastBridgedRenderSettings()
    , _lastMaterialRenderContexts()
    , _lastFrame(0.0)
    , _lastTime(0.0)
    , _viewMatrix(1.0f) // == identity
    , _projMatrix(1.0f) // == identity
    , _hasSubdivisionCamera(false)
    , _dynamicSubdivisionTessellation(false)
    , _subdivisionSceneUpdatePending(false)
    , _subdivisionDisplacementUpdatePending(false)
    , _subdivisionViewMatrix(1.0f)
    , _subdivisionProjMatrix(1.0f)
    , _cameraExposureScale(1.0f)
    , _cameraDepthOfField()
    , _wireframeColor(0.0f)
    , _wireframeLineWidth(1.0f)
    , _aovBindings()
    , _hasInstalledAovBindings(false)
    , _aovBindingsVersion(0)
    , _colorBuffer(SdfPath::EmptyPath())
    , _depthBuffer(SdfPath::EmptyPath())
    , _renderProductsWritten(false)
{
}

void
HdEmbreeRenderPass::_MarkCollectionDirty()
{
    HdRprimCollection const& collection = GetRprimCollection();
    HdReprSelector const& reprSelector = collection.GetReprSelector();
    const bool forcedRepr = collection.IsForcedRepr();
    if (_lastCollectionReprSelector == reprSelector &&
        _lastCollectionForcedRepr == forcedRepr) {
        return;
    }

    _lastCollectionReprSelector = reprSelector;
    _lastCollectionForcedRepr = forcedRepr;

    // HdDirtyList only rebuilds when it encounters a repr selector for the
    // first time. Marking DirtyRepr makes previously initialized selectors
    // take the same InitRepr/Sync path when a viewer switches back to them.
    HdRenderIndex* const index = GetRenderIndex();
    if (index) {
        index->GetChangeTracker().MarkAllRprimsDirty(
            HdChangeTracker::DirtyRepr);
    }
}

HdEmbreeRenderPass::~HdEmbreeRenderPass()
{
    // Stop only a render that owns this pass's borrowed buffers. Destroying a
    // non-current pass must not cancel another live pass's render.
    if (_hasInstalledAovBindings &&
        _aovBindingsVersion == _renderer->GetAovBindingsVersion()) {
        _renderThread->StopRender();
        _renderer->SetAovBindings(HdRenderPassAovBindingVector());
    }
}

bool
HdEmbreeRenderPass::_HasConverged() const
{
    // Convergence and frame validity belong only to the pass whose bindings
    // remain active in the shared renderer.
    if (!_hasInstalledAovBindings ||
        _aovBindingsVersion != _renderer->GetAovBindingsVersion()) {
        return false;
    }

    // Empty caller bindings use this pass's anonymous fallback buffers.
    if (_aovBindings.empty()) {
        return _colorBuffer.IsConverged() &&
            _depthBuffer.IsConverged();
    }

    // Explicit output is complete only after every usable attachment parks.
    for (HdRenderPassAovBinding const& binding : _aovBindings) {
        if (binding.renderBuffer &&
            !binding.renderBuffer->IsConverged()) {
            return false;
        }
    }
    return true;
}

bool
HdEmbreeRenderPass::IsConverged() const
{
    const bool converged = _HasConverged();
    if (converged && _renderer->DidLastFrameProduceValidPixels()) {
        HdEmbreeRenderPass *self = const_cast<HdEmbreeRenderPass *>(this);
        if (!self->_renderProductsWritten) {
            self->_WriteActiveRenderProducts();
            self->_renderProductsWritten = true;
        }
    }
    return converged;
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
            ty::DefaultEnableExposureCompensation);
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

static ty::CameraDepthOfField
_GetCameraDepthOfField(HdRenderPassStateSharedPtr const& renderPassState)
{
    HdCamera const * const camera = renderPassState->GetCamera();
    if (!camera) {
        return ty::CameraDepthOfField();
    }

    ty::CameraDepthOfField result;
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
    if (HdDoubleDataSourceHandle frameHandle = sgSchema.GetCurrentFrame()) {
        const double value = frameHandle->GetTypedValue(0);
        if (std::isfinite(value)) {
            currentFrame = value;
        }
    }

    double timeCodesPerSecond = 1.0;
    if (HdDoubleDataSourceHandle tcpsHandle =
            sgSchema.GetTimeCodesPerSecond()) {
        const double value = tcpsHandle->GetTypedValue(0);
        if (std::isfinite(value) && value > 0.0) {
            timeCodesPerSecond = value;
        }
    }

    *frame = currentFrame;
    *time = currentFrame / timeCodesPerSecond;
}

static void
_ResyncMaterialNetworksForRenderSettingsChange(HdRenderIndex *index)
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
            material->ResyncForRenderSettingsChange(renderParam);
        }
    }
}

void
HdEmbreeRenderPass::_WriteActiveRenderProducts()
{
    HdRenderIndex *index = GetRenderIndex();
    if (!index) {
        return;
    }

    if (index->GetRenderDelegate()->GetRenderSetting<bool>(
            HdRenderSettingsTokens->enableInteractive, true)) {
        return;
    }

    HdSceneIndexBaseRefPtr si = index->GetTerminalSceneIndex();
    if (!si) {
        return;
    }

    SdfPath rsPath;
    if (!HdUtils::HasActiveRenderSettingsPrim(si, &rsPath)) {
        return;
    }

    HdSceneIndexPrim prim = si->GetPrim(rsPath);
    HdRenderSettingsSchema settingsSchema =
        HdRenderSettingsSchema::GetFromParent(prim.dataSource);
    HdRenderProductVectorSchema productsSchema =
        settingsSchema.GetRenderProducts();
    if (!productsSchema || productsSchema.GetNumElements() == 0) {
        return;
    }

    HdRenderBuffer *colorBuffer =
        _GetColorRenderBuffer(_aovBindings, &_colorBuffer);
    if (!colorBuffer) {
        TF_WARN("Cannot write RenderProduct images; no color AOV buffer "
                "is available");
        return;
    }

    std::vector<float> pixels;
    if (!_CopyRenderBufferToFloatRgba(colorBuffer, &pixels)) {
        return;
    }

    const unsigned int width = colorBuffer->GetWidth();
    const unsigned int height = colorBuffer->GetHeight();
    for (size_t i = 0; i < productsSchema.GetNumElements(); ++i) {
        HdRenderProductSchema productSchema = productsSchema.GetElement(i);
        if (!productSchema) {
            continue;
        }

        SdfPath productPath;
        if (HdPathDataSourceHandle handle = productSchema.GetPath()) {
            productPath = handle->GetTypedValue(0);
        }

        if (HdTokenDataSourceHandle handle = productSchema.GetType()) {
            const TfToken productType = handle->GetTypedValue(0);
            if (!productType.IsEmpty() && productType != TfToken("raster")) {
                TF_WARN("Skipping unsupported RenderProduct <%s> of type '%s'",
                        productPath.GetText(), productType.GetText());
                continue;
            }
        }

        if (!_RenderProductRequestsColor(productSchema)) {
            TF_WARN("Skipping RenderProduct <%s>; hdEmbree currently writes "
                    "only color/raw raster products",
                    productPath.GetText());
            continue;
        }

        TfToken productName;
        if (HdTokenDataSourceHandle handle = productSchema.GetName()) {
            productName = handle->GetTypedValue(0);
        }
        if (productName.IsEmpty()) {
            TF_WARN("Skipping RenderProduct <%s> without productName",
                    productPath.GetText());
            continue;
        }

        _WriteFloatRgbaImage(
            productName.GetString(),
            width,
            height,
            &pixels);
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

    // Remove stale bridge-owned opinions first so disappearing USD values
    // reveal delegate defaults without clobbering later direct overrides.
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

    // Adopt newly authored opinions only where no direct delegate/UI value
    // already has precedence.
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

    // A live pass can replace all state in the shared renderer. Drop any
    // binding set not installed by this pass, then republish this pass's
    // cached inputs even when its local values have not changed.
    const bool passActivated =
        !_hasInstalledAovBindings ||
        _aovBindingsVersion != _renderer->GetAovBindingsVersion();
    bool needStartRender = passActivated;
    if (passActivated) {
        _renderThread->StopRender();
        if (!_renderer->GetAovBindings().empty()) {
            _renderer->SetAovBindings(HdRenderPassAovBindingVector());
        }
        _hasInstalledAovBindings = false;
    }

    if (_UpdateRenderSettingsFromActiveRenderSettingsPrim()) {
        needStartRender = true;
    }

    int currentSceneVersion = _sceneVersion->load();
    bool sceneChanged = _lastSceneVersion != currentSceneVersion;
    if (sceneChanged) {
        needStartRender = true;
        _lastSceneVersion = currentSceneVersion;
    }
    int currentMaterialVersion = _materialVersion->load();
    bool materialChanged =
        _lastMaterialVersion != currentMaterialVersion;
    if (materialChanged) {
        _lastMaterialVersion = currentMaterialVersion;
    }

    bool frameTimeChanged = false;
    {
        HdRenderIndex *index = GetRenderIndex();
        const HdSceneIndexBaseRefPtr si = index->GetTerminalSceneIndex();
        double currentFrame = 0.0;
        double currentTime = 0.0;
        _GetSceneFrameAndTime(si, &currentFrame, &currentTime);
        frameTimeChanged =
            _lastFrame != currentFrame || _lastTime != currentTime;
        if (passActivated || frameTimeChanged) {
            _renderThread->StopRender();
            _renderer->SetSceneFrameAndTime(
                static_cast<float>(currentFrame),
                static_cast<float>(currentTime));
            _lastFrame = currentFrame;
            _lastTime = currentTime;
            needStartRender = true;
        }
    }

    const GfVec4f wireframeColor = renderPassState->GetWireframeColor();
    const float wireframeLineWidth = renderPassState->GetLineWidth();
    if (passActivated ||
        _wireframeColor != wireframeColor ||
        _wireframeLineWidth != wireframeLineWidth) {
        _wireframeColor = wireframeColor;
        _wireframeLineWidth = wireframeLineWidth;
        _renderThread->StopRender();
        _renderer->SetWireframeStyle(
            _wireframeColor, _wireframeLineWidth);
        _renderer->ResetAccumulation();
        needStartRender = true;
    }

    // Likewise the render settings.
    HdRenderDelegate *renderDelegate = GetRenderIndex()->GetRenderDelegate();
    int currentSettingsVersion = renderDelegate->GetRenderSettingsVersion();
    if (passActivated ||
        !_hasAppliedRendererSettings ||
        _lastSettingsVersion != currentSettingsVersion) {
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

        const ty::RenderSettings defaults;

        const TfToken renderColorSpaceToken = _GetTokenRenderSetting(
            renderDelegate,
            HdRenderSettingsPrimTokens->renderingColorSpace,
            GfColorSpaceNames->LinearRec709);
        ty::RenderColorSpace renderColorSpace =
            ty::RenderColorSpace::LinearRec709;
        if (!ty::ParseRenderColorSpace(
                renderColorSpaceToken, &renderColorSpace)) {
            TF_WARN(
                "hdEmbree rendering color space '%s' is unsupported; "
                "falling back to '%s'. Supported values are '%s', '%s', "
                "and '%s'.",
                renderColorSpaceToken.GetText(),
                GfColorSpaceNames->LinearRec709.GetText(),
                GfColorSpaceNames->LinearRec709.GetText(),
                GfColorSpaceNames->LinearAP1.GetText(),
                GfColorSpaceNames->Data.GetText());
        }
        const bool materialColorSpaceChanged =
            _renderer->GetMaterialEvalServices()->renderColorSpace !=
                renderColorSpace;
        _renderer->SetRenderColorSpace(renderColorSpace);

        // Resolve Hydra values and cross-setting policy into one renderer
        // value before applying it while rendering is stopped.
        ty::RenderSettings nextSettings = defaults;
        nextSettings.samplesToConvergence =
            renderDelegate->GetRenderSetting<int>(
                HdEmbreeRenderSettingsTokens->convergedSamplesPerPixel,
                defaults.samplesToConvergence);
        nextSettings.textureCacheSizeMB =
            renderDelegate->GetRenderSetting<int>(
                HdEmbreeRenderSettingsTokens->textureCacheSize,
                defaults.textureCacheSizeMB);
        nextSettings.enableLighting =
            renderDelegate->GetRenderSetting<bool>(
                HdEmbreeRenderSettingsTokens->enableLighting,
                defaults.enableLighting);
        const bool enableAmbientOcclusion =
            renderDelegate->GetRenderSetting<bool>(
                HdEmbreeRenderSettingsTokens->enableAmbientOcclusion,
                ty::DefaultEnableAmbientOcclusion);
        nextSettings.ambientOcclusionSamples =
            !nextSettings.enableLighting && enableAmbientOcclusion
                ? renderDelegate->GetRenderSetting<int>(
                    HdEmbreeRenderSettingsTokens->ambientOcclusionSamples,
                    defaults.ambientOcclusionSamples)
                : 0;
        nextSettings.domeLightCameraVisibility =
            renderDelegate->GetRenderSetting<bool>(
                HdRenderSettingsTokens->domeLightCameraVisibility,
                defaults.domeLightCameraVisibility);
        nextSettings.enableSceneColors =
            renderDelegate->GetRenderSetting<bool>(
                HdEmbreeRenderSettingsTokens->enableSceneColors,
                defaults.enableSceneColors);
        nextSettings.randomNumberSeed =
            renderDelegate->GetRenderSetting<int>(
                HdEmbreeRenderSettingsTokens->randomNumberSeed,
                defaults.randomNumberSeed);
        nextSettings.tileSize =
            renderDelegate->GetRenderSetting<int>(
                HdEmbreeRenderSettingsTokens->tileSize,
                defaults.tileSize);
        nextSettings.enableAdaptiveSampling =
            renderDelegate->GetRenderSetting<bool>(
                HdEmbreeRenderSettingsTokens->enableAdaptiveSampling,
                defaults.enableAdaptiveSampling);
        nextSettings.adaptiveThreshold =
            renderDelegate->GetRenderSetting<float>(
                HdEmbreeRenderSettingsTokens->adaptiveThreshold,
                defaults.adaptiveThreshold);
        nextSettings.minSamplesBeforeAdaptive =
            renderDelegate->GetRenderSetting<int>(
                HdEmbreeRenderSettingsTokens->minSamplesBeforeAdaptive,
                defaults.minSamplesBeforeAdaptive);
        nextSettings.maxBounces =
            renderDelegate->GetRenderSetting<int>(
                HdEmbreeRenderSettingsTokens->maxBounces,
                defaults.maxBounces);
        nextSettings.minBouncesBeforeRR =
            renderDelegate->GetRenderSetting<int>(
                HdEmbreeRenderSettingsTokens->minBouncesBeforeRR,
                defaults.minBouncesBeforeRR);
        nextSettings.lightSamplesPerHit =
            renderDelegate->GetRenderSetting<int>(
                HdEmbreeRenderSettingsTokens->lightSamplesPerHit,
                defaults.lightSamplesPerHit);
        nextSettings.showAdaptiveHeatmap =
            renderDelegate->GetRenderSetting<bool>(
                HdEmbreeRenderSettingsTokens->showAdaptiveHeatmap,
                defaults.showAdaptiveHeatmap);
        nextSettings.fireflyClampThreshold =
            renderDelegate->GetRenderSetting<float>(
                HdEmbreeRenderSettingsTokens->fireflyClampThreshold,
                defaults.fireflyClampThreshold);
        nextSettings.enableCaustics =
            renderDelegate->GetRenderSetting<bool>(
                HdEmbreeRenderSettingsTokens->enableCaustics,
                defaults.enableCaustics);
        nextSettings.causticsClampThreshold =
            renderDelegate->GetRenderSetting<float>(
                HdEmbreeRenderSettingsTokens->causticsClampThreshold,
                defaults.causticsClampThreshold);
        nextSettings.disableShadows =
            renderDelegate->GetRenderSetting<bool>(
                HdEmbreeRenderSettingsTokens->disableShadows,
                defaults.disableShadows);

        const TfToken defaultDielectricModeToken =
            ty::GetDielectricLayerThroughputModeToken(
                defaults.dielectricLayerThroughputMode);
        const TfToken dielectricModeToken = _GetTokenRenderSetting(
            renderDelegate,
            HdEmbreeRenderSettingsTokens->dielectricLayerThroughputMode,
            defaultDielectricModeToken);
        nextSettings.dielectricLayerThroughputMode =
            ty::GetDielectricLayerThroughputModeFromToken(
                dielectricModeToken);
        if (ty::GetDielectricLayerThroughputModeToken(
                nextSettings.dielectricLayerThroughputMode) !=
            dielectricModeToken) {
            TF_WARN(
                "hdEmbree dielectric layer throughput mode '%s' is unknown; "
                "falling back to '%s'.",
                dielectricModeToken.GetText(),
                defaultDielectricModeToken.GetText());
            nextSettings.dielectricLayerThroughputMode =
                defaults.dielectricLayerThroughputMode;
        }
        nextSettings.useAdobeOpenPBR =
            renderDelegate->GetRenderSetting<bool>(
                HdEmbreeRenderSettingsTokens->useAdobeOpenPBR,
                defaults.useAdobeOpenPBR);

        _renderer->SetRenderSettings(nextSettings);
        _hasAppliedRendererSettings = true;

        if (materialRenderContextsChanged || materialColorSpaceChanged) {
            _ResyncMaterialNetworksForRenderSettingsChange(GetRenderIndex());

            // The resync can publish a new displacement graph. Observe its
            // versions in this Execute so we never render one pass with new
            // shading state but stale displaced tessellation.
            currentSceneVersion = _sceneVersion->load();
            if (_lastSceneVersion != currentSceneVersion) {
                sceneChanged = true;
                _lastSceneVersion = currentSceneVersion;
            }
            currentMaterialVersion = _materialVersion->load();
            if (_lastMaterialVersion != currentMaterialVersion) {
                materialChanged = true;
                _lastMaterialVersion = currentMaterialVersion;
            }
        }

        needStartRender = true;
    }

    // Determine whether we need to update the renderer camera.
    const GfMatrix4d view = renderPassState->GetWorldToViewMatrix();
    const GfMatrix4d proj = renderPassState->GetProjectionMatrix();
    const float cameraExposureScale =
        _GetCameraExposureScale(renderPassState, renderDelegate);
    const ty::CameraDepthOfField cameraDepthOfField =
        _GetCameraDepthOfField(renderPassState);
    const bool projectionChanged =
        passActivated ||
        _viewMatrix != view || _projMatrix != proj;
    if (projectionChanged ||
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

    const bool dataWindowChanged =
        passActivated || _dataWindow != dataWindow;
    if (dataWindowChanged) {
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

    const bool dynamicSubdivisionTessellation =
        renderDelegate->GetRenderSetting<bool>(
            HdEmbreeRenderSettingsTokens->dynamicSubdvTesselation,
            ty::DefaultDynamicSubdvTesselation);
    const bool dynamicSubdivisionTessellationEnabled =
        dynamicSubdivisionTessellation &&
        !_dynamicSubdivisionTessellation;
    _dynamicSubdivisionTessellation = dynamicSubdivisionTessellation;

    // Freeze the first attached camera and valid viewport by default.
    // Dynamic mode replaces that snapshot whenever either changes.
    const bool hasAttachedCamera = renderPassState->GetCamera() != nullptr;
    const bool hasValidDataWindow =
        _dataWindow.GetWidth() > 0 && _dataWindow.GetHeight() > 0;
    const bool updateSubdivisionCamera =
        hasAttachedCamera && hasValidDataWindow &&
        (!_hasSubdivisionCamera || dynamicSubdivisionTessellationEnabled ||
         (dynamicSubdivisionTessellation &&
          (projectionChanged || dataWindowChanged)));
    if (updateSubdivisionCamera) {
        _hasSubdivisionCamera = true;
        _subdivisionViewMatrix = _viewMatrix;
        _subdivisionProjMatrix = _projMatrix;
        _subdivisionDataWindow = _dataWindow;
    }

    // A material recompile replaces graphs and their shared handle table
    // without necessarily dirtying bound meshes. Refresh all prototypes
    // before subdivision commits can evaluate displacement.
    if (materialChanged) {
        _renderThread->StopRender();
        static_cast<HdEmbreeRenderDelegate*>(renderDelegate)
            ->RefreshMaterialBindings();
        needStartRender = true;
    }

    // Scene changes can add subdivision meshes. Displacement materials and
    // animated graph inputs can also change geometry without changing edge
    // levels. Hold that work until a camera is attached, then recommit it
    // against the frozen subdivision camera unless dynamic updates are
    // enabled.
    _subdivisionSceneUpdatePending |=
        sceneChanged || materialChanged || frameTimeChanged;
    _subdivisionDisplacementUpdatePending |=
        materialChanged || frameTimeChanged;
    if (hasAttachedCamera && _hasSubdivisionCamera &&
        (passActivated ||
         _subdivisionSceneUpdatePending ||
         updateSubdivisionCamera)) {
        _renderThread->StopRender();
        if (static_cast<HdEmbreeRenderDelegate*>(renderDelegate)
                ->UpdateAdaptiveSubdivision(
                    _subdivisionViewMatrix, _subdivisionProjMatrix,
                    _subdivisionDataWindow,
                    _subdivisionDisplacementUpdatePending)) {
            _renderer->ResetAccumulation();
        }
        _subdivisionSceneUpdatePending = false;
        _subdivisionDisplacementUpdatePending = false;
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
    if (!_hasInstalledAovBindings ||
        _aovBindings != aovBindings ||
        _renderer->GetAovBindings().empty()) {
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
        _hasInstalledAovBindings = true;
        _aovBindingsVersion = _renderer->GetAovBindingsVersion();
        // Preserve the last resolved image across restarts and only reset
        // progressive accumulation state on the new attachments.
        _renderer->ResetAccumulation();
        needStartRender = true;
    }

    TF_VERIFY(
        !_renderer->GetAovBindings().empty(),
        "No aov bindings to render into");

    // Only start a new render if something in the scene has changed.
    if (needStartRender) {
        _renderProductsWritten = false;
        _renderer->MarkAovBuffersUnconverged();
        _renderer->MarkFramePending();
        _renderThread->StartRender();
    }
}

PXR_NAMESPACE_CLOSE_SCOPE
