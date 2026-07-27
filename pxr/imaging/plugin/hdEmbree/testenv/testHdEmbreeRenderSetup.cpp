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

    void Write(GfVec3i const&, size_t, float const*) override
    {
        ++sampleWriteCount;
    }

    void Write(GfVec3i const&, size_t, int const*) override
    {
        ++sampleWriteCount;
    }

    void WriteOutput(GfVec3i const&, size_t, float const*) override
    {
        ++sampleWriteCount;
    }

    void Clear(size_t, float const*) override {}
    void Clear(size_t, int const*) override {}

    unsigned int mapCount = 0;
    unsigned int unmapCount = 0;
    unsigned int resolveCount = 0;
    unsigned int sampleWriteCount = 0;
    bool converged = false;

protected:
    void _Deallocate() override {}

private:
    unsigned int _width;
    unsigned int _height;
    HdFormat _format;
    uint8_t _storage = 0;
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
    TF_AXIOM(_TestFrameStatusTransitions());
    TF_AXIOM(_TestRenderPassMarksRestartPending());
    return 0;
}
