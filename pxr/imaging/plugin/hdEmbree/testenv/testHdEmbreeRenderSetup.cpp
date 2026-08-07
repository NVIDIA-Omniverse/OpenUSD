//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include <delegate/renderBuffer.h>
#include <delegate/renderDelegate.h>
#include <delegate/renderParam.h>
#include <delegate/renderPass.h>
#include <renderer/lights/light.h>
#include <renderer/materials/MaterialXCpp/materials/bsdf.h>
#include <renderer/renderBuffer.h>
#include <renderer/renderer.h>

#include "pxr/base/gf/colorSpace.h"
#include "pxr/base/gf/frustum.h"
#include "pxr/base/gf/math.h"
#include "pxr/base/gf/matrix3f.h"
#include "pxr/base/gf/matrix4f.h"
#include "pxr/base/gf/vec2i.h"
#include "pxr/base/tf/diagnostic.h"
#include "pxr/base/tf/diagnosticMgr.h"
#include "pxr/imaging/cameraUtil/framing.h"
#include "pxr/imaging/hd/material.h"
#include "pxr/imaging/hd/renderBuffer.h"
#include "pxr/imaging/hd/renderIndex.h"
#include "pxr/imaging/hd/renderPassState.h"
#include "pxr/imaging/hd/renderThread.h"
#include "pxr/imaging/hd/rprim.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/hd/unitTestDelegate.h"

#include <embree4/rtcore.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

// Record buffer operations so setup failures can prove they do not start any
// buffer I/O or sample work.
class _CountingRenderBuffer final
    : public HdRenderBuffer
    , public ty::RenderBufferInterface
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

// Capture the warnings that define fallback behavior for authored tokens.
// Unexpected diagnostics are counted so none can be hidden by the delegate.
class _ScopedTokenWarningDelegate final
    : public TfDiagnosticMgr::Delegate
{
public:
    _ScopedTokenWarningDelegate()
    {
        TfDiagnosticMgr::GetInstance().AddDelegate(this);
    }

    ~_ScopedTokenWarningDelegate() override
    {
        TfDiagnosticMgr::GetInstance().RemoveDelegate(this);
    }

    void IssueError(TfError const&) override
    {
        ++unexpectedDiagnostics;
    }
    void IssueFatalError(
        TfCallContext const&, std::string const&) override
    {
        ++unexpectedDiagnostics;
    }
    void IssueStatus(TfStatus const&) override
    {
        ++unexpectedDiagnostics;
    }

    void IssueWarning(TfWarning const& warning) override
    {
        const std::string& commentary = warning.GetCommentary();
        if (commentary.find("dielectric layer throughput mode") !=
            std::string::npos) {
            ++dielectricWarnings;
        } else {
            ++unexpectedDiagnostics;
        }
    }

    unsigned int dielectricWarnings = 0;
    unsigned int unexpectedDiagnostics = 0;
};

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
    ty::Renderer* renderer,
    RTCScene scene,
    HdRenderPassAovBindingVector const& bindings,
    GfRect2i const& dataWindow)
{
    renderer->SetScene(scene);
    renderer->SetAovBindings(bindings);
    renderer->SetDataWindow(dataWindow);
    ty::RenderSettings settings;
    settings.samplesToConvergence = 1;
    // Every render-setup case exercises the newly exposed zero tile-size
    // input; SetRenderSettings must normalize it before tile partitioning.
    settings.tileSize = 0;
    renderer->SetRenderSettings(settings);
}

bool
_FailedWithoutWork(
    ty::Renderer* renderer,
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
    ty::Renderer renderer;
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
    ty::Renderer renderer;
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
    ty::Renderer renderer;
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
    const GfRect2i dataWindow(GfVec2i(0), 1, 1);
    {
        _CountingRenderBuffer buffer(
            SdfPath("/unsupportedColor"), 1, 1, HdFormatInt32);
        ty::Renderer renderer;
        HdRenderThread renderThread;
        _Configure(
            &renderer,
            scene.scene,
            {_Binding(HdAovTokens->color, &buffer)},
            dataWindow);
        if (!_FailedWithoutWork(&renderer, &renderThread, buffer)) {
            return false;
        }
    }
    {
        _CountingRenderBuffer buffer(
            SdfPath("/unsupportedAmbientOcclusion"),
            1, 1, HdFormatFloat32Vec4);
        ty::Renderer renderer;
        HdRenderThread renderThread;
        _Configure(
            &renderer,
            scene.scene,
            {_Binding(ty::AovTokens->ambocc, &buffer)},
            dataWindow);
        if (!_FailedWithoutWork(&renderer, &renderThread, buffer)) {
            return false;
        }
    }
    return true;
}

bool
_TestZeroSizedBuffer()
{
    _Scene scene;
    _CountingRenderBuffer buffer(
        SdfPath("/zeroBuffer"), 0, 1, HdFormatFloat32Vec4);
    ty::Renderer renderer;
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
    ty::Renderer renderer;
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
    ty::Renderer renderer;
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
    ty::Renderer renderer;
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
    ty::Renderer renderer;
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
    ty::Renderer renderer;
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
    ty::Renderer renderer;
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
        ty::Renderer renderer;
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
        _CountingRenderBuffer ambientOcclusion(
            SdfPath("/dispatchAmbientOcclusion"),
            1, 1, HdFormatFloat32Vec3);
        ty::Renderer renderer;
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
                _Binding(TfToken("primvars:displayColor"), &primvar),
                _Binding(ty::AovTokens->ambocc, &ambientOcclusion)
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
            !TF_VERIFY(primvar.sampleWriteCount == 0) ||
            !TF_VERIFY(ambientOcclusion.sampleWriteCount == 1) ||
            !TF_VERIFY(
                ambientOcclusion.firstFloatWrite == GfVec4f(0.0f)) ||
            !TF_VERIFY(renderer.GetAmbientOcclusionRayCount() == 0)) {
            return false;
        }
    }

    // Color and adaptive heatmap remain independent when bound together.
    {
        _CountingRenderBuffer color(
            SdfPath("/dispatchColorWithHeatmap"),
            1, 1, HdFormatFloat32Vec4);
        _CountingRenderBuffer heatmap(
            SdfPath("/dispatchHeatmapWithColor"),
            1, 1, HdFormatFloat32Vec4);
        HdRenderPassAovBinding colorBinding =
            _Binding(HdAovTokens->color, &color);
        colorBinding.clearValue = VtValue(GfVec4f(0.25f));
        ty::Renderer renderer;
        HdRenderThread renderThread;
        _Configure(
            &renderer,
            scene.scene,
            {
                colorBinding,
                _Binding(ty::AovTokens->adaptiveHeatmap, &heatmap)
            },
            dataWindow);
        ty::RenderSettings settings;
        settings.samplesToConvergence = 4;
        settings.minSamplesBeforeAdaptive = 5;
        renderer.SetRenderSettings(settings);
        renderThread.StartRender();
        renderer.Render(&renderThread);
        if (!TF_VERIFY(
                color.floatWriteCount == 4,
                "floatWriteCount=%u", color.floatWriteCount) ||
            !TF_VERIFY(
                color.floatOutputWriteCount == 0,
                "floatOutputWriteCount=%u", color.floatOutputWriteCount) ||
            !TF_VERIFY(
                color.firstFloatWrite ==
                GfVec4f(0.25f, 0.25f, 0.25f, 1.0f)) ||
            !TF_VERIFY(
                heatmap.floatWriteCount == 4,
                "heatmap.floatWriteCount=%u", heatmap.floatWriteCount) ||
            !TF_VERIFY(heatmap.floatOutputWriteCount == 0)) {
            return false;
        }
    }

    // A constant radiance signal converges early without an enable flag.
    {
        _CountingRenderBuffer color(
            SdfPath("/dispatchAlwaysAdaptiveColor"),
            1, 1, HdFormatFloat32Vec4);
        HdRenderPassAovBinding colorBinding =
            _Binding(HdAovTokens->color, &color);
        colorBinding.clearValue = VtValue(GfVec4f(0.25f));
        ty::Renderer renderer;
        HdRenderThread renderThread;
        _Configure(
            &renderer,
            scene.scene,
            {colorBinding},
            dataWindow);
        ty::RenderSettings settings;
        settings.samplesToConvergence = 8;
        settings.minSamplesBeforeAdaptive = 2;
        renderer.SetRenderSettings(settings);
        renderThread.StartRender();
        renderer.Render(&renderThread);
        if (!TF_VERIFY(renderer.GetCompletedSamples() == 2) ||
            !TF_VERIFY(color.floatWriteCount == 2) ||
            !TF_VERIFY(color.floatOutputWriteCount == 0)) {
            return false;
        }
    }

    // Heatmap-only rendering evaluates actual radiance for convergence. A
    // high-frequency dome keeps this pixel above the zero-threshold floor, so
    // it reaches the configured sample limit instead of converging on black.
    {
        ty::LightData dome;
        dome.xformLightToWorld = GfMatrix4f(1.0f);
        dome.normalXformLightToWorld = GfMatrix3f(1.0f);
        dome.xformWorldToLight = GfMatrix4f(1.0f);
        dome.color = GfVec3f(1.0f);
        dome.lightVariant = ty::DomeLight();
        dome.texture.width = 16;
        dome.texture.height = 8;
        dome.texture.colorSpaceName = GfColorSpaceNames->LinearRec709;
        dome.texture.pixels.reserve(
            dome.texture.width * dome.texture.height);
        for (int y = 0; y < dome.texture.height; ++y) {
            for (int x = 0; x < dome.texture.width; ++x) {
                dome.texture.pixels.push_back(
                    ((x + y) % 2 == 0)
                        ? GfVec3f(0.0f)
                        : GfVec3f(4.0f));
            }
        }
        ty::BuildDomeLightSamplingDistribution(&dome.texture);

        _CountingRenderBuffer heatmap(
            SdfPath("/dispatchHeatmapOnly"),
            1, 1, HdFormatFloat32Vec4);
        ty::Renderer renderer;
        HdRenderThread renderThread;
        _Configure(
            &renderer,
            scene.scene,
            {_Binding(ty::AovTokens->adaptiveHeatmap, &heatmap)},
            dataWindow);
        GfFrustum frustum;
        frustum.SetPerspective(90.0, 1.0, 0.1, 100.0);
        renderer.SetCamera(
            frustum.ComputeViewMatrix(),
            frustum.ComputeProjectionMatrix());
        renderer.AddLight(SdfPath("/varianceDome"), &dome);
        ty::RenderSettings settings;
        settings.samplesToConvergence = 8;
        settings.randomNumberSeed = 1;
        settings.adaptiveThreshold = 0.0f;
        settings.minSamplesBeforeAdaptive = 4;
        renderer.SetRenderSettings(settings);
        renderThread.StartRender();
        renderer.Render(&renderThread);
        if (!TF_VERIFY(renderer.GetCompletedSamples() == 8) ||
            !TF_VERIFY(
                heatmap.floatWriteCount == 8,
                "floatWriteCount=%u", heatmap.floatWriteCount) ||
            !TF_VERIFY(
                heatmap.floatOutputWriteCount == 0,
                "floatOutputWriteCount=%u", heatmap.floatOutputWriteCount)) {
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
    _CountingRenderBuffer cameraDepth(
        SdfPath("/invalidContextCameraDepth"), 1, 1, HdFormatFloat32);
    _CountingRenderBuffer normal(
        SdfPath("/invalidContextNormal"), 1, 1, HdFormatFloat32Vec3);
    _CountingRenderBuffer primvar(
        SdfPath("/invalidContextPrimvar"), 1, 1, HdFormatFloat32Vec3);
    _CountingRenderBuffer ambientOcclusion(
        SdfPath("/invalidContextAmbientOcclusion"),
        1, 1, HdFormatFloat32Vec3);
    ty::Renderer renderer;
    HdRenderThread renderThread;
    _Configure(
        &renderer,
        scene.scene,
        {
            _Binding(HdAovTokens->primId, &primId),
            _Binding(HdAovTokens->cameraDepth, &cameraDepth),
            _Binding(HdAovTokens->normal, &normal),
            _Binding(TfToken("primvars:displayColor"), &primvar),
            _Binding(ty::AovTokens->ambocc, &ambientOcclusion)
        },
        GfRect2i(GfVec2i(0), 1, 1));
    renderThread.StartRender();
    renderer.Render(&renderThread);

    // Depth proves the top-level triangle was hit without needing its invalid
    // context. Context-dependent AOVs must then take their normal miss paths.
    return TF_VERIFY(cameraDepth.floatWriteCount == 1) &&
           TF_VERIFY(cameraDepth.firstFloatWrite[0] > 0.0f) &&
           TF_VERIFY(primId.intWriteCount == 1) &&
           TF_VERIFY(primId.firstIntWrite == -1) &&
           TF_VERIFY(normal.sampleWriteCount == 0) &&
           TF_VERIFY(primvar.sampleWriteCount == 0) &&
           TF_VERIFY(ambientOcclusion.sampleWriteCount == 1) &&
           TF_VERIFY(
               ambientOcclusion.firstFloatWrite == GfVec4f(0.0f)) &&
           TF_VERIFY(renderer.GetAmbientOcclusionRayCount() == 0);
}

bool
_TestFrameStatusTransitions()
{
    _Scene scene;
    _CountingRenderBuffer buffer(
        SdfPath("/frameStatus"), 1, 1, HdFormatFloat32Vec4);
    ty::Renderer renderer;
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
    ty::Renderer renderer;
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
    std::atomic<int> materialVersion{0};
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
        &materialVersion);
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

bool
_WaitForConvergence(HdEmbreeRenderPass* renderPass)
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
_TestEmptyRenderPassBindingsConverge(bool useFraming)
{
    _Scene scene;
    if (scene.scene == nullptr) {
        return false;
    }

    HdEmbreeRenderDelegate delegate;
    delegate.SetRenderSetting(
        HdEmbreeRenderSettingsTokens->convergedSamplesPerPixel,
        VtValue(1));
    std::unique_ptr<HdRenderIndex> renderIndex(
        HdRenderIndex::New(&delegate, HdDriverVector()));
    if (!renderIndex) {
        return false;
    }

    ty::Renderer renderer;
    renderer.SetScene(scene.scene);
    std::atomic<int> sceneVersion{0};
    std::atomic<int> materialVersion{0};
    HdRenderThread renderThread;
    renderThread.SetRenderCallback([&]() {
        renderer.Render(&renderThread);
    });
    renderThread.StartThread();

    HdEmbreeRenderPass renderPass(
        renderIndex.get(),
        HdRprimCollection(),
        &renderThread,
        &renderer,
        &sceneVersion,
        &materialVersion);
    if (renderPass.IsConverged()) {
        renderThread.StopThread();
        return false;
    }

    HdRenderPassStateSharedPtr renderPassState =
        delegate.CreateRenderPassState();
    if (useFraming) {
        renderPassState->SetFraming(
            CameraUtilFraming(GfRect2i(GfVec2i(0), 1, 1)));
    } else {
        renderPassState->SetViewport(
            GfVec4d(0.0, 0.0, 1.0, 1.0));
    }
    renderPassState->SetAovBindings(HdRenderPassAovBindingVector());
    renderPass.Execute(renderPassState, TfTokenVector());

    const bool converged = _WaitForConvergence(&renderPass);
    renderThread.StopThread();

    HdRenderPassAovBindingVector const& rendererBindings =
        renderer.GetAovBindings();
    if (!converged ||
        rendererBindings.size() != 2 ||
        rendererBindings[0].renderBuffer == nullptr ||
        rendererBindings[1].renderBuffer == nullptr) {
        return false;
    }

    if (useFraming) {
        // Camera framing deliberately leaves anonymous fallbacks unallocated,
        // so renderer setup must park the zero-sized buffers without a frame.
        return rendererBindings[0].renderBuffer->GetWidth() == 0 &&
            rendererBindings[0].renderBuffer->GetHeight() == 0 &&
            rendererBindings[1].renderBuffer->GetWidth() == 0 &&
            rendererBindings[1].renderBuffer->GetHeight() == 0 &&
            !renderer.DidLastFrameProduceValidPixels();
    }

    // Legacy viewport execution allocates both fallbacks and renders them.
    return rendererBindings[0].renderBuffer->GetWidth() == 1 &&
        rendererBindings[0].renderBuffer->GetHeight() == 1 &&
        rendererBindings[1].renderBuffer->GetWidth() == 1 &&
        rendererBindings[1].renderBuffer->GetHeight() == 1 &&
        renderer.DidLastFrameProduceValidPixels();
}

bool
_TestLiveRenderPassesOwnAnonymousBindings()
{
    _Scene scene;
    if (scene.scene == nullptr) {
        return false;
    }

    HdEmbreeRenderDelegate delegate;
    delegate.SetRenderSetting(
        HdEmbreeRenderSettingsTokens->convergedSamplesPerPixel,
        VtValue(1));
    std::unique_ptr<HdRenderIndex> renderIndex(
        HdRenderIndex::New(&delegate, HdDriverVector()));
    if (!renderIndex) {
        return false;
    }

    ty::Renderer renderer;
    renderer.SetScene(scene.scene);
    std::atomic<int> sceneVersion{0};
    std::atomic<int> materialVersion{0};
    std::atomic<bool> holdCallback{false};
    std::atomic<bool> callbackEntered{false};
    std::atomic<bool> releaseCallback{false};
    HdRenderThread renderThread;
    renderThread.SetRenderCallback([&]() {
        if (holdCallback.load(std::memory_order_acquire)) {
            callbackEntered.store(true, std::memory_order_release);
            while (!releaseCallback.load(std::memory_order_acquire)) {
                if (renderThread.IsStopRequested()) {
                    return;
                }
                std::this_thread::yield();
            }
        }
        renderer.Render(&renderThread);
    });
    renderThread.StartThread();

    HdRenderPassStateSharedPtr firstRenderPassState =
        delegate.CreateRenderPassState();
    firstRenderPassState->SetViewport(
        GfVec4d(0.0, 0.0, 1.0, 1.0));
    firstRenderPassState->SetAovBindings(
        HdRenderPassAovBindingVector());
    HdRenderPassStateSharedPtr secondRenderPassState =
        delegate.CreateRenderPassState();
    secondRenderPassState->SetViewport(
        GfVec4d(0.0, 0.0, 2.0, 2.0));
    secondRenderPassState->SetAovBindings(
        HdRenderPassAovBindingVector());

    std::unique_ptr<HdEmbreeRenderPass> firstRenderPass =
        std::make_unique<HdEmbreeRenderPass>(
            renderIndex.get(),
            HdRprimCollection(),
            &renderThread,
            &renderer,
            &sceneVersion,
            &materialVersion);
    std::unique_ptr<HdEmbreeRenderPass> secondRenderPass =
        std::make_unique<HdEmbreeRenderPass>(
            renderIndex.get(),
            HdRprimCollection(),
            &renderThread,
            &renderer,
            &sceneVersion,
            &materialVersion);

    firstRenderPass->Execute(firstRenderPassState, TfTokenVector());
    const bool firstConverged =
        _WaitForConvergence(firstRenderPass.get());
    if (!firstConverged || renderer.GetAovBindings().size() != 2) {
        renderThread.StopThread();
        return false;
    }
    HdRenderBuffer* const firstColor =
        renderer.GetAovBindings()[0].renderBuffer;
    HdRenderBuffer* const firstDepth =
        renderer.GetAovBindings()[1].renderBuffer;

    // A second live pass must replace the shared bindings and invalidate the
    // first pass's convergence without destroying either pass.
    if (secondRenderPass->IsConverged()) {
        renderThread.StopThread();
        return false;
    }
    secondRenderPass->Execute(secondRenderPassState, TfTokenVector());
    const bool secondConverged =
        _WaitForConvergence(secondRenderPass.get());
    HdRenderPassAovBindingVector const& secondBindings =
        renderer.GetAovBindings();
    if (!secondConverged ||
        firstRenderPass->IsConverged() ||
        secondBindings.size() != 2 ||
        secondBindings[0].renderBuffer == firstColor ||
        secondBindings[1].renderBuffer == firstDepth ||
        secondBindings[0].renderBuffer->GetWidth() != 2 ||
        secondBindings[0].renderBuffer->GetHeight() != 2 ||
        secondBindings[1].renderBuffer->GetWidth() != 2 ||
        secondBindings[1].renderBuffer->GetHeight() != 2) {
        renderThread.StopThread();
        return false;
    }

    // Returning to the first live pass must reclaim the renderer and make the
    // second pass non-current until it executes again.
    firstRenderPass->Execute(firstRenderPassState, TfTokenVector());
    const bool firstConvergedAgain =
        _WaitForConvergence(firstRenderPass.get());
    HdRenderPassAovBindingVector const& firstBindingsAgain =
        renderer.GetAovBindings();
    if (!firstConvergedAgain ||
        secondRenderPass->IsConverged() ||
        firstBindingsAgain.size() != 2 ||
        firstBindingsAgain[0].renderBuffer != firstColor ||
        firstBindingsAgain[1].renderBuffer != firstDepth ||
        firstBindingsAgain[0].renderBuffer->GetWidth() != 1 ||
        firstBindingsAgain[0].renderBuffer->GetHeight() != 1 ||
        firstBindingsAgain[1].renderBuffer->GetWidth() != 1 ||
        firstBindingsAgain[1].renderBuffer->GetHeight() != 1) {
        renderThread.StopThread();
        return false;
    }

    // Destroying the non-current pass must not stop the current owner's
    // in-flight render.
    holdCallback.store(true, std::memory_order_release);
    sceneVersion.fetch_add(1);
    firstRenderPass->Execute(firstRenderPassState, TfTokenVector());
    for (int i = 0;
         i != 500 &&
             !callbackEntered.load(std::memory_order_acquire);
         ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!callbackEntered.load(std::memory_order_acquire)) {
        releaseCallback.store(true, std::memory_order_release);
        renderThread.StopThread();
        return false;
    }

    secondRenderPass.reset();
    const bool ownerStillRendering = renderThread.IsRendering();
    releaseCallback.store(true, std::memory_order_release);
    const bool firstConvergedAfterDestruction =
        _WaitForConvergence(firstRenderPass.get());

    // Destroying the current owner must release its borrowed anonymous
    // buffers immediately rather than leaving dangling renderer pointers.
    firstRenderPass.reset();
    const bool ownerBindingsReleased =
        renderer.GetAovBindings().empty();
    renderThread.StopThread();
    return ownerStillRendering &&
        firstConvergedAfterDestruction &&
        ownerBindingsReleased &&
        renderer.DidLastFrameProduceValidPixels();
}

bool
_TestProcessGlobalDielectricSettingIsReapplied()
{
    ty::Renderer renderer;
    ty::RenderSettings settings;
    settings.dielectricLayerThroughputMode =
        ty::DielectricLayerThroughputMode::MaterialXGlsl;
    renderer.SetRenderSettings(settings);

    mxcpp::Bsdf::SetDielectricLayerThroughputMode(
        mxcpp::Bsdf::DielectricLayerThroughputMode::Bsdl);
    renderer.SetRenderSettings(settings);

    const bool reapplied =
        mxcpp::Bsdf::GetDielectricLayerThroughputMode() ==
            mxcpp::Bsdf::DielectricLayerThroughputMode::MaterialXGlsl;
    renderer.SetRenderSettings(ty::RenderSettings{});
    return reapplied;
}

struct _DisplayColorRenderCase
{
    GfVec3f displayColor = GfVec3f(0.25f);
    bool authorDisplayColor = true;
    bool bindMaterial = false;
    bool enableLighting = false;
    bool addDistantLight = false;
    bool bindColor = true;
    bool bindAmbientOcclusion = false;
    bool moveCameraToMissAfterFirstRender = false;
    int samplesToConvergence = 1;
    int minSamplesBeforeAdaptive = 64;
};

struct _SurfaceRenderResult
{
    GfVec4f color = GfVec4f(0.0f);
    GfVec3f ambientOcclusion = GfVec3f(0.0f);
    int completedSamples = 0;
    uint64_t ambientOcclusionRayCount = 0;
};

HdMaterialNetwork2
_MakeDisplayColorTestMaterial()
{
    HdMaterialNetwork2 network;
    const SdfPath surfacePath("/Material/Surface");
    HdMaterialNode2 surface;
    surface.nodeTypeId = TfToken("UsdPreviewSurface");
    surface.parameters[TfToken("diffuseColor")] =
        VtValue(GfVec3f(0.8f, 0.05f, 0.05f));
    network.nodes[surfacePath] = surface;
    network.terminals[TfToken("surface")] =
        HdMaterialConnection2{surfacePath, TfToken("out")};
    return network;
}

bool
_RenderSurfaceCase(
    _DisplayColorRenderCase const& renderCase,
    _SurfaceRenderResult* outResult)
{
    if (!outResult ||
        (!renderCase.bindColor && !renderCase.bindAmbientOcclusion)) {
        return false;
    }

    // Declare the borrowed light before the delegate so it outlives the
    // renderer registry when this function returns.
    ty::LightData distantLight;
    distantLight.xformLightToWorld = GfMatrix4f(1.0f);
    distantLight.normalXformLightToWorld = GfMatrix3f(1.0f);
    distantLight.xformWorldToLight = GfMatrix4f(1.0f);
    distantLight.color = GfVec3f(1.0f);
    distantLight.lightVariant = ty::DistantLight{0.0f};

    HdEmbreeRenderDelegate delegate;
    std::unique_ptr<HdRenderIndex> renderIndex(
        HdRenderIndex::New(&delegate, HdDriverVector()));
    if (!renderIndex) {
        return false;
    }

    HdUnitTestDelegate sceneDelegate(
        renderIndex.get(), SdfPath::AbsoluteRootPath());
    const SdfPath meshId("/displayColorQuad");
    const SdfPath materialId("/displayColorMaterial");
    if (renderCase.bindMaterial) {
        sceneDelegate.AddMaterialResource(
            materialId, VtValue(_MakeDisplayColorTestMaterial()));
    }
    sceneDelegate.AddMesh(
        meshId,
        GfMatrix4f(1.0f),
        VtVec3fArray{
            GfVec3f(-2.0f, -2.0f, -2.0f),
            GfVec3f(2.0f, -2.0f, -2.0f),
            GfVec3f(2.0f, 2.0f, -2.0f),
            GfVec3f(-2.0f, 2.0f, -2.0f)},
        VtIntArray{4},
        VtIntArray{0, 1, 2, 3},
        false,
        SdfPath(),
        PxOsdOpenSubdivTokens->none,
        HdTokens->rightHanded,
        true);
    if (renderCase.authorDisplayColor) {
        sceneDelegate.UpdatePrimvarValue(
            meshId, HdTokens->displayColor,
            VtValue(renderCase.displayColor));
    } else {
        sceneDelegate.RemovePrimvar(meshId, HdTokens->displayColor);
        sceneDelegate.RemovePrimvar(meshId, HdTokens->displayOpacity);
    }
    if (renderCase.bindMaterial) {
        sceneDelegate.BindMaterial(meshId, materialId);
        HdSprim* const material = renderIndex->GetSprim(
            HdPrimTypeTokens->material, materialId);
        if (!material) {
            return false;
        }
        HdDirtyBits materialBits = material->GetInitialDirtyBitsMask();
        material->Sync(
            &sceneDelegate, delegate.GetRenderParam(), &materialBits);
    }

    HdRprim* const mesh =
        const_cast<HdRprim*>(renderIndex->GetRprim(meshId));
    if (!mesh) {
        return false;
    }
    HdDirtyBits meshBits = mesh->GetInitialDirtyBitsMask();
    mesh->InitRepr(
        &sceneDelegate, HdReprTokens->smoothHull, &meshBits);
    mesh->Sync(
        &sceneDelegate,
        delegate.GetRenderParam(),
        &meshBits,
        HdReprTokens->smoothHull);

    HdEmbreeRenderParam* const renderParam =
        static_cast<HdEmbreeRenderParam*>(delegate.GetRenderParam());
    ty::Renderer* const renderer = renderParam->GetRenderer();
    renderer->SetCamera(GfMatrix4d(1.0), GfMatrix4d(1.0));
    renderer->SetDataWindow(GfRect2i(GfVec2i(0), 1, 1));
    if (renderCase.addDistantLight) {
        renderer->AddLight(SdfPath("/displayColorLight"), &distantLight);
    }

    HdEmbreeRenderBuffer color(SdfPath("/displayColorOutput"));
    HdEmbreeRenderBuffer ambientOcclusion(
        SdfPath("/ambientOcclusionOutput"));
    if ((renderCase.bindColor &&
         !color.Allocate(
             GfVec3i(1, 1, 1), HdFormatFloat32Vec4, true)) ||
        (renderCase.bindAmbientOcclusion &&
         !ambientOcclusion.Allocate(
             GfVec3i(1, 1, 1), HdFormatFloat32Vec3, true))) {
        return false;
    }
    HdRenderPassAovBindingVector bindings;
    if (renderCase.bindColor) {
        bindings.push_back(_Binding(HdAovTokens->color, &color));
    }
    if (renderCase.bindAmbientOcclusion) {
        bindings.push_back(
            _Binding(ty::AovTokens->ambocc, &ambientOcclusion));
    }
    renderer->SetAovBindings(bindings);

    ty::RenderSettings settings;
    settings.samplesToConvergence = renderCase.samplesToConvergence;
    settings.minSamplesBeforeAdaptive =
        renderCase.minSamplesBeforeAdaptive;
    settings.randomNumberSeed = 1;
    settings.enableLighting = renderCase.enableLighting;
    renderer->SetRenderSettings(settings);

    HdRenderThread renderThread;
    renderThread.StartRender();
    renderer->Render(&renderThread);
    renderThread.StopRender();
    if (renderCase.moveCameraToMissAfterFirstRender) {
        GfMatrix4d movedView(1.0);
        movedView.SetTranslate(GfVec3d(-100.0, 0.0, 0.0));
        renderer->SetCamera(movedView, GfMatrix4d(1.0));
        renderer->ResetAccumulation();
        renderThread.StartRender();
        renderer->Render(&renderThread);
        renderThread.StopRender();
    }
    if (!renderer->DidLastFrameProduceValidPixels() ||
        renderer->GetCompletedSamples() < 1) {
        return false;
    }
    outResult->completedSamples = renderer->GetCompletedSamples();
    outResult->ambientOcclusionRayCount =
        renderer->GetAmbientOcclusionRayCount();

    if (renderCase.bindColor) {
        const float* const colorData =
            static_cast<float const*>(color.Map());
        if (!colorData) {
            return false;
        }
        outResult->color = GfVec4f(
            colorData[0], colorData[1], colorData[2], colorData[3]);
        color.Unmap();
    }
    if (renderCase.bindAmbientOcclusion) {
        const float* const ambientOcclusionData =
            static_cast<float const*>(ambientOcclusion.Map());
        if (!ambientOcclusionData) {
            return false;
        }
        outResult->ambientOcclusion = GfVec3f(
            ambientOcclusionData[0],
            ambientOcclusionData[1],
            ambientOcclusionData[2]);
        ambientOcclusion.Unmap();
    }
    return true;
}

bool
_RenderDisplayColorCase(
    _DisplayColorRenderCase const& renderCase,
    GfVec4f* outColor)
{
    if (!outColor) {
        return false;
    }
    _SurfaceRenderResult result;
    if (!_RenderSurfaceCase(renderCase, &result)) {
        return false;
    }
    *outColor = result.color;
    return true;
}

bool
_TestDisplayColorFallbacks()
{
    const GfVec3f authoredColor(0.1f, 0.2f, 0.3f);
    GfVec4f unmaterialized;
    GfVec4f materialized;
    if (!_RenderDisplayColorCase(
            _DisplayColorRenderCase{authoredColor, true, false, false, false},
            &unmaterialized) ||
        !_RenderDisplayColorCase(
            _DisplayColorRenderCase{authoredColor, true, true, false, false},
            &materialized) ||
        !GfIsClose(unmaterialized, materialized, 1.0e-5f)) {
        std::printf("unlit material changed authored displayColor\n");
        return false;
    }

    GfVec4f authoredGray;
    GfVec4f fallbackGray;
    if (!_RenderDisplayColorCase(
            _DisplayColorRenderCase{
                GfVec3f(0.25f), true, false, false, false},
            &authoredGray) ||
        !_RenderDisplayColorCase(
            _DisplayColorRenderCase{
                GfVec3f(0.0f), false, false, false, false},
            &fallbackGray)) {
        return false;
    }
    const GfVec4f expectedFallback(
        2.0f * authoredGray[0],
        2.0f * authoredGray[1],
        2.0f * authoredGray[2],
        1.0f);
    if (!GfIsClose(fallbackGray, expectedFallback, 1.0e-5f)) {
        std::printf("missing displayColor did not resolve to neutral gray\n");
        return false;
    }

    GfVec4f litColor;
    GfVec4f litDoubleColor;
    if (!_RenderDisplayColorCase(
            _DisplayColorRenderCase{
                authoredColor, true, false, true, true},
            &litColor) ||
        !_RenderDisplayColorCase(
            _DisplayColorRenderCase{
                2.0f * authoredColor, true, false, true, true},
            &litDoubleColor)) {
        return false;
    }
    const GfVec4f expectedLitDouble(
        2.0f * litColor[0],
        2.0f * litColor[1],
        2.0f * litColor[2],
        1.0f);
    if (litColor[0] <= 0.0f ||
        !GfIsClose(litDoubleColor, expectedLitDouble, 1.0e-5f)) {
        std::printf("lit fallback did not use authored displayColor\n");
        return false;
    }

    GfVec4f unlitByMissingLight;
    if (!_RenderDisplayColorCase(
            _DisplayColorRenderCase{
                authoredColor, true, false, true, false},
            &unlitByMissingLight) ||
        !GfIsClose(
            unlitByMissingLight,
            GfVec4f(0.0f, 0.0f, 0.0f, 1.0f),
            1.0e-6f)) {
        std::printf("lit fallback generated radiance without a light\n");
        return false;
    }

    return true;
}

bool
_TestAmbientOcclusionAov()
{
    _DisplayColorRenderCase baselineCase;
    baselineCase.displayColor = GfVec3f(0.1f, 0.2f, 0.3f);
    baselineCase.samplesToConvergence = 8;
    baselineCase.minSamplesBeforeAdaptive = 9;

    _SurfaceRenderResult baseline;
    if (!_RenderSurfaceCase(baselineCase, &baseline) ||
        baseline.completedSamples != 8 ||
        baseline.ambientOcclusionRayCount != 0) {
        return false;
    }

    _DisplayColorRenderCase combinedCase = baselineCase;
    combinedCase.bindAmbientOcclusion = true;
    _SurfaceRenderResult combined;
    if (!_RenderSurfaceCase(combinedCase, &combined) ||
        combined.completedSamples != 8 ||
        combined.ambientOcclusionRayCount != 8 ||
        !GfIsClose(combined.color, baseline.color, 1.0e-6f) ||
        !GfIsClose(
            combined.ambientOcclusion, GfVec3f(1.0f), 1.0e-6f)) {
        return false;
    }

    // With only ambocc bound, AO itself drives convergence and the renderer
    // still emits at most one visibility ray per progressive pixel sample.
    _DisplayColorRenderCase ambientOnlyCase = baselineCase;
    ambientOnlyCase.bindColor = false;
    ambientOnlyCase.bindAmbientOcclusion = true;
    _SurfaceRenderResult ambientOnly;
    if (!_RenderSurfaceCase(ambientOnlyCase, &ambientOnly) ||
        ambientOnly.completedSamples != 8 ||
        ambientOnly.ambientOcclusionRayCount != 8 ||
        !GfIsClose(
            ambientOnly.ambientOcclusion, GfVec3f(1.0f), 1.0e-6f)) {
        return false;
    }

    ambientOnlyCase.minSamplesBeforeAdaptive = 2;
    _SurfaceRenderResult adaptiveExit;
    if (!_RenderSurfaceCase(ambientOnlyCase, &adaptiveExit) ||
        adaptiveExit.completedSamples != 2 ||
        adaptiveExit.ambientOcclusionRayCount != 2 ||
        !GfIsClose(
            adaptiveExit.ambientOcclusion, GfVec3f(1.0f), 1.0e-6f)) {
        return false;
    }

    // Camera invalidation retains the resolved preview while resetting sample
    // accumulation. A new miss must replace the previous hit rather than
    // leaving stale AO in the resolved buffer.
    ambientOnlyCase.samplesToConvergence = 1;
    ambientOnlyCase.minSamplesBeforeAdaptive = 64;
    ambientOnlyCase.moveCameraToMissAfterFirstRender = true;
    _SurfaceRenderResult cameraMoved;
    return _RenderSurfaceCase(ambientOnlyCase, &cameraMoved) &&
        cameraMoved.completedSamples == 1 &&
        cameraMoved.ambientOcclusionRayCount == 0 &&
        GfIsClose(
            cameraMoved.ambientOcclusion, GfVec3f(0.0f), 1.0e-6f);
}

bool
_TestCameraJitterTileDeterminism()
{
    constexpr unsigned int width = 40;
    constexpr unsigned int height = 8;

    HdEmbreeRenderDelegate delegate;
    std::unique_ptr<HdRenderIndex> renderIndex(
        HdRenderIndex::New(&delegate, HdDriverVector()));
    if (!renderIndex) {
        return false;
    }

    HdUnitTestDelegate sceneDelegate(
        renderIndex.get(), SdfPath::AbsoluteRootPath());
    const SdfPath meshId("/leftHalf");
    // The right edge crosses pixel 20 halfway through its interior in NDC.
    // Non-jittered rays stay left of it while jittered samples straddle it.
    sceneDelegate.AddMesh(
        meshId,
        GfMatrix4f(1.0f),
        VtVec3fArray{
            GfVec3f(-2.0f, -2.0f, -2.0f),
            GfVec3f(0.025f, -2.0f, -2.0f),
            GfVec3f(0.025f, 2.0f, -2.0f),
            GfVec3f(-2.0f, 2.0f, -2.0f)},
        VtIntArray{4},
        VtIntArray{0, 1, 2, 3},
        false,
        SdfPath(),
        PxOsdOpenSubdivTokens->none,
        HdTokens->rightHanded,
        true);

    HdRprim* const mesh =
        const_cast<HdRprim*>(renderIndex->GetRprim(meshId));
    if (!mesh) {
        return false;
    }
    HdDirtyBits meshBits = mesh->GetInitialDirtyBitsMask();
    mesh->InitRepr(
        &sceneDelegate, HdReprTokens->smoothHull, &meshBits);
    mesh->Sync(
        &sceneDelegate,
        delegate.GetRenderParam(),
        &meshBits,
        HdReprTokens->smoothHull);

    HdEmbreeRenderParam* const renderParam =
        static_cast<HdEmbreeRenderParam*>(delegate.GetRenderParam());
    ty::Renderer* const renderer = renderParam->GetRenderer();
    renderer->SetCamera(GfMatrix4d(1.0), GfMatrix4d(1.0));
    renderer->SetDataWindow(
        GfRect2i(GfVec2i(0), width, height));

    const auto renderImage =
        [&](int tileSize,
            std::vector<uint8_t>* colorBytes,
            std::vector<uint8_t>* primIdBytes) {
            HdEmbreeRenderBuffer color(SdfPath("/imageColor"));
            HdEmbreeRenderBuffer primId(SdfPath("/imagePrimId"));
            if (!color.Allocate(
                    GfVec3i(width, height, 1),
                    HdFormatFloat32Vec4,
                    true) ||
                !primId.Allocate(
                    GfVec3i(width, height, 1),
                    HdFormatInt32,
                    true)) {
                return false;
            }
            renderer->SetAovBindings({
                _Binding(HdAovTokens->color, &color),
                _Binding(HdAovTokens->primId, &primId)});

            ty::RenderSettings settings;
            settings.samplesToConvergence = 16;
            settings.randomNumberSeed = 1;
            settings.tileSize = tileSize;
            settings.enableLighting = false;
            renderer->SetRenderSettings(settings);

            HdRenderThread renderThread;
            renderThread.StartRender();
            renderer->Render(&renderThread);
            renderThread.StopRender();

            const size_t colorByteCount =
                width * height * HdDataSizeOfFormat(color.GetFormat());
            const uint8_t* const colorData =
                static_cast<uint8_t const*>(color.Map());
            colorBytes->assign(
                colorData, colorData + colorByteCount);
            color.Unmap();

            const size_t primIdByteCount =
                width * height * HdDataSizeOfFormat(primId.GetFormat());
            const uint8_t* const primIdData =
                static_cast<uint8_t const*>(primId.Map());
            primIdBytes->assign(
                primIdData, primIdData + primIdByteCount);
            primId.Unmap();
            return renderer->DidLastFrameProduceValidPixels() &&
                renderer->GetCompletedSamples() == 16;
        };

    std::vector<uint8_t> tile8Color;
    std::vector<uint8_t> tile32Color;
    std::vector<uint8_t> tile8PrimId;
    std::vector<uint8_t> tile32PrimId;
    const bool rendered =
        renderImage(
            8, &tile8Color, &tile8PrimId) &&
        renderImage(
            32, &tile32Color, &tile32PrimId);

    renderer->SetRenderSettings(ty::RenderSettings{});
    return rendered &&
        tile8Color == tile32Color &&
        tile8PrimId == tile32PrimId;
}

bool
_TestRenderPassSettingsApplication()
{
    HdEmbreeRenderDelegate delegate;
    std::unique_ptr<HdRenderIndex> renderIndex(
        HdRenderIndex::New(&delegate, HdDriverVector()));
    if (!renderIndex) {
        return false;
    }

    ty::Renderer renderer;
    std::atomic<int> sceneVersion{0};
    std::atomic<int> materialVersion{0};
    HdRenderThread renderThread;
    renderThread.SetRenderCallback([]() {});
    renderThread.StartThread();
    const auto finish = [&]() {
        renderThread.StopThread();
        renderer.SetRenderSettings(ty::RenderSettings{});
    };

    HdEmbreeRenderPass renderPass(
        renderIndex.get(),
        HdRprimCollection(),
        &renderThread,
        &renderer,
        &sceneVersion,
        &materialVersion);
    HdRenderPassStateSharedPtr renderPassState =
        delegate.CreateRenderPassState();
    renderPassState->SetViewport(GfVec4d(0.0, 0.0, 1.0, 1.0));

    // The first Execute must restore the process-wide default even when the
    // settings version has not changed and state was changed externally.
    mxcpp::Bsdf::SetDielectricLayerThroughputMode(
        mxcpp::Bsdf::DielectricLayerThroughputMode::MaterialXGlsl);
    renderPass.Execute(renderPassState, TfTokenVector());
    if (mxcpp::Bsdf::GetDielectricLayerThroughputMode() !=
            mxcpp::Bsdf::DielectricLayerThroughputMode::Bsdl) {
        finish();
        return false;
    }

    // A direct delegate update must propagate on the next Execute, normalize
    // renderer invariants, and canonicalize the unknown throughput token.
    delegate.SetRenderSetting(
        HdEmbreeRenderSettingsTokens->tileSize, VtValue(0));
    delegate.SetRenderSetting(
        HdEmbreeRenderSettingsTokens->maxBounces, VtValue(-2));
    delegate.SetRenderSetting(
        HdEmbreeRenderSettingsTokens->lightSamplesPerHit, VtValue(0));
    delegate.SetRenderSetting(
        HdEmbreeRenderSettingsTokens->dielectricLayerThroughputMode,
        VtValue(std::string("invalid")));
    unsigned int dielectricWarnings = 0;
    unsigned int unexpectedDiagnostics = 0;
    {
        _ScopedTokenWarningDelegate warnings;
        renderPass.Execute(renderPassState, TfTokenVector());
        dielectricWarnings = warnings.dielectricWarnings;
        unexpectedDiagnostics = warnings.unexpectedDiagnostics;
    }
    const ty::RenderSettings normalized = renderer.GetRenderSettings();
    if (normalized.tileSize != 1 ||
        normalized.maxBounces != 0 ||
        normalized.lightSamplesPerHit != 1 ||
        normalized.dielectricLayerThroughputMode !=
            ty::DielectricLayerThroughputMode::Bsdl ||
        dielectricWarnings != 1 ||
        unexpectedDiagnostics != 0) {
        finish();
        return false;
    }

    finish();
    return true;
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
    TF_AXIOM(_TestEmptyRenderPassBindingsConverge(false));
    TF_AXIOM(_TestEmptyRenderPassBindingsConverge(true));
    TF_AXIOM(_TestLiveRenderPassesOwnAnonymousBindings());
    TF_AXIOM(_TestProcessGlobalDielectricSettingIsReapplied());
    TF_AXIOM(_TestDisplayColorFallbacks());
    TF_AXIOM(_TestAmbientOcclusionAov());
    TF_AXIOM(_TestCameraJitterTileDeterminism());
    TF_AXIOM(_TestRenderPassSettingsApplication());
    return 0;
}
