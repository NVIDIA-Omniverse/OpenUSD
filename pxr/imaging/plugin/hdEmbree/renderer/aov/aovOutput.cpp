//
// Copyright 2018 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
// AOV validation, accumulation, dispatch, and hit outputs.

#include "pxr/imaging/plugin/hdEmbree/renderer/renderer.h"
#include "../rendererImpl.h"
#include "pxr/imaging/plugin/hdEmbree/renderer/renderBuffer.h"

#include "pxr/imaging/hd/perfLog.h"
#include "pxr/imaging/hd/renderBuffer.h"
#include "pxr/base/work/loops.h"
#include "pxr/base/work/threadLimits.h"

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

    // Re-validate the attachments.
    _aovBindingsNeedValidation = true;
}

bool
HdEmbreeRenderer::_ValidateAovBindings()
{
    if (!_aovBindingsNeedValidation) {
        return _aovBindingsValid;
    }

    _aovBindingsNeedValidation = false;
    _aovBindingsValid = true;

    for (size_t i = 0; i < _aovBindings.size(); ++i) {
        // By the time the attachment gets here, there should be a bound
        // output buffer.
        if (_aovBindings[i].renderBuffer == nullptr) {
            TF_WARN("Aov '%s' doesn't have any renderbuffer bound",
                    _aovNames[i].name.GetText());
            _aovBindingsValid = false;
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

        HdFormat format = _aovBindings[i].renderBuffer->GetFormat();

        // depth is only supported for float32 attachments
        if ((_aovNames[i].name == HdAovTokens->cameraDepth ||
             _aovNames[i].name == HdAovTokens->depth) &&
            format != HdFormatFloat32) {
            TF_WARN("Aov '%s' has unsupported format '%s'",
                    _aovNames[i].name.GetText(),
                    TfEnum::GetName(format).c_str());
            _aovBindingsValid = false;
        }

        // ids are only supported for int32 attachments
        if ((_aovNames[i].name == HdAovTokens->primId ||
             _aovNames[i].name == HdAovTokens->instanceId ||
             _aovNames[i].name == HdAovTokens->elementId) &&
            format != HdFormatInt32) {
            TF_WARN("Aov '%s' has unsupported format '%s'",
                    _aovNames[i].name.GetText(),
                    TfEnum::GetName(format).c_str());
            _aovBindingsValid = false;
        }

        // Normal is only supported for vec3 attachments of float.
        if ((_aovNames[i].name == HdAovTokens->Neye ||
             _aovNames[i].name == HdAovTokens->normal) &&
            format != HdFormatFloat32Vec3) {
            TF_WARN("Aov '%s' has unsupported format '%s'",
                    _aovNames[i].name.GetText(),
                    TfEnum::GetName(format).c_str());
            _aovBindingsValid = false;
        }

        // Primvars support vec3 output (though some channels may not be used).
        if (_aovNames[i].isPrimvar &&
            format != HdFormatFloat32Vec3) {
            TF_WARN("Aov 'primvars:%s' has unsupported format '%s'",
                    _aovNames[i].name.GetText(),
                    TfEnum::GetName(format).c_str());
            _aovBindingsValid = false;
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
                    _aovBindingsValid = false;
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
                _aovBindingsValid = false;
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
                _aovBindingsValid = false;
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
                _aovBindingsValid = false;
            }
        }
    }

    return _aovBindingsValid;
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
            dynamic_cast<HdEmbreeRenderBufferInterface*>(_aovBindings[i].renderBuffer);

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
            dynamic_cast<HdEmbreeRenderBufferInterface*>(_aovBindings[i].renderBuffer);
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
            dynamic_cast<HdEmbreeRenderBufferInterface*>(_aovBindings[i].renderBuffer);
        rb->SetConverged(false);
    }
}

void
HdEmbreeRenderer::_BuildAovDispatchTable()
{
    _aovWriters.clear();
    _needColor = _enableAdaptiveSampling;
    _colorClearValue = GfVec4f(0.0f);

    // Find color clear value and set _needColor.
    for (size_t i = 0; i < _aovNames.size(); ++i) {
        if (_aovNames[i].name == HdAovTokens->color) {
            _needColor = true;
            _colorClearValue = _GetClearColor(_aovBindings[i].clearValue);
            break;
        }
    }

    // Build writer table.
    for (size_t i = 0; i < _aovBindings.size(); ++i) {
        HdEmbreeRenderBufferInterface *rb = dynamic_cast<HdEmbreeRenderBufferInterface*>(
            _aovBindings[i].renderBuffer);
        const auto& aovName = _aovNames[i];

        if (aovName.name == HdAovTokens->color) {
            _AovWriteFn fn = (_showAdaptiveHeatmap
                && _enableAdaptiveSampling
                && !_pixelSampleCount.empty())
                ? &_WriteColorHeatmap : &_WriteColor;
            _aovWriters.push_back(_AovWriter{rb, fn, {}});
        } else if (aovName.name == HdAovTokens->cameraDepth &&
                   rb->GetFormat() == HdFormatFloat32) {
            _aovWriters.push_back(_AovWriter{rb, &_WriteDepth, {}});
        } else if (aovName.name == HdAovTokens->depth &&
                   rb->GetFormat() == HdFormatFloat32) {
            _aovWriters.push_back(_AovWriter{rb, &_WriteClipDepth, {}});
        } else if ((aovName.name == HdAovTokens->primId ||
                    aovName.name == HdAovTokens->elementId ||
                    aovName.name == HdAovTokens->instanceId) &&
                   rb->GetFormat() == HdFormatInt32) {
            _aovWriters.push_back(_AovWriter{rb, &_WriteId, aovName.name});
        } else if (aovName.name == HdAovTokens->normal &&
                   rb->GetFormat() == HdFormatFloat32Vec3) {
            _aovWriters.push_back(_AovWriter{rb, &_WriteNormal, {}});
        } else if (aovName.name == HdAovTokens->Neye &&
                   rb->GetFormat() == HdFormatFloat32Vec3) {
            _aovWriters.push_back(_AovWriter{rb, &_WriteNormalEye, {}});
        } else if (aovName.isPrimvar &&
                   rb->GetFormat() == HdFormatFloat32Vec3) {
            _aovWriters.push_back(_AovWriter{rb, &_WritePrimvar, aovName.name});
        } else if (aovName.name == HdEmbreeAovTokens->adaptiveHeatmap) {
            if (_enableAdaptiveSampling && !_pixelSampleCount.empty()) {
                _aovWriters.push_back(
                    _AovWriter{rb, &_WriteAdaptiveHeatmap, {}});
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
HdEmbreeRenderer::_WriteColor(
    HdEmbreeRenderer* self, _AovWriter const& w,
    RTCRayHit const&, GfVec4f const& color,
    unsigned int x, unsigned int y)
{
    GfVec4f exposedColor = color;
    exposedColor[0] *= self->_cameraExposureScale;
    exposedColor[1] *= self->_cameraExposureScale;
    exposedColor[2] *= self->_cameraExposureScale;
    w.buffer->Write(GfVec3i(x, y, 1), 4, exposedColor.data());
}

void
HdEmbreeRenderer::_WriteColorHeatmap(
    HdEmbreeRenderer* self, _AovWriter const& w,
    RTCRayHit const&, GfVec4f const&,
    unsigned int x, unsigned int y)
{
    const size_t idx = y * self->_width + x;
    float t = static_cast<float>(self->_pixelSampleCount[idx])
            / static_cast<float>(std::max(1, self->_samplesToConvergence));
    GfVec4f heatColor = _HeatmapColor(t);
    w.buffer->WriteOutput(GfVec3i(x, y, 1), 4, heatColor.data());
}

void
HdEmbreeRenderer::_WriteDepth(
    HdEmbreeRenderer* self, _AovWriter const& w,
    RTCRayHit const& rayHit, GfVec4f const&,
    unsigned int x, unsigned int y)
{
    float depth;
    if (self->_ComputeDepth(rayHit, &depth, false)) {
        w.buffer->Write(GfVec3i(x, y, 1), 1, &depth);
    }
}

void
HdEmbreeRenderer::_WriteClipDepth(
    HdEmbreeRenderer* self, _AovWriter const& w,
    RTCRayHit const& rayHit, GfVec4f const&,
    unsigned int x, unsigned int y)
{
    float depth;
    if (self->_ComputeDepth(rayHit, &depth, true)) {
        w.buffer->Write(GfVec3i(x, y, 1), 1, &depth);
    }
}

void
HdEmbreeRenderer::_WriteId(
    HdEmbreeRenderer* self, _AovWriter const& w,
    RTCRayHit const& rayHit, GfVec4f const&,
    unsigned int x, unsigned int y)
{
    int32_t id;
    if (!self->_ComputeId(rayHit, w.token, &id)) {
        id = -1;
    }
    w.buffer->Write(GfVec3i(x, y, 1), 1, &id);
}

void
HdEmbreeRenderer::_WriteNormal(
    HdEmbreeRenderer* self, _AovWriter const& w,
    RTCRayHit const& rayHit, GfVec4f const&,
    unsigned int x, unsigned int y)
{
    GfVec3f normal;
    if (self->_ComputeNormal(rayHit, &normal, false)) {
        w.buffer->Write(GfVec3i(x, y, 1), 3, normal.data());
    }
}

void
HdEmbreeRenderer::_WriteNormalEye(
    HdEmbreeRenderer* self, _AovWriter const& w,
    RTCRayHit const& rayHit, GfVec4f const&,
    unsigned int x, unsigned int y)
{
    GfVec3f normal;
    if (self->_ComputeNormal(rayHit, &normal, true)) {
        w.buffer->Write(GfVec3i(x, y, 1), 3, normal.data());
    }
}

void
HdEmbreeRenderer::_WritePrimvar(
    HdEmbreeRenderer* self, _AovWriter const& w,
    RTCRayHit const& rayHit, GfVec4f const&,
    unsigned int x, unsigned int y)
{
    GfVec3f value;
    if (self->_ComputePrimvar(rayHit, w.token, &value)) {
        w.buffer->Write(GfVec3i(x, y, 1), 3, value.data());
    }
}

void
HdEmbreeRenderer::_WriteAdaptiveHeatmap(
    HdEmbreeRenderer* self, _AovWriter const& w,
    RTCRayHit const&, GfVec4f const&,
    unsigned int x, unsigned int y)
{
    const size_t idx = y * self->_width + x;
    float t = static_cast<float>(self->_pixelSampleCount[idx] + 1)
            / static_cast<float>(std::max(1, self->_samplesToConvergence));
    GfVec4f heatColor = _HeatmapColor(t);
    w.buffer->Write(GfVec3i(x, y, 1), 4, heatColor.data());
}

void
HdEmbreeRenderer::_UpdateVariance(
    HdEmbreeRenderer* self,
    unsigned int x, unsigned int y,
    GfVec3f const& rgb)
{
    const size_t idx = y * self->_width + x;
    uint32_t count = ++self->_pixelSampleCount[idx];
    GfVec3f delta = rgb - self->_pixelMean[idx];
    self->_pixelMean[idx] += delta / static_cast<float>(count);
    GfVec3f delta2 = rgb - self->_pixelMean[idx];
    self->_pixelM2[idx] += GfCompMult(delta, delta2);

    if (count >= static_cast<uint32_t>(self->_minSamplesBeforeAdaptive)) {
        float fCount = static_cast<float>(count);
        GfVec3f varOfMean = self->_pixelM2[idx] / (fCount * fCount);
        const GfVec3f &mean = self->_pixelMean[idx];
        if (_IsPerChannelVarianceConverged(
                varOfMean, mean, self->_adaptiveThreshold)) {
            self->_pixelConverged[idx] = true;
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

    // Get the instance and prototype context structures for the hit prim.
    // We don't use embree's multi-level instancing; we
    // flatten everything in hydra. So instID[0] should always be correct.
    const HdEmbreeInstanceContext *instanceContext =
        static_cast<HdEmbreeInstanceContext*>(
            rtcGetGeometryUserData(rtcGetGeometry(_scene,
                                                  rayHit.hit.instID[0])));

    const HdEmbreePrototypeContext *prototypeContext =
        static_cast<HdEmbreePrototypeContext*>(
            rtcGetGeometryUserData(rtcGetGeometry(instanceContext->rootScene,
                                                  rayHit.hit.geomID)));

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
        GfVec3f hitPos = _CalculateHitPosition(rayHit);

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

    // We don't use embree's multi-level instancing; we
    // flatten everything in hydra. So instID[0] should always be correct.
    const HdEmbreeInstanceContext *instanceContext =
        static_cast<HdEmbreeInstanceContext*>(
                rtcGetGeometryUserData(rtcGetGeometry(_scene,
                                                      rayHit.hit.instID[0])));

    const HdEmbreePrototypeContext *prototypeContext =
        static_cast<HdEmbreePrototypeContext*>(
                rtcGetGeometryUserData(
                    rtcGetGeometry(instanceContext->rootScene,
                                   rayHit.hit.geomID)));

    GfVec3f n = _ResolveObjectSpaceNormal(
        prototypeContext, instanceContext->rootScene, rayHit.hit.geomID,
        rayHit);

    n = _TransformNormalToWorld(instanceContext, n);
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

    // We don't use embree's multi-level instancing; we
    // flatten everything in hydra. So instID[0] should always be correct.
    const HdEmbreeInstanceContext *instanceContext =
        static_cast<HdEmbreeInstanceContext*>(
                rtcGetGeometryUserData(rtcGetGeometry(_scene,
                                                      rayHit.hit.instID[0])));

    const HdEmbreePrototypeContext *prototypeContext =
        static_cast<HdEmbreePrototypeContext*>(
                rtcGetGeometryUserData(
                    rtcGetGeometry(instanceContext->rootScene,
                                   rayHit.hit.geomID)));

    // XXX: This is a little clunky, although sample will early out if the
    // types don't match.
    auto it = prototypeContext->primvarMap.find(primvar);
    if (it != prototypeContext->primvarMap.end()) {
        const HdEmbreePrimvarSampler *sampler = it->second;
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
