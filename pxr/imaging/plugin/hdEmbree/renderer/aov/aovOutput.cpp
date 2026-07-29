//
// Copyright 2018 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
// AOV validation, accumulation, dispatch, and hit outputs.

#include <renderer/geometry/normalTransforms.h>
#include <renderer/geometry/surfaceDerivatives.h>
#include <renderer/rayUtil.h>
#include <renderer/renderBuffer.h>
#include <renderer/renderer.h>

#include "pxr/base/work/loops.h"
#include "pxr/base/work/threadLimits.h"
#include "pxr/imaging/hd/meshUtil.h"
#include "pxr/imaging/hd/perfLog.h"
#include "pxr/imaging/hd/renderBuffer.h"
#include "pxr/imaging/hd/tokens.h"

#include <chrono>
#include <cstdio>
#include <thread>

PXR_NAMESPACE_OPEN_SCOPE

namespace
{

constexpr float _kAdaptiveAbsoluteStdError = 0.001f;
constexpr float _kAdaptiveAbsoluteVarianceOfMean =
    _kAdaptiveAbsoluteStdError * _kAdaptiveAbsoluteStdError;

bool
_IsPerChannelVarianceConverged(
    GfVec3f const& varOfMean,
    GfVec3f const& mean,
    float relativeVarianceThreshold)
{
    const float threshold = std::max(0.0f, relativeVarianceThreshold);
    for (int c = 0; c < 3; ++c) {
        const float meanMagnitude = std::abs(mean[c]);
        const float varianceLimit =
            _kAdaptiveAbsoluteVarianceOfMean
            + threshold * meanMagnitude * meanMagnitude;
        if (varOfMean[c] > varianceLimit) {
            return false;
        }
    }
    return true;
}

// Returns false if the hit has no valid instance/prototype context.
// Outputs are written only on success.
bool
_GetHitContexts(
    RTCScene scene,
    RTCRayHit const& rayHit,
    HdEmbreeInstanceContext const** instanceContextOutput,
    HdEmbreePrototypeContext const** prototypeContextOutput)
{
    if (instanceContextOutput == nullptr ||
        prototypeContextOutput == nullptr ||
        scene == nullptr ||
        rayHit.hit.geomID == RTC_INVALID_GEOMETRY_ID ||
        rayHit.hit.instID[0] == RTC_INVALID_GEOMETRY_ID) {
        return false;
    }

    // hdEmbree flattens Hydra instances, so only the first Embree instance
    // level owns the prototype scene containing the hit geometry.
    RTCGeometry instanceGeometry =
        rtcGetGeometry(scene, rayHit.hit.instID[0]);
    if (instanceGeometry == nullptr) {
        return false;
    }
    HdEmbreeInstanceContext const* instanceContext =
        static_cast<HdEmbreeInstanceContext const*>(
            rtcGetGeometryUserData(instanceGeometry));
    if (instanceContext == nullptr || instanceContext->rootScene == nullptr) {
        return false;
    }

    RTCGeometry prototypeGeometry =
        rtcGetGeometry(instanceContext->rootScene, rayHit.hit.geomID);
    if (prototypeGeometry == nullptr) {
        return false;
    }
    HdEmbreePrototypeContext const* prototypeContext =
        static_cast<HdEmbreePrototypeContext const*>(
            rtcGetGeometryUserData(prototypeGeometry));
    if (prototypeContext == nullptr) {
        return false;
    }

    *instanceContextOutput = instanceContext;
    *prototypeContextOutput = prototypeContext;
    return true;
}

} // anonymous namespace

void
HdEmbreeRenderer::SetAovBindings(
    HdRenderPassAovBindingVector const& aovBindings)
{
    _aovBindings = aovBindings;
    _aovNames.resize(_aovBindings.size());
    for (size_t i = 0; i < _aovBindings.size(); ++i) {
        _aovNames[i] = HdParsedAovToken(_aovBindings[i].aovName);
    }
}

bool
HdEmbreeRenderer::_ValidateAovBindings()
{
    if (_aovBindings.empty()) {
        TF_WARN("Cannot render without an AOV binding");
        return false;
    }

    bool bindingsValid = true;
    unsigned int bufferWidth = 0;
    unsigned int bufferHeight = 0;

    for (size_t i = 0; i < _aovBindings.size(); ++i) {
        // By the time the attachment gets here, there should be a bound
        // output buffer.
        if (_aovBindings[i].renderBuffer == nullptr) {
            TF_WARN("Aov '%s' doesn't have any renderbuffer bound",
                    _aovNames[i].name.GetText());
            bindingsValid = false;
            continue;
        }

        HdEmbreeRenderBufferInterface *rb =
            dynamic_cast<HdEmbreeRenderBufferInterface*>(
                _aovBindings[i].renderBuffer);
        if (rb == nullptr) {
            TF_WARN("Aov '%s' renderbuffer is not an hdEmbree render buffer",
                    _aovNames[i].name.GetText());
            bindingsValid = false;
            continue;
        }

        if (_aovNames[i].name != HdAovTokens->color &&
            _aovNames[i].name != HdAovTokens->cameraDepth &&
            _aovNames[i].name != HdAovTokens->depth &&
            _aovNames[i].name != HdAovTokens->primId &&
            _aovNames[i].name != HdAovTokens->instanceId &&
            _aovNames[i].name != HdAovTokens->elementId &&
            _aovNames[i].name != HdAovTokens->Neye &&
            _aovNames[i].name != HdAovTokens->normal &&
            _aovNames[i].name != HdEmbreeAovTokens->adaptiveHeatmap &&
            !_aovNames[i].isPrimvar) {
            TF_WARN("Unsupported attachment with Aov '%s' won't be rendered to",
                    _aovNames[i].name.GetText());
        }

        HdFormat format = rb->GetFormat();

        // depth is only supported for float32 attachments
        if ((_aovNames[i].name == HdAovTokens->cameraDepth ||
             _aovNames[i].name == HdAovTokens->depth) &&
            format != HdFormatFloat32) {
            TF_WARN("Aov '%s' has unsupported format '%s'",
                    _aovNames[i].name.GetText(),
                    TfEnum::GetName(format).c_str());
            bindingsValid = false;
        }

        // ids are only supported for int32 attachments
        if ((_aovNames[i].name == HdAovTokens->primId ||
             _aovNames[i].name == HdAovTokens->instanceId ||
             _aovNames[i].name == HdAovTokens->elementId) &&
            format != HdFormatInt32) {
            TF_WARN("Aov '%s' has unsupported format '%s'",
                    _aovNames[i].name.GetText(),
                    TfEnum::GetName(format).c_str());
            bindingsValid = false;
        }

        // Normal is only supported for vec3 attachments of float.
        if ((_aovNames[i].name == HdAovTokens->Neye ||
             _aovNames[i].name == HdAovTokens->normal) &&
            format != HdFormatFloat32Vec3) {
            TF_WARN("Aov '%s' has unsupported format '%s'",
                    _aovNames[i].name.GetText(),
                    TfEnum::GetName(format).c_str());
            bindingsValid = false;
        }

        // Primvars support vec3 output (though some channels may not be used).
        if (_aovNames[i].isPrimvar &&
            format != HdFormatFloat32Vec3) {
            TF_WARN("Aov 'primvars:%s' has unsupported format '%s'",
                    _aovNames[i].name.GetText(),
                    TfEnum::GetName(format).c_str());
            bindingsValid = false;
        }

        // The adaptive heatmap writes four float color components.
        if (_aovNames[i].name == HdEmbreeAovTokens->adaptiveHeatmap &&
            format != HdFormatFloat32Vec4) {
            TF_WARN("Aov '%s' has unsupported format '%s'",
                    _aovNames[i].name.GetText(),
                    TfEnum::GetName(format).c_str());
            bindingsValid = false;
        }

        // color is only supported for vec3/vec4 attachments of float,
        // unorm, or snorm.
        if (_aovNames[i].name == HdAovTokens->color) {
            switch (format) {
                case HdFormatUNorm8Vec4:
                case HdFormatUNorm8Vec3:
                case HdFormatSNorm8Vec4:
                case HdFormatSNorm8Vec3:
                case HdFormatFloat32Vec4:
                case HdFormatFloat32Vec3:
                    break;
                default:
                    TF_WARN("Aov '%s' has unsupported format '%s'",
                        _aovNames[i].name.GetText(),
                        TfEnum::GetName(format).c_str());
                    bindingsValid = false;
                    break;
            }
        }

        // make sure the clear value is reasonable for the format of the
        // attached buffer.
        if (!_aovBindings[i].clearValue.IsEmpty()) {
            HdTupleType clearType =
                HdGetValueTupleType(_aovBindings[i].clearValue);

            // array-valued clear types aren't supported.
            if (clearType.count != 1) {
                TF_WARN("Aov '%s' clear value type '%s' is an array",
                        _aovNames[i].name.GetText(),
                        _aovBindings[i].clearValue.GetTypeName().c_str());
                bindingsValid = false;
            }

            // color only supports float/double vec3/4
            if (_aovNames[i].name == HdAovTokens->color &&
                clearType.type != HdTypeFloatVec3 &&
                clearType.type != HdTypeFloatVec4 &&
                clearType.type != HdTypeDoubleVec3 &&
                clearType.type != HdTypeDoubleVec4) {
                TF_WARN("Aov '%s' clear value type '%s' isn't compatible",
                        _aovNames[i].name.GetText(),
                        _aovBindings[i].clearValue.GetTypeName().c_str());
                bindingsValid = false;
            }

            // only clear float formats with float, int with int, float3 with
            // float3.
            if ((format == HdFormatFloat32 && clearType.type != HdTypeFloat) ||
                (format == HdFormatInt32 && clearType.type != HdTypeInt32) ||
                (format == HdFormatFloat32Vec3 &&
                 clearType.type != HdTypeFloatVec3)) {
                TF_WARN("Aov '%s' clear value type '%s' isn't compatible with"
                        " format %s",
                        _aovNames[i].name.GetText(),
                        _aovBindings[i].clearValue.GetTypeName().c_str(),
                        TfEnum::GetName(format).c_str());
                bindingsValid = false;
            }
        }

        const unsigned int width = rb->GetWidth();
        const unsigned int height = rb->GetHeight();
        if (width == 0 || height == 0) {
            TF_WARN("Aov '%s' renderbuffer has zero size %u x %u",
                    _aovNames[i].name.GetText(), width, height);
            bindingsValid = false;
        } else if (bufferWidth == 0 && bufferHeight == 0) {
            bufferWidth = width;
            bufferHeight = height;
        } else if (bufferWidth != width || bufferHeight != height) {
            TF_WARN(
                "Aov '%s' renderbuffer size %u x %u does not match %u x %u",
                _aovNames[i].name.GetText(),
                width,
                height,
                bufferWidth,
                bufferHeight);
            bindingsValid = false;
        }
    }

    if (_dataWindow.GetWidth() <= 0 || _dataWindow.GetHeight() <= 0) {
        TF_WARN("Cannot render an empty data window");
        bindingsValid = false;
    } else if (bufferWidth > 0 && bufferHeight > 0 &&
               (_dataWindow.GetMinX() < 0 ||
                _dataWindow.GetMaxX() >= static_cast<int>(bufferWidth) ||
                _dataWindow.GetMinY() < 0 ||
                _dataWindow.GetMaxY() >= static_cast<int>(bufferHeight))) {
        TF_WARN("dataWindow is not contained by the render buffers");
        bindingsValid = false;
    }

    if (bindingsValid) {
        _width = bufferWidth;
        _height = bufferHeight;
    }
    return bindingsValid;
}

GfVec4f
HdEmbreeRenderer::_GetClearColor(VtValue const& clearValue)
{
    HdTupleType type = HdGetValueTupleType(clearValue);
    if (type.count != 1) {
        return GfVec4f(0.0f, 0.0f, 0.0f, 1.0f);
    }

    switch(type.type) {
        case HdTypeFloatVec3:
        {
            GfVec3f f =
                *(static_cast<const GfVec3f*>(HdGetValueData(clearValue)));
            return GfVec4f(f[0], f[1], f[2], 1.0f);
        }
        case HdTypeFloatVec4:
        {
            GfVec4f f =
                *(static_cast<const GfVec4f*>(HdGetValueData(clearValue)));
            return f;
        }
        case HdTypeDoubleVec3:
        {
            GfVec3d f =
                *(static_cast<const GfVec3d*>(HdGetValueData(clearValue)));
            return GfVec4f(f[0], f[1], f[2], 1.0f);
        }
        case HdTypeDoubleVec4:
        {
            GfVec4d f =
                *(static_cast<const GfVec4d*>(HdGetValueData(clearValue)));
            return GfVec4f(f);
        }
        default:
            return GfVec4f(0.0f, 0.0f, 0.0f, 1.0f);
    }
}

void
HdEmbreeRenderer::Clear()
{
    if (!_ValidateAovBindings()) {
        return;
    }

    for (size_t i = 0; i < _aovBindings.size(); ++i) {
        if (_aovBindings[i].clearValue.IsEmpty()) {
            continue;
        }

        HdEmbreeRenderBufferInterface *rb =
            dynamic_cast<HdEmbreeRenderBufferInterface*>(
                _aovBindings[i].renderBuffer);

        rb->Map();
        if (_aovNames[i].name == HdAovTokens->color) {
            GfVec4f clearColor = _GetClearColor(_aovBindings[i].clearValue);
            rb->Clear(4, clearColor.data());
        } else if (rb->GetFormat() == HdFormatInt32) {
            int32_t clearValue = _aovBindings[i].clearValue.Get<int32_t>();
            rb->Clear(1, &clearValue);
        } else if (rb->GetFormat() == HdFormatFloat32) {
            float clearValue = _aovBindings[i].clearValue.Get<float>();
            rb->Clear(1, &clearValue);
        } else if (rb->GetFormat() == HdFormatFloat32Vec3) {
            GfVec3f clearValue = _aovBindings[i].clearValue.Get<GfVec3f>();
            rb->Clear(3, clearValue.data());
        } // else, _ValidateAovBindings would have already warned.

        rb->Unmap();
        rb->SetConverged(false);
    }

    // Reset adaptive sampling state.
    std::fill(_pixelMean.begin(), _pixelMean.end(), GfVec3f(0.0f));
    std::fill(_pixelM2.begin(), _pixelM2.end(), GfVec3f(0.0f));
    std::fill(_pixelSampleCount.begin(), _pixelSampleCount.end(), 0);
    std::fill(_pixelConverged.begin(), _pixelConverged.end(), false);
}

void
HdEmbreeRenderer::ResetAccumulation()
{
    if (!_ValidateAovBindings()) {
        return;
    }

    for (size_t i = 0; i < _aovBindings.size(); ++i) {
        HdEmbreeRenderBufferInterface *rb =
            dynamic_cast<HdEmbreeRenderBufferInterface*>(
                _aovBindings[i].renderBuffer);
        rb->ClearSamples();
        rb->SetConverged(false);
    }

    std::fill(_pixelMean.begin(), _pixelMean.end(), GfVec3f(0.0f));
    std::fill(_pixelM2.begin(), _pixelM2.end(), GfVec3f(0.0f));
    std::fill(_pixelSampleCount.begin(), _pixelSampleCount.end(), 0);
    std::fill(_pixelConverged.begin(), _pixelConverged.end(), false);
}

void
HdEmbreeRenderer::MarkAovBuffersUnconverged()
{
    for (size_t i = 0; i < _aovBindings.size(); ++i) {
        HdEmbreeRenderBufferInterface *rb =
            dynamic_cast<HdEmbreeRenderBufferInterface*>(
                _aovBindings[i].renderBuffer);
        if (rb != nullptr) {
            rb->SetConverged(false);
        }
    }
}

void
HdEmbreeRenderer::_ClassifyAovOutputs()
{
    _aovOutputs.clear();
    _needColor = _settings.enableAdaptiveSampling;
    _colorClearValue = GfVec4f(0.0f);

    // Find color clear value and set _needColor.
    for (size_t i = 0; i < _aovNames.size(); ++i) {
        if (_aovNames[i].name == HdAovTokens->color) {
            _needColor = true;
            _colorClearValue = _GetClearColor(_aovBindings[i].clearValue);
            break;
        }
    }

    // Borrow interfaces from buffers mapped earlier in _PreRenderSetup.
    // They remain valid only until the next setup remaps the buffers.
    for (size_t i = 0; i < _aovBindings.size(); ++i) {
        HdEmbreeRenderBufferInterface* rb =
            dynamic_cast<HdEmbreeRenderBufferInterface*>(
                _aovBindings[i].renderBuffer);
        HdParsedAovToken const& aovName = _aovNames[i];

        if (aovName.name == HdAovTokens->color) {
            _AovKind const kind =
                (_settings.showAdaptiveHeatmap &&
                 _settings.enableAdaptiveSampling &&
                 !_pixelSampleCount.empty())
                ? _AovKind::ColorAdaptiveHeatmap
                : _AovKind::Color;
            _aovOutputs.push_back(_AovOutput{rb, kind, TfToken()});
        } else if (aovName.name == HdAovTokens->cameraDepth &&
                   rb->GetFormat() == HdFormatFloat32) {
            _aovOutputs.push_back(
                _AovOutput{rb, _AovKind::CameraDepth, TfToken()});
        } else if (aovName.name == HdAovTokens->depth &&
                   rb->GetFormat() == HdFormatFloat32) {
            _aovOutputs.push_back(
                _AovOutput{rb, _AovKind::Depth, TfToken()});
        } else if ((aovName.name == HdAovTokens->primId ||
                    aovName.name == HdAovTokens->elementId ||
                    aovName.name == HdAovTokens->instanceId) &&
                   rb->GetFormat() == HdFormatInt32) {
            _aovOutputs.push_back(
                _AovOutput{rb, _AovKind::Id, aovName.name});
        } else if (aovName.name == HdAovTokens->normal &&
                   rb->GetFormat() == HdFormatFloat32Vec3) {
            _aovOutputs.push_back(
                _AovOutput{rb, _AovKind::Normal, TfToken()});
        } else if (aovName.name == HdAovTokens->Neye &&
                   rb->GetFormat() == HdFormatFloat32Vec3) {
            _aovOutputs.push_back(
                _AovOutput{rb, _AovKind::EyeNormal, TfToken()});
        } else if (aovName.isPrimvar &&
                   rb->GetFormat() == HdFormatFloat32Vec3) {
            _aovOutputs.push_back(
                _AovOutput{rb, _AovKind::Primvar, aovName.name});
        } else if (aovName.name == HdEmbreeAovTokens->adaptiveHeatmap) {
            if (_settings.enableAdaptiveSampling &&
                !_pixelSampleCount.empty()) {
                _aovOutputs.push_back(
                    _AovOutput{
                        rb, _AovKind::AdaptiveHeatmap, TfToken()});
            }
        }
    }
}

GfVec4f
HdEmbreeRenderer::_HeatmapColor(float t)
{
    t = std::min(t, 1.0f);
    float r, g, b;
    if (t < 0.25f) {
        float s = t / 0.25f;
        r = 0.0f; g = s; b = 1.0f;
    } else if (t < 0.5f) {
        float s = (t - 0.25f) / 0.25f;
        r = 0.0f; g = 1.0f; b = 1.0f - s;
    } else if (t < 0.75f) {
        float s = (t - 0.5f) / 0.25f;
        r = s; g = 1.0f; b = 0.0f;
    } else {
        float s = (t - 0.75f) / 0.25f;
        r = 1.0f; g = 1.0f - s; b = 0.0f;
    }
    return GfVec4f(r, g, b, 1.0f);
}

void
HdEmbreeRenderer::_WriteAov(
    _AovOutput const& aov,
    RTCRayHit const& rayHit,
    GfVec4f const& color,
    unsigned int x, unsigned int y)
{
    GfVec3i const pixel(x, y, 1);
    switch (aov.kind) {
        case _AovKind::Color: {
            GfVec4f exposedColor = color;
            exposedColor[0] *= _cameraExposureScale;
            exposedColor[1] *= _cameraExposureScale;
            exposedColor[2] *= _cameraExposureScale;
            aov.buffer->Write(pixel, 4, exposedColor.data());
            break;
        }
        case _AovKind::ColorAdaptiveHeatmap: {
            // The color replacement deliberately shows the completed count
            // through WriteOutput; it differs from the dedicated heatmap.
            size_t const index = y * _width + x;
            float const fraction =
                static_cast<float>(_pixelSampleCount[index]) /
                static_cast<float>(std::max(1, _settings.samplesToConvergence));
            GfVec4f const heatmapColor = _HeatmapColor(fraction);
            aov.buffer->WriteOutput(
                pixel, 4, heatmapColor.data());
            break;
        }
        case _AovKind::CameraDepth: {
            float depth;
            if (_ComputeDepth(rayHit, &depth, false)) {
                aov.buffer->Write(pixel, 1, &depth);
            }
            break;
        }
        case _AovKind::Depth: {
            float depth;
            if (_ComputeDepth(rayHit, &depth, true)) {
                aov.buffer->Write(pixel, 1, &depth);
            }
            break;
        }
        case _AovKind::Id: {
            int32_t id;
            if (!_ComputeId(rayHit, aov.token, &id)) {
                id = -1;
            }
            aov.buffer->Write(pixel, 1, &id);
            break;
        }
        case _AovKind::Normal: {
            GfVec3f normal;
            if (_ComputeNormal(rayHit, &normal, false)) {
                aov.buffer->Write(pixel, 3, normal.data());
            }
            break;
        }
        case _AovKind::EyeNormal: {
            GfVec3f normal;
            if (_ComputeNormal(rayHit, &normal, true)) {
                aov.buffer->Write(pixel, 3, normal.data());
            }
            break;
        }
        case _AovKind::Primvar: {
            GfVec3f value;
            if (_ComputePrimvar(rayHit, aov.token, &value)) {
                aov.buffer->Write(pixel, 3, value.data());
            }
            break;
        }
        case _AovKind::AdaptiveHeatmap: {
            // The dedicated AOV deliberately accumulates count + 1 through
            // Write; it differs from the color-replacement heatmap.
            size_t const index = y * _width + x;
            float const fraction =
                static_cast<float>(_pixelSampleCount[index] + 1) /
                static_cast<float>(std::max(1, _settings.samplesToConvergence));
            GfVec4f const heatmapColor = _HeatmapColor(fraction);
            aov.buffer->Write(pixel, 4, heatmapColor.data());
            break;
        }
    }
}

void
HdEmbreeRenderer::_UpdateVariance(
    unsigned int x, unsigned int y,
    GfVec3f const& rgb)
{
    const size_t idx = y * _width + x;
    uint32_t count = ++_pixelSampleCount[idx];
    GfVec3f delta = rgb - _pixelMean[idx];
    _pixelMean[idx] += delta / static_cast<float>(count);
    GfVec3f delta2 = rgb - _pixelMean[idx];
    _pixelM2[idx] += GfCompMult(delta, delta2);

    if (count >= static_cast<uint32_t>(_settings.minSamplesBeforeAdaptive)) {
        float fCount = static_cast<float>(count);
        GfVec3f varOfMean = _pixelM2[idx] / (fCount * fCount);
        const GfVec3f &mean = _pixelMean[idx];
        if (_IsPerChannelVarianceConverged(
                varOfMean, mean, _settings.adaptiveThreshold)) {
            _pixelConverged[idx] = true;
        }
    }
}

bool
HdEmbreeRenderer::_ComputeId(RTCRayHit const& rayHit, TfToken const& idType,
                             int32_t *id)
{
    if (rayHit.hit.geomID == RTC_INVALID_GEOMETRY_ID) {
        return false;
    }
    if (_GetLightGeometryHit(rayHit)) {
        return false;
    }

    HdEmbreeInstanceContext const* instanceContext;
    HdEmbreePrototypeContext const* prototypeContext;
    if (!_GetHitContexts(
            _scene, rayHit, &instanceContext, &prototypeContext)) {
        return false;
    }

    if (idType == HdAovTokens->primId) {
        *id = prototypeContext->primId;
    } else if (idType == HdAovTokens->elementId) {
        if (prototypeContext->primitiveParams.empty()) {
            *id = rayHit.hit.primID;
        } else {
            *id = HdMeshUtil::DecodeFaceIndexFromCoarseFaceParam(
                prototypeContext->primitiveParams[rayHit.hit.primID]);
        }
    } else if (idType == HdAovTokens->instanceId) {
        *id = instanceContext->instanceId;
    } else {
        return false;
    }

    return true;
}

bool
HdEmbreeRenderer::_ComputeDepth(RTCRayHit const& rayHit,
                                float *depth,
                                bool clip)
{
    if (rayHit.hit.geomID == RTC_INVALID_GEOMETRY_ID) {
        return false;
    }

    if (clip) {
        GfVec3f hitPos = ty::CalculateHitPosition(rayHit);

        hitPos = GfVec3f(_viewMatrix.Transform(hitPos));
        hitPos = GfVec3f(_projMatrix.Transform(hitPos));

        // For the depth range transform, we assume [0,1].
        *depth = (hitPos[2] + 1.0f) / 2.0f;
    } else {
        *depth = rayHit.ray.tfar;
    }
    return true;
}

bool
HdEmbreeRenderer::_ComputeNormal(RTCRayHit const& rayHit,
                                 GfVec3f *normal,
                                 bool eye)
{
    if (rayHit.hit.geomID == RTC_INVALID_GEOMETRY_ID) {
        return false;
    }
    if (_GetLightGeometryHit(rayHit)) {
        return false;
    }

    HdEmbreeInstanceContext const* instanceContext;
    HdEmbreePrototypeContext const* prototypeContext;
    if (!_GetHitContexts(
            _scene, rayHit, &instanceContext, &prototypeContext)) {
        return false;
    }

    GfVec3f n = ty::ResolveObjectSpaceNormal(
        prototypeContext, instanceContext->rootScene, rayHit.hit.geomID,
        rayHit);

    n = ty::TransformNormalToWorld(instanceContext, n);
    if (eye) {
        n = GfVec3f(_viewMatrix.TransformDir(n));
    }
    n.Normalize();

    *normal = n;
    return true;
}

bool
HdEmbreeRenderer::_ComputePrimvar(RTCRayHit const& rayHit,
                                  TfToken const& primvar,
                                  GfVec3f *value)
{
    if (rayHit.hit.geomID == RTC_INVALID_GEOMETRY_ID) {
        return false;
    }
    if (_GetLightGeometryHit(rayHit)) {
        return false;
    }

    // Primvar sampling needs only the prototype, but a valid instance context
    // remains part of the complete hit-context invariant.
    HdEmbreeInstanceContext const* validatedInstanceContext;
    HdEmbreePrototypeContext const* prototypeContext;
    if (!_GetHitContexts(
            _scene, rayHit, &validatedInstanceContext, &prototypeContext)) {
        return false;
    }

    // XXX: This is a little clunky, although sample will early out if the
    // types don't match.
    auto it = prototypeContext->primvarMap.find(primvar);
    if (it != prototypeContext->primvarMap.end()) {
        const HdEmbreePrimvarSampler *sampler = it->second.get();
        if (sampler->Sample(rayHit.hit.primID, rayHit.hit.u, rayHit.hit.v,
                            value)) {
            return true;
        }
        GfVec2f v2;
        if (sampler->Sample(rayHit.hit.primID, rayHit.hit.u, rayHit.hit.v,
                            &v2)) {
            value->Set(v2[0], v2[1], 0.0f);
            return true;
        }
        float v1;
        if (sampler->Sample(rayHit.hit.primID, rayHit.hit.u, rayHit.hit.v,
                            &v1)) {
            value->Set(v1, 0.0f, 0.0f);
            return true;
        }
    }
    return false;
}

PXR_NAMESPACE_CLOSE_SCOPE
