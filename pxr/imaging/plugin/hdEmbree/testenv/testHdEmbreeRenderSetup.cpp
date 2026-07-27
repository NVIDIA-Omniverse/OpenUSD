//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/imaging/plugin/hdEmbree/delegate/renderDelegate.h"
#include "pxr/imaging/plugin/hdEmbree/delegate/renderPass.h"
#include "pxr/imaging/plugin/hdEmbree/renderer/renderer.h"
#include "pxr/imaging/plugin/hdEmbree/renderer/renderBuffer.h"

#include "pxr/imaging/hd/renderBuffer.h"
#include "pxr/imaging/hd/renderIndex.h"
#include "pxr/imaging/hd/renderPassState.h"
#include "pxr/imaging/hd/renderThread.h"
#include "pxr/imaging/hd/tokens.h"

#include "pxr/base/gf/vec2i.h"
#include "pxr/base/tf/diagnostic.h"

#include <embree4/rtcore.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

// Record buffer operations so setup failures can prove they do not start any
// buffer I/O or sample work.
class _CountingRenderBuffer final
    : public HdRenderBuffer
    , public HdEmbreeRenderBufferInterface
{
public:
    _CountingRenderBuffer(
        SdfPath const& id,
        unsigned int width,
        unsigned int height,
        HdFormat format)
        : HdRenderBuffer(id)
        , _width(width)
        , _height(height)
        , _format(format)
    {
    }

    bool Allocate(
        GfVec3i const& dimensions,
        HdFormat format,
        bool) override
    {
        _width = dimensions[0];
        _height = dimensions[1];
        _format = format;
        return true;
    }
    unsigned int GetWidth() const override { return _width; }
    unsigned int GetHeight() const override { return _height; }
    unsigned int GetDepth() const override { return 1; }
    HdFormat GetFormat() const override { return _format; }
    bool IsMultiSampled() const override { return true; }

    void* Map() override
    {
        ++mapCount;
        return &_storage;
    }

    void Unmap() override { ++unmapCount; }
    bool IsMapped() const override { return mapCount != unmapCount; }
    void Resolve() override { ++resolveCount; }
    bool IsConverged() const override { return converged; }
    void SetConverged(bool value) override { converged = value; }
    void ClearSamples() override {}
    void BlockFill(unsigned int) override {}

    void Write(GfVec3i const&, size_t, float const*) override;

    void Write(GfVec3i const&, size_t, int const*) override;

    void WriteOutput(GfVec3i const&, size_t, float const*) override;

    void Clear(size_t, float const*) override {}
    void Clear(size_t, int const*) override {}

    unsigned int mapCount = 0;
    unsigned int unmapCount = 0;
    unsigned int resolveCount = 0;
    unsigned int sampleWriteCount = 0;
    unsigned int floatWriteCount = 0;
    unsigned int intWriteCount = 0;
    unsigned int floatOutputWriteCount = 0;
    GfVec4f firstFloatWrite = GfVec4f(0.0f);
    GfVec4f firstFloatOutputWrite = GfVec4f(0.0f);
    int firstIntWrite = 0;
    bool converged = false;

protected:
    void _Deallocate() override {}

private:
    unsigned int _width;
    unsigned int _height;
    HdFormat _format;
    uint8_t _storage = 0;
};

void
_CountingRenderBuffer::Write(
    GfVec3i const&, size_t components, float const* value)
{
    ++sampleWriteCount;
    ++floatWriteCount;
    if (floatWriteCount == 1) {
        for (size_t i = 0; i < std::min(components, size_t(4)); ++i) {
            firstFloatWrite[i] = value[i];
        }
    }
}

void
_CountingRenderBuffer::Write(
    GfVec3i const&, size_t, int const* value)
{
    ++sampleWriteCount;
    ++intWriteCount;
    if (intWriteCount == 1) {
        firstIntWrite = value[0];
    }
}

void
_CountingRenderBuffer::WriteOutput(
    GfVec3i const&, size_t components, float const* value)
{
    ++sampleWriteCount;
    ++floatOutputWriteCount;
    if (floatOutputWriteCount == 1) {
        for (size_t i = 0; i < std::min(components, size_t(4)); ++i) {
            firstFloatOutputWrite[i] = value[i];
        }
    }
}

// Model a valid Hydra buffer owned by another delegate implementation.
class _WrongRenderBuffer final : public HdRenderBuffer
{
public:
    explicit _WrongRenderBuffer(SdfPath const& id)
        : HdRenderBuffer(id)
    {
    }

    bool Allocate(GfVec3i const&, HdFormat, bool) override { return true; }
    unsigned int GetWidth() const override { return 1; }
    unsigned int GetHeight() const override { return 1; }
    unsigned int GetDepth() const override { return 1; }
    HdFormat GetFormat() const override { return HdFormatFloat32Vec4; }
    bool IsMultiSampled() const override { return true; }
    void* Map() override
    {
        ++mapCount;
        return nullptr;
    }
    void Unmap() override { ++unmapCount; }
    bool IsMapped() const override { return mapCount != unmapCount; }
    void Resolve() override {}
    bool IsConverged() const override { return false; }

    unsigned int mapCount = 0;
    unsigned int unmapCount = 0;

protected:
    void _Deallocate() override {}
};

// Own a minimal valid Embree scene for render setup and empty-scene sampling.
class _Scene final
{
public:
    _Scene()
        : device(rtcNewDevice(nullptr))
        , scene(device != nullptr ? rtcNewScene(device) : nullptr)
    {
    }

    ~_Scene()
    {
        if (scene != nullptr) {
            rtcReleaseScene(scene);
        }
        if (device != nullptr) {
            rtcReleaseDevice(device);
        }
    }

    // Add renderer-invalid top-level geometry with no instance context.
    // Returns false if Embree cannot allocate the test geometry or buffers.
    bool AddUninstancedTriangle()
    {
        RTCGeometry geometry =
            rtcNewGeometry(device, RTC_GEOMETRY_TYPE_TRIANGLE);
        if (geometry == nullptr) {
            return false;
        }

        float* const vertices = static_cast<float*>(
            rtcSetNewGeometryBuffer(
                geometry,
                RTC_BUFFER_TYPE_VERTEX,
                0,
                RTC_FORMAT_FLOAT3,
                3 * sizeof(float),
                3));
        unsigned int* const indices = static_cast<unsigned int*>(
            rtcSetNewGeometryBuffer(
                geometry,
                RTC_BUFFER_TYPE_INDEX,
                0,
                RTC_FORMAT_UINT3,
                3 * sizeof(unsigned int),
                1));
        if (vertices == nullptr || indices == nullptr) {
            rtcReleaseGeometry(geometry);
            return false;
        }

        vertices[0] = -10.0f;
        vertices[1] = -10.0f;
        vertices[2] = -2.0f;
        vertices[3] = 10.0f;
        vertices[4] = -10.0f;
        vertices[5] = -2.0f;
        vertices[6] = 0.0f;
        vertices[7] = 10.0f;
        vertices[8] = -2.0f;
        indices[0] = 0;
        indices[1] = 1;
        indices[2] = 2;

        rtcCommitGeometry(geometry);
        unsigned int const geometryId = rtcAttachGeometry(scene, geometry);
        rtcReleaseGeometry(geometry);
        return geometryId != RTC_INVALID_GEOMETRY_ID;
    }

    RTCDevice device;
    RTCScene scene;
};

HdRenderPassAovBinding
_Binding(TfToken const& name, HdRenderBuffer* buffer)
{
    HdRenderPassAovBinding binding;
    binding.aovName = name;
    binding.renderBuffer = buffer;
    return binding;
}

void
_Configure(
    HdEmbreeRenderer* renderer,
    RTCScene scene,
    HdRenderPassAovBindingVector const& bindings,
    GfRect2i const& dataWindow)
{
    renderer->SetScene(scene);
    renderer->SetAovBindings(bindings);
    renderer->SetDataWindow(dataWindow);
    renderer->SetSamplesToConvergence(1);
    renderer->SetEnableAdaptiveSampling(false);
}

bool
_FailedWithoutWork(
    HdEmbreeRenderer* renderer,
    HdRenderThread* renderThread,
    _CountingRenderBuffer const& buffer)
{
    renderer->Render(renderThread);
    return buffer.mapCount == 0 &&
           buffer.unmapCount == 0 &&
           buffer.resolveCount == 0 &&
           buffer.sampleWriteCount == 0 &&
           buffer.converged &&
           !renderer->DidLastFrameProduceValidPixels() &&
           renderer->GetCompletedSamples() == 0;
}

bool
_TestNullBinding()
{
    _Scene scene;
    HdEmbreeRenderer renderer;
    HdRenderThread renderThread;
    _Configure(
        &renderer,
        scene.scene,
        {_Binding(HdAovTokens->color, nullptr)},
        GfRect2i(GfVec2i(0), 1, 1));

    // This call runs before Render in the render pass and must also tolerate
    // the invalid binding.
    renderer.MarkAovBuffersUnconverged();
    renderer.Render(&renderThread);
    return renderer.GetCompletedSamples() == 0;
}

bool
_TestNoBindings()
{
    _Scene scene;
    HdEmbreeRenderer renderer;
    HdRenderThread renderThread;
    _Configure(
        &renderer,
        scene.scene,
        {},
        GfRect2i(GfVec2i(0), 1, 1));
    renderer.Render(&renderThread);
    return renderer.GetCompletedSamples() == 0;
}

bool
_TestWrongBufferImplementation()
{
    _Scene scene;
    _WrongRenderBuffer buffer(SdfPath("/wrong"));
    HdEmbreeRenderer renderer;
    HdRenderThread renderThread;
    _Configure(
        &renderer,
        scene.scene,
        {_Binding(HdAovTokens->color, &buffer)},
        GfRect2i(GfVec2i(0), 1, 1));

    renderer.MarkAovBuffersUnconverged();
    renderer.Render(&renderThread);
    return buffer.mapCount == 0 &&
           buffer.unmapCount == 0 &&
           renderer.GetCompletedSamples() == 0;
}

bool
_TestUnsupportedFormat()
{
    _Scene scene;
    _CountingRenderBuffer buffer(
        SdfPath("/unsupported"), 1, 1, HdFormatInt32);
    HdEmbreeRenderer renderer;
    HdRenderThread renderThread;
    _Configure(
        &renderer,
        scene.scene,
        {_Binding(HdAovTokens->color, &buffer)},
        GfRect2i(GfVec2i(0), 1, 1));
    return _FailedWithoutWork(&renderer, &renderThread, buffer);
}

bool
_TestZeroSizedBuffer()
{
    _Scene scene;
    _CountingRenderBuffer buffer(
        SdfPath("/zeroBuffer"), 0, 1, HdFormatFloat32Vec4);
    HdEmbreeRenderer renderer;
    HdRenderThread renderThread;
    _Configure(
        &renderer,
        scene.scene,
        {_Binding(HdAovTokens->color, &buffer)},
        GfRect2i(GfVec2i(0), 1, 1));
    return _FailedWithoutWork(&renderer, &renderThread, buffer);
}

bool
_TestMismatchedDimensions()
{
    _Scene scene;
    _CountingRenderBuffer color(
        SdfPath("/color"), 2, 2, HdFormatFloat32Vec4);
    _CountingRenderBuffer depth(
        SdfPath("/depth"), 1, 2, HdFormatFloat32);
    HdEmbreeRenderer renderer;
    HdRenderThread renderThread;
    _Configure(
        &renderer,
        scene.scene,
        {
            _Binding(HdAovTokens->color, &color),
            _Binding(HdAovTokens->depth, &depth)
        },
        GfRect2i(GfVec2i(0), 1, 1));
    renderer.Render(&renderThread);
    return color.mapCount == 0 &&
           color.unmapCount == 0 &&
           color.resolveCount == 0 &&
           color.sampleWriteCount == 0 &&
           depth.mapCount == 0 &&
           depth.unmapCount == 0 &&
           depth.resolveCount == 0 &&
           depth.sampleWriteCount == 0 &&
           color.converged &&
           depth.converged &&
           renderer.GetCompletedSamples() == 0;
}

bool
_TestBufferPropertiesAreRevalidated()
{
    _Scene scene;
    _CountingRenderBuffer buffer(
        SdfPath("/revalidation"), 1, 1, HdFormatFloat32Vec4);
    HdEmbreeRenderer renderer;
    HdRenderThread renderThread;
    _Configure(
        &renderer,
        scene.scene,
        {_Binding(HdAovTokens->color, &buffer)},
        GfRect2i(GfVec2i(0), 1, 1));

    // A bound buffer can change without SetAovBindings being called. The
    // second render must reject its new format rather than reuse old validity.
    renderer.Render(&renderThread);
    buffer.Allocate(GfVec3i(1, 1, 1), HdFormatInt32, true);
    buffer.converged = false;
    renderer.Render(&renderThread);
    return buffer.mapCount == 1 &&
           buffer.unmapCount == 1 &&
           buffer.resolveCount == 1 &&
           buffer.sampleWriteCount == 1 &&
           buffer.converged &&
           renderer.GetCompletedSamples() == 0;
}

bool
_TestOutOfBoundsDataWindow()
{
    _Scene scene;
    _CountingRenderBuffer buffer(
        SdfPath("/outOfBounds"), 1, 1, HdFormatFloat32Vec4);
    HdEmbreeRenderer renderer;
    HdRenderThread renderThread;
    _Configure(
        &renderer,
        scene.scene,
        {_Binding(HdAovTokens->color, &buffer)},
        GfRect2i(GfVec2i(1, 0), 1, 1));
    return _FailedWithoutWork(&renderer, &renderThread, buffer);
}

bool
_TestEmptyDataWindow()
{
    _Scene scene;
    _CountingRenderBuffer buffer(
        SdfPath("/emptyWindow"), 1, 1, HdFormatFloat32Vec4);
    HdEmbreeRenderer renderer;
    HdRenderThread renderThread;
    _Configure(
        &renderer,
        scene.scene,
        {_Binding(HdAovTokens->color, &buffer)},
        GfRect2i(GfVec2i(0), 0, 1));
    return _FailedWithoutWork(&renderer, &renderThread, buffer);
}

bool
_TestNullScene()
{
    _CountingRenderBuffer buffer(
        SdfPath("/nullScene"), 1, 1, HdFormatFloat32Vec4);
    HdEmbreeRenderer renderer;
    HdRenderThread renderThread;
    _Configure(
        &renderer,
        nullptr,
        {_Binding(HdAovTokens->color, &buffer)},
        GfRect2i(GfVec2i(0), 1, 1));
    return _FailedWithoutWork(&renderer, &renderThread, buffer);
}

bool
_TestSuccessfulMapBalance()
{
    _Scene scene;
    _CountingRenderBuffer color(
        SdfPath("/successColor"), 1, 1, HdFormatFloat32Vec4);
    _CountingRenderBuffer primId(
        SdfPath("/successPrimId"), 1, 1, HdFormatInt32);
    HdEmbreeRenderer renderer;
    HdRenderThread renderThread;
    _Configure(
        &renderer,
        scene.scene,
        {
            _Binding(HdAovTokens->color, &color),
            _Binding(HdAovTokens->primId, &primId)
        },
        GfRect2i(GfVec2i(0), 1, 1));

    renderer.Render(&renderThread);
    return color.mapCount == 1 &&
           color.unmapCount == 1 &&
           color.resolveCount == 1 &&
           color.sampleWriteCount == 1 &&
           color.converged &&
           primId.mapCount == 1 &&
           primId.unmapCount == 1 &&
           primId.resolveCount == 1 &&
           primId.sampleWriteCount == 1 &&
           primId.converged &&
           renderer.DidLastFrameProduceValidPixels() &&
           renderer.GetCompletedSamples() == 1;
}

bool
_TestAovOutputDispatch()
{
    _Scene scene;
    GfRect2i const dataWindow(GfVec2i(0), 1, 1);

    // Ordinary color uses multisampled accumulation and applies exposure to
    // RGB without changing alpha.
    {
        _CountingRenderBuffer color(
            SdfPath("/dispatchColor"), 1, 1, HdFormatFloat32Vec4);
        HdRenderPassAovBinding binding =
            _Binding(HdAovTokens->color, &color);
        binding.clearValue = VtValue(GfVec4f(0.25f, 0.5f, 0.75f, 0.8f));
        HdEmbreeRenderer renderer;
        HdRenderThread renderThread;
        _Configure(&renderer, scene.scene, {binding}, dataWindow);
        renderer.SetCameraExposureScale(2.0f);
        if (!TF_VERIFY(!color.converged)) {
            return false;
        }
        renderThread.StartRender();
        renderer.Render(&renderThread);
        if (!TF_VERIFY(
                renderer.GetCompletedSamples() == 1,
                "completedSamples=%d", renderer.GetCompletedSamples()) ||
            !TF_VERIFY(color.mapCount == 1, "mapCount=%u", color.mapCount) ||
            !TF_VERIFY(
                color.floatWriteCount == 1,
                "floatWriteCount=%u", color.floatWriteCount) ||
            !TF_VERIFY(
                color.floatOutputWriteCount == 0,
                "floatOutputWriteCount=%u", color.floatOutputWriteCount) ||
            !TF_VERIFY(
                color.firstFloatWrite ==
                GfVec4f(0.5f, 1.0f, 1.5f, 1.0f),
                "firstFloatWrite=(%g, %g, %g, %g)",
                color.firstFloatWrite[0],
                color.firstFloatWrite[1],
                color.firstFloatWrite[2],
                color.firstFloatWrite[3])) {
            return false;
        }
    }

    // ID misses write -1, while other geometric misses retain their cleared
    // output by issuing no sample write.
    {
        _CountingRenderBuffer primId(
            SdfPath("/dispatchPrimId"), 1, 1, HdFormatInt32);
        _CountingRenderBuffer elementId(
            SdfPath("/dispatchElementId"), 1, 1, HdFormatInt32);
        _CountingRenderBuffer instanceId(
            SdfPath("/dispatchInstanceId"), 1, 1, HdFormatInt32);
        _CountingRenderBuffer cameraDepth(
            SdfPath("/dispatchCameraDepth"), 1, 1, HdFormatFloat32);
        _CountingRenderBuffer depth(
            SdfPath("/dispatchDepth"), 1, 1, HdFormatFloat32);
        _CountingRenderBuffer normal(
            SdfPath("/dispatchNormal"), 1, 1, HdFormatFloat32Vec3);
        _CountingRenderBuffer eyeNormal(
            SdfPath("/dispatchEyeNormal"), 1, 1, HdFormatFloat32Vec3);
        _CountingRenderBuffer primvar(
            SdfPath("/dispatchPrimvar"), 1, 1, HdFormatFloat32Vec3);
        HdEmbreeRenderer renderer;
        HdRenderThread renderThread;
        _Configure(
            &renderer,
            scene.scene,
            {
                _Binding(HdAovTokens->primId, &primId),
                _Binding(HdAovTokens->elementId, &elementId),
                _Binding(HdAovTokens->instanceId, &instanceId),
                _Binding(HdAovTokens->cameraDepth, &cameraDepth),
                _Binding(HdAovTokens->depth, &depth),
                _Binding(HdAovTokens->normal, &normal),
                _Binding(HdAovTokens->Neye, &eyeNormal),
                _Binding(TfToken("primvars:displayColor"), &primvar)
            },
            dataWindow);
        renderThread.StartRender();
        renderer.Render(&renderThread);
        if (!TF_VERIFY(primId.intWriteCount == 1) ||
            !TF_VERIFY(primId.firstIntWrite == -1) ||
            !TF_VERIFY(elementId.intWriteCount == 1) ||
            !TF_VERIFY(elementId.firstIntWrite == -1) ||
            !TF_VERIFY(instanceId.intWriteCount == 1) ||
            !TF_VERIFY(instanceId.firstIntWrite == -1) ||
            !TF_VERIFY(cameraDepth.sampleWriteCount == 0) ||
            !TF_VERIFY(depth.sampleWriteCount == 0) ||
            !TF_VERIFY(normal.sampleWriteCount == 0) ||
            !TF_VERIFY(eyeNormal.sampleWriteCount == 0) ||
            !TF_VERIFY(primvar.sampleWriteCount == 0)) {
            return false;
        }
    }

    // The color-replacement heatmap bypasses accumulation and uses the
    // current sample count.
    {
        _CountingRenderBuffer color(
            SdfPath("/dispatchColorHeatmap"), 1, 1, HdFormatFloat32Vec4);
        HdEmbreeRenderer renderer;
        HdRenderThread renderThread;
        _Configure(
            &renderer,
            scene.scene,
            {_Binding(HdAovTokens->color, &color)},
            dataWindow);
        renderer.SetSamplesToConvergence(4);
        renderer.SetEnableAdaptiveSampling(true);
        renderer.SetMinSamplesBeforeAdaptive(5);
        renderer.SetShowAdaptiveHeatmap(true);
        renderThread.StartRender();
        renderer.Render(&renderThread);
        if (!TF_VERIFY(
                color.floatWriteCount == 0,
                "floatWriteCount=%u", color.floatWriteCount) ||
            !TF_VERIFY(
                color.floatOutputWriteCount == 4,
                "floatOutputWriteCount=%u", color.floatOutputWriteCount) ||
            !TF_VERIFY(
                color.firstFloatOutputWrite ==
                GfVec4f(0.0f, 1.0f, 1.0f, 1.0f))) {
            return false;
        }
    }

    // Adaptive-only output is omitted while disabled, and requesting the
    // color heatmap still falls back to ordinary color.
    {
        _CountingRenderBuffer heatmap(
            SdfPath("/dispatchDisabledAdaptiveHeatmap"),
            1, 1, HdFormatFloat32Vec4);
        _CountingRenderBuffer color(
            SdfPath("/dispatchDisabledColorHeatmap"),
            1, 1, HdFormatFloat32Vec4);
        HdRenderPassAovBinding colorBinding =
            _Binding(HdAovTokens->color, &color);
        colorBinding.clearValue = VtValue(GfVec4f(0.25f));
        HdEmbreeRenderer renderer;
        HdRenderThread renderThread;
        _Configure(
            &renderer,
            scene.scene,
            {
                _Binding(
                    HdEmbreeAovTokens->adaptiveHeatmap, &heatmap),
                colorBinding
            },
            dataWindow);
        renderer.SetShowAdaptiveHeatmap(true);
        renderThread.StartRender();
        renderer.Render(&renderThread);
        if (!TF_VERIFY(heatmap.sampleWriteCount == 0) ||
            !TF_VERIFY(color.floatWriteCount == 1) ||
            !TF_VERIFY(color.floatOutputWriteCount == 0)) {
            return false;
        }
    }

    // The dedicated heatmap accumulates samples and visualizes count + 1.
    {
        _CountingRenderBuffer heatmap(
            SdfPath("/dispatchAdaptiveHeatmap"),
            1, 1, HdFormatFloat32Vec4);
        HdEmbreeRenderer renderer;
        HdRenderThread renderThread;
        _Configure(
            &renderer,
            scene.scene,
            {_Binding(HdEmbreeAovTokens->adaptiveHeatmap, &heatmap)},
            dataWindow);
        renderer.SetSamplesToConvergence(4);
        renderer.SetEnableAdaptiveSampling(true);
        renderer.SetMinSamplesBeforeAdaptive(5);
        renderThread.StartRender();
        renderer.Render(&renderThread);
        if (!TF_VERIFY(
                heatmap.floatWriteCount == 4,
                "floatWriteCount=%u", heatmap.floatWriteCount) ||
            !TF_VERIFY(
                heatmap.floatOutputWriteCount == 0,
                "floatOutputWriteCount=%u", heatmap.floatOutputWriteCount) ||
            !TF_VERIFY(
                heatmap.firstFloatWrite ==
                GfVec4f(0.0f, 1.0f, 0.0f, 1.0f))) {
            return false;
        }
    }

    return true;
}

bool
_TestInvalidHitContextsBecomeMisses()
{
    _Scene scene;
    if (!TF_VERIFY(scene.AddUninstancedTriangle())) {
        return false;
    }

    _CountingRenderBuffer primId(
        SdfPath("/invalidContextPrimId"), 1, 1, HdFormatInt32);
    _CountingRenderBuffer normal(
        SdfPath("/invalidContextNormal"), 1, 1, HdFormatFloat32Vec3);
    _CountingRenderBuffer primvar(
        SdfPath("/invalidContextPrimvar"), 1, 1, HdFormatFloat32Vec3);
    HdEmbreeRenderer renderer;
    HdRenderThread renderThread;
    _Configure(
        &renderer,
        scene.scene,
        {
            _Binding(HdAovTokens->primId, &primId),
            _Binding(HdAovTokens->normal, &normal),
            _Binding(TfToken("primvars:displayColor"), &primvar)
        },
        GfRect2i(GfVec2i(0), 1, 1));
    renderThread.StartRender();
    renderer.Render(&renderThread);

    // A top-level hit has a valid geomID but no instID[0]. It must take each
    // AOV's normal miss path without dereferencing an instance context.
    return TF_VERIFY(primId.intWriteCount == 1) &&
           TF_VERIFY(primId.firstIntWrite == -1) &&
           TF_VERIFY(normal.sampleWriteCount == 0) &&
           TF_VERIFY(primvar.sampleWriteCount == 0);
}

bool
_TestFrameStatusTransitions()
{
    _Scene scene;
    _CountingRenderBuffer buffer(
        SdfPath("/frameStatus"), 1, 1, HdFormatFloat32Vec4);
    HdEmbreeRenderer renderer;
    HdRenderThread renderThread;
    _Configure(
        &renderer,
        scene.scene,
        {_Binding(HdAovTokens->color, &buffer)},
        GfRect2i(GfVec2i(0), 1, 1));

    // A new renderer and a restarted valid frame must not expose validity
    // until the new Render invocation passes setup.
    if (renderer.DidLastFrameProduceValidPixels()) {
        return false;
    }
    renderer.Render(&renderThread);
    if (!renderer.DidLastFrameProduceValidPixels()) {
        return false;
    }
    renderer.MarkFramePending();
    if (renderer.DidLastFrameProduceValidPixels()) {
        return false;
    }

    // A terminal setup failure must replace Pending with Failed.
    buffer.Allocate(GfVec3i(1, 1, 1), HdFormatInt32, true);
    buffer.converged = false;
    renderer.Render(&renderThread);
    return !renderer.DidLastFrameProduceValidPixels() &&
           buffer.converged;
}

bool
_TestRenderPassMarksRestartPending()
{
    _Scene scene;
    _CountingRenderBuffer buffer(
        SdfPath("/pendingRestart"), 1, 1, HdFormatFloat32Vec4);
    HdEmbreeRenderer renderer;
    HdRenderThread statusThread;
    _Configure(
        &renderer,
        scene.scene,
        {_Binding(HdAovTokens->color, &buffer)},
        GfRect2i(GfVec2i(0), 1, 1));

    // Establish a valid previous frame before the render pass restarts it.
    statusThread.StartRender();
    renderer.Render(&statusThread);
    statusThread.StopRender();
    if (!renderer.DidLastFrameProduceValidPixels()) {
        return false;
    }

    HdEmbreeRenderDelegate delegate;
    std::unique_ptr<HdRenderIndex> renderIndex(
        HdRenderIndex::New(&delegate, HdDriverVector()));
    if (!renderIndex) {
        return false;
    }

    std::atomic<int> sceneVersion{1};
    std::atomic<int> displacementVersion{0};
    std::atomic<bool> callbackEntered{false};
    std::atomic<bool> releaseCallback{false};
    HdRenderThread renderThread;
    renderThread.SetRenderCallback([&]() {
        callbackEntered.store(true, std::memory_order_release);
        while (!releaseCallback.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    });
    renderThread.StartThread();

    HdEmbreeRenderPass renderPass(
        renderIndex.get(),
        HdRprimCollection(),
        &renderThread,
        &renderer,
        &sceneVersion,
        &displacementVersion);
    HdRenderPassStateSharedPtr renderPassState =
        delegate.CreateRenderPassState();
    renderPassState->SetViewport(GfVec4d(0.0, 0.0, 1.0, 1.0));
    renderPassState->SetAovBindings(
        {_Binding(HdAovTokens->color, &buffer)});
    renderPass.Execute(renderPassState, TfTokenVector());

    for (int i = 0;
         i != 500 &&
             !callbackEntered.load(std::memory_order_acquire);
         ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const bool entered =
        callbackEntered.load(std::memory_order_acquire);
    const bool pending =
        !renderer.DidLastFrameProduceValidPixels();

    releaseCallback.store(true, std::memory_order_release);
    renderThread.StopThread();
    return entered && pending;
}

} // anonymous namespace

int
main()
{
    TF_AXIOM(_TestNullBinding());
    TF_AXIOM(_TestNoBindings());
    TF_AXIOM(_TestWrongBufferImplementation());
    TF_AXIOM(_TestUnsupportedFormat());
    TF_AXIOM(_TestZeroSizedBuffer());
    TF_AXIOM(_TestMismatchedDimensions());
    TF_AXIOM(_TestBufferPropertiesAreRevalidated());
    TF_AXIOM(_TestOutOfBoundsDataWindow());
    TF_AXIOM(_TestEmptyDataWindow());
    TF_AXIOM(_TestNullScene());
    TF_AXIOM(_TestSuccessfulMapBalance());
    TF_AXIOM(_TestAovOutputDispatch());
    TF_AXIOM(_TestInvalidHitContextsBecomeMisses());
    TF_AXIOM(_TestFrameStatusTransitions());
    TF_AXIOM(_TestRenderPassMarksRestartPending());
    return 0;
}
