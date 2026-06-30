//
// Copyright 2016 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/imaging/hd/camera.h"
#include "pxr/imaging/hd/renderPassState.h"
#include "pxr/imaging/hd/material.h"
#include "pxr/imaging/hd/renderProductSchema.h"
#include "pxr/imaging/hd/renderSettingsSchema.h"
#include "pxr/imaging/hd/renderVarSchema.h"
#include "pxr/imaging/hd/sceneGlobalsSchema.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/hd/types.h"
#include "pxr/imaging/hd/utils.h"
#include "pxr/imaging/hio/image.h"
#include "pxr/imaging/plugin/hdEmbree/config.h"
#include "pxr/imaging/plugin/hdEmbree/material.h"
#include "pxr/imaging/plugin/hdEmbree/renderDelegate.h"
#include "pxr/imaging/plugin/hdEmbree/renderPass.h"
#include "pxr/base/gf/half.h"
#include "pxr/base/tf/diagnostic.h"
#include "pxr/base/tf/enum.h"

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

    if (auto ds = container->Get(key)) {
        if (auto sampled = HdSampledDataSource::Cast(ds)) {
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

static bool
_RenderVarRequestsColor(HdRenderVarSchema varSchema)
{
    TfToken sourceName;
    if (auto handle = varSchema.GetSourceName()) {
        sourceName = handle->GetTypedValue(0);
    }

    if (sourceName != HdAovTokens->color && sourceName != TfToken("Ci")) {
        return false;
    }

    if (auto handle = varSchema.GetSourceType()) {
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

static std::string
_DefaultFrameString(double frame)
{
    if (std::isfinite(frame) && std::floor(frame) == frame) {
        return std::to_string(static_cast<long long>(frame));
    }

    std::ostringstream out;
    out << frame;
    return out.str();
}

static bool
_ParseUnsignedDecimal(const std::string &text, int *value)
{
    if (text.empty()) {
        *value = 0;
        return true;
    }

    int result = 0;
    for (char ch : text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        result = result * 10 + (ch - '0');
    }

    *value = result;
    return true;
}

static bool
_FormatFrameWithSpec(double frame,
                     const std::string &spec,
                     std::string *formatted)
{
    if (spec.empty()) {
        *formatted = _DefaultFrameString(frame);
        return true;
    }

    if (spec.back() != 'd') {
        return false;
    }

    std::string widthText = spec.substr(0, spec.size() - 1);
    char fill = ' ';
    if (widthText.size() > 1 && widthText[0] == '0') {
        fill = '0';
        widthText.erase(0, 1);
    }

    int width = 0;
    if (!_ParseUnsignedDecimal(widthText, &width)) {
        return false;
    }

    std::ostringstream out;
    if (width > 0) {
        out << std::setfill(fill) << std::setw(width);
    }
    out << static_cast<long long>(std::llround(frame));
    *formatted = out.str();
    return true;
}

static std::string
_ExpandFramePlaceholders(const std::string &productName, double frame)
{
    std::string result = productName;
    size_t searchFrom = 0;
    while (true) {
        const size_t open = result.find("{frame", searchFrom);
        if (open == std::string::npos) {
            break;
        }

        const size_t close = result.find('}', open);
        if (close == std::string::npos) {
            TF_WARN("RenderProduct productName '%s' has an unterminated "
                    "frame placeholder",
                    productName.c_str());
            break;
        }

        const std::string field = result.substr(open + 1, close - open - 1);
        std::string spec;
        if (field == "frame") {
            spec = std::string();
        } else if (field.compare(0, 6, "frame:") == 0) {
            spec = field.substr(6);
        } else {
            searchFrom = close + 1;
            continue;
        }

        std::string formatted;
        if (!_FormatFrameWithSpec(frame, spec, &formatted)) {
            TF_WARN("RenderProduct productName '%s' has unsupported frame "
                    "placeholder '{%s}'",
                    productName.c_str(), field.c_str());
            searchFrom = close + 1;
            continue;
        }

        result.replace(open, close - open + 1, formatted);
        searchFrom = open + formatted.size();
    }

    return result;
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
    , _renderProductsWritten(false)
{
}

HdEmbreeRenderPass::~HdEmbreeRenderPass()
{
    // Make sure the render thread's not running, in case it's writing
    // to _colorBuffer/_depthBuffer.
    _renderThread->StopRender();
}

bool
HdEmbreeRenderPass::_HasConverged() const
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

bool
HdEmbreeRenderPass::IsConverged() const
{
    const bool converged = _HasConverged();
    if (converged) {
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

void
HdEmbreeRenderPass::_WriteActiveRenderProducts()
{
    HdRenderIndex *index = GetRenderIndex();
    if (!index) {
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
        if (auto handle = productSchema.GetPath()) {
            productPath = handle->GetTypedValue(0);
        }

        if (auto handle = productSchema.GetType()) {
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
        if (auto handle = productSchema.GetName()) {
            productName = handle->GetTypedValue(0);
        }
        if (productName.IsEmpty()) {
            TF_WARN("Skipping RenderProduct <%s> without productName",
                    productPath.GetText());
            continue;
        }

        _WriteFloatRgbaImage(
            _ExpandFramePlaceholders(productName.GetString(), _lastFrame),
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
                HdRenderSettingsTokens->domeLightCameraVisibility,
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
        _renderProductsWritten = false;
        _renderer->MarkAovBuffersUnconverged();
        _renderThread->StartRender();
    }
}

PXR_NAMESPACE_CLOSE_SCOPE
