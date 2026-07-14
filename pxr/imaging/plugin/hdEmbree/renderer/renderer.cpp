//
// Copyright 2018 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
// Frame orchestration and renderer configuration.

#include "pxr/imaging/plugin/hdEmbree/renderer/renderer.h"
#include "rendererImpl.h"
#include "pxr/imaging/plugin/hdEmbree/renderer/renderBuffer.h"

#include "pxr/imaging/hd/perfLog.h"
#include "pxr/imaging/hd/renderBuffer.h"
#include "pxr/base/work/loops.h"
#include "pxr/base/work/threadLimits.h"

#include "pxr/base/work/workTBB/tbb_version.h"

#include <chrono>
#include <cstdio>
#include <thread>

// -------------------------------------------------------------------------
// Old TBB workaround - we plan to remove this once OpenUSD adopts
// oneTBB as a min spec. This applies the "Work" thread limit to the
// render thread if "Work" is using old TBB, but won't affect other "Work"
// implementations.  Note that it may affect Embree TBB usage as well.
//
// Note: The TBB version macro is located in different headers in legacy TBB.
// -------------------------------------------------------------------------
#if TBB_INTERFACE_VERSION_MAJOR < 12

#include <optional>
#include <tbb/task_scheduler_init.h>

namespace {

PXR_NAMESPACE_USING_DIRECTIVE

// Make the calling context respect PXR_WORK_THREAD_LIMIT, if run from a thread
// other than the main thread (ie, the renderThread)
class _ScopedThreadScheduler {
public:
    _ScopedThreadScheduler() {
        auto limit = WorkGetConcurrencyLimitSetting();
        if (limit != 0) {
            _tbbTaskSchedInit.emplace(limit);
        }
    }

    std::optional<tbb::task_scheduler_init> _tbbTaskSchedInit;
};

}  // anonymous namespace

#endif  // TBB_INTERFACE_VERSION_MAJOR < 12


PXR_NAMESPACE_OPEN_SCOPE

TF_DEFINE_PUBLIC_TOKENS(HdEmbreeAovTokens, HDEMBREE_AOV_TOKENS);

static
bool
_IsContained(const GfRect2i& rect, int width, int height)
{
    return
        rect.GetMinX() >= 0 && rect.GetMaxX() < width &&
        rect.GetMinY() >= 0 && rect.GetMaxY() < height;
}

HdEmbreeRenderer::HdEmbreeRenderer()
    : _aovBindings()
    , _aovNames()
    , _aovBindingsNeedValidation(false)
    , _aovBindingsValid(false)
    , _width(0)
    , _height(0)
    , _viewMatrix(1.0f) // == identity
    , _projMatrix(1.0f) // == identity
    , _inverseViewMatrix(1.0f) // == identity
    , _inverseProjMatrix(1.0f) // == identity
    , _cameraExposureScale(1.0f)
    , _cameraDepthOfField()
    , _scene(nullptr)
    , _samplesToConvergence(
        HdEmbreeConfig::GetInstance().samplesToConvergence)
    , _ambientOcclusionSamples(
        HdEmbreeConfig::GetInstance().enableAmbientOcclusion
            ? HdEmbreeConfig::GetInstance().ambientOcclusionSamples
            : 0)
    , _enableSceneColors(HdEmbreeConfig::GetInstance().enableSceneColors)
    , _domeLightCameraVisibility(
        HdEmbreeConfig::GetInstance().domeLightCameraVisibility)
    , _enableLighting(HdEmbreeConfig::GetInstance().enableLighting)
    , _maxBounces(HdEmbreeConfig::GetInstance().maxBounces)
    , _minBouncesBeforeRR(HdEmbreeConfig::GetInstance().minBouncesBeforeRR)
    , _samplerSequence(HdEmbreeGetSamplerSequenceFromToken(
        TfToken(HdEmbreeConfig::GetInstance().samplerSequence)))
    , _enableAdaptiveSampling(
        HdEmbreeConfig::GetInstance().enableAdaptiveSampling)
    , _adaptiveThreshold(HdEmbreeConfig::GetInstance().adaptiveThreshold)
    , _minSamplesBeforeAdaptive(
        HdEmbreeConfig::GetInstance().minSamplesBeforeAdaptive)
    , _lightSamplesPerHit(HdEmbreeConfig::GetInstance().lightSamplesPerHit)
    , _stratifyLightSamples(
        HdEmbreeConfig::GetInstance().stratifyLightSamples)
    , _showAdaptiveHeatmap(HdEmbreeConfig::GetInstance().showAdaptiveHeatmap)
    , _fireflyClampThreshold(
        HdEmbreeConfig::GetInstance().fireflyClampThreshold)
    , _enableCaustics(HdEmbreeConfig::GetInstance().enableCaustics)
    , _causticsClampThreshold(
        HdEmbreeConfig::GetInstance().causticsClampThreshold)
    , _approxTransparentShadows(
        HdEmbreeConfig::GetInstance().approxTransparentShadows)
    , _disableShadows(HdEmbreeConfig::GetInstance().disableShadows)
    , _enableGgxMicrofacetMultipleScattering(
        HdEmbreeConfig::GetInstance().enableGgxMicrofacetMultipleScattering)
    , _dielectricLayerThroughputMode(
        TfToken(HdEmbreeConfig::GetInstance().dielectricLayerThroughputMode))
    , _useAdobeOpenPBR(HdEmbreeConfig::GetInstance().useAdobeOpenPBR)
    , _textureSystem(std::make_unique<HdEmbreeOiioTextureSystem>())
    , _sceneFrame(0.0f)
    , _sceneTime(0.0f)
    , _completedSamples(0)
    , _sssCallCount(0)
    , _sssSuccessCount(0)
    , _sssWalkStepCount(0)
    , _sssIntersectionCount(0)
{
}

HdEmbreeRenderer::~HdEmbreeRenderer() = default;

void
HdEmbreeRenderer::SetScene(RTCScene scene)
{
    _scene = scene;
}

void
HdEmbreeRenderer::SetSamplesToConvergence(int samplesToConvergence)
{
    _samplesToConvergence = samplesToConvergence;
}

void
HdEmbreeRenderer::SetAmbientOcclusionSamples(int ambientOcclusionSamples)
{
    _ambientOcclusionSamples = ambientOcclusionSamples;
}

void
HdEmbreeRenderer::SetEnableSceneColors(bool enableSceneColors)
{
    _enableSceneColors = enableSceneColors;
}

void
HdEmbreeRenderer::SetDomeLightCameraVisibility(bool domeLightCameraVisibility)
{
    _domeLightCameraVisibility = domeLightCameraVisibility;
}

void
HdEmbreeRenderer::SetEnableLighting(bool enableLighting)
{
    _enableLighting = enableLighting;
}

void
HdEmbreeRenderer::SetMaxBounces(int maxBounces)
{
    _maxBounces = std::max(0, maxBounces);
}

void
HdEmbreeRenderer::SetMinBouncesBeforeRR(int minBounces)
{
    _minBouncesBeforeRR = minBounces;
}

void
HdEmbreeRenderer::SetSamplerSequence(HdEmbreeSamplerSequence sequence)
{
    _samplerSequence = sequence;
}

void
HdEmbreeRenderer::SetEnableAdaptiveSampling(bool enable)
{
    _enableAdaptiveSampling = enable;
}

void
HdEmbreeRenderer::SetAdaptiveThreshold(float threshold)
{
    _adaptiveThreshold = threshold;
}

void
HdEmbreeRenderer::SetMinSamplesBeforeAdaptive(int minSamples)
{
    _minSamplesBeforeAdaptive = minSamples;
}

void
HdEmbreeRenderer::SetLightSamplesPerHit(int samples)
{
    _lightSamplesPerHit = std::max(1, samples);
}

void
HdEmbreeRenderer::SetStratifyLightSamples(bool stratify)
{
    _stratifyLightSamples = stratify;
}

void
HdEmbreeRenderer::SetShowAdaptiveHeatmap(bool show)
{
    _showAdaptiveHeatmap = show;
}

void
HdEmbreeRenderer::SetFireflyClampThreshold(float threshold)
{
    _fireflyClampThreshold = threshold;
}

void
HdEmbreeRenderer::SetEnableCaustics(bool enable)
{
    _enableCaustics = enable;
}

void
HdEmbreeRenderer::SetCausticsClampThreshold(float threshold)
{
    _causticsClampThreshold = threshold;
}

void
HdEmbreeRenderer::SetApproxTransparentShadows(bool enable)
{
    _approxTransparentShadows = enable;
}

void
HdEmbreeRenderer::SetDisableShadows(bool disable)
{
    _disableShadows = disable;
}

void
HdEmbreeRenderer::SetEnableGgxMicrofacetMultipleScattering(bool enable)
{
    _enableGgxMicrofacetMultipleScattering = enable;
    mxcpp::Bsdf::SetGgxMicrofacetMultipleScatteringEnabled(enable);
}

void
HdEmbreeRenderer::SetDielectricLayerThroughputMode(TfToken const& mode)
{
    if (mode == _tokensDielectricLayerThroughputModeMaterialXGlsl) {
        _dielectricLayerThroughputMode = mode;
        mxcpp::Bsdf::SetDielectricLayerThroughputMode(
            mxcpp::Bsdf::DielectricLayerThroughputMode::MaterialXGlsl);
        return;
    }

    if (mode != _tokensDielectricLayerThroughputModeBsdl) {
        TF_WARN("hdEmbree dielectric layer throughput mode '%s' is unknown; "
                "falling back to 'bsdl'.",
                mode.GetText());
    }
    _dielectricLayerThroughputMode = _tokensDielectricLayerThroughputModeBsdl;
    mxcpp::Bsdf::SetDielectricLayerThroughputMode(
        mxcpp::Bsdf::DielectricLayerThroughputMode::Bsdl);
}

void
HdEmbreeRenderer::SetUseAdobeOpenPBR(bool enable)
{
    _useAdobeOpenPBR = enable;
}

void
HdEmbreeRenderer::SetTextureCacheSize(int sizeMB)
{
    // The texture system is always an HdEmbreeOiioTextureSystem (constructed
    // above); route the cache size to it. Guarded so a future alternate
    // texture backend is simply left untouched.
    if (auto* oiio =
            dynamic_cast<HdEmbreeOiioTextureSystem*>(_textureSystem.get())) {
        oiio->SetCacheSizeMB(sizeMB);
    }
}

void
HdEmbreeRenderer::SetRandomNumberSeed(int randomNumberSeed)
{
    _randomNumberSeed = randomNumberSeed;
}

void
HdEmbreeRenderer::SetDataWindow(const GfRect2i& dataWindow)
{
    _dataWindow = dataWindow;

    // Here for clients that do not use camera framing but the
    // viewport.
    //
    // Re-validate the attachments, since attachment viewport and
    // render viewport need to match.
    _aovBindingsNeedValidation = true;
}

void
HdEmbreeRenderer::SetCamera(const GfMatrix4d& viewMatrix,
                            const GfMatrix4d& projMatrix)
{
    _viewMatrix = viewMatrix;
    _projMatrix = projMatrix;
    _inverseViewMatrix = viewMatrix.GetInverse();
    _inverseProjMatrix = projMatrix.GetInverse();
}

void
HdEmbreeRenderer::SetCameraExposureScale(float cameraExposureScale)
{
    _cameraExposureScale = cameraExposureScale;
}

void
HdEmbreeRenderer::SetCameraDepthOfField(
    HdEmbreeCameraDepthOfField const& cameraDepthOfField)
{
    _cameraDepthOfField = cameraDepthOfField;
}

void
HdEmbreeRenderer::SetSceneFrameAndTime(float frame, float time)
{
    _sceneFrame = frame;
    _sceneTime = time;
}

int
HdEmbreeRenderer::GetCompletedSamples() const
{
    return _completedSamples.load();
}

float
HdEmbreeRenderer::GetRenderElapsedSeconds() const
{
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<float>(now - _renderStartTime).count();
}

uint64_t
HdEmbreeRenderer::GetSssCallCount() const
{
    return _sssCallCount.load();
}

uint64_t
HdEmbreeRenderer::GetSssSuccessCount() const
{
    return _sssSuccessCount.load();
}

uint64_t
HdEmbreeRenderer::GetSssWalkStepCount() const
{
    return _sssWalkStepCount.load();
}

uint64_t
HdEmbreeRenderer::GetSssIntersectionCount() const
{
    return _sssIntersectionCount.load();
}

void
HdEmbreeRenderer::_PreRenderSetup()
{
    HD_TRACE_FUNCTION();

    _completedSamples.store(0);
    _sssCallCount.store(0);
    _sssSuccessCount.store(0);
    _sssWalkStepCount.store(0);
    _sssIntersectionCount.store(0);

    {
        HD_TRACE_SCOPE("HdEmbreeRenderer::CommitScene");
        // Commit any pending changes to the scene.
        rtcCommitScene(_scene);
    }

    if (!_ValidateAovBindings()) {
        // We aren't going to render anything. Just mark all AOVs as converged
        // so that we will stop rendering.
        for (size_t i = 0; i < _aovBindings.size(); ++i) {
            HdEmbreeRenderBufferInterface *rb = dynamic_cast<HdEmbreeRenderBufferInterface*>(
                _aovBindings[i].renderBuffer);
            rb->SetConverged(true);
        }
        // XXX:validation
        TF_WARN("Could not validate Aovs. Render will not complete");
        return;
    }

    _width  = 0;
    _height = 0;

    // Map all of the attachments.
    for (size_t i = 0; i < _aovBindings.size(); ++i) {
        //
        // XXX
        //
        // A scene delegate might specify the path to a
        // render buffer instead of a pointer to the
        // render buffer.
        //
        dynamic_cast<HdEmbreeRenderBufferInterface*>(
            _aovBindings[i].renderBuffer)->Map();

        if (i == 0) {
            _width  = _aovBindings[i].renderBuffer->GetWidth();
            _height = _aovBindings[i].renderBuffer->GetHeight();
        } else {
            if (_width  != _aovBindings[i].renderBuffer->GetWidth() ||
                 _height != _aovBindings[i].renderBuffer->GetHeight()) {
                TF_CODING_ERROR(
                    "Embree render buffers have inconsistent sizes");
            }
        }
    }

    if (_width > 0 || _height > 0) {
        if (!_IsContained(_dataWindow, _width, _height)) {
            TF_CODING_ERROR(
                "dataWindow is larger than render buffer");
        }
    }

    // Allocate adaptive sampling arrays if enabled.
    if (_enableAdaptiveSampling && _width > 0 && _height > 0) {
        const size_t numPixels = _width * _height;
        _pixelMean.resize(numPixels, GfVec3f(0.0f));
        _pixelM2.resize(numPixels, GfVec3f(0.0f));
        _pixelSampleCount.resize(numPixels, 0);
        _pixelConverged.resize(numPixels, false);
    }

    _BuildAovDispatchTable();
}

void
HdEmbreeRenderer::Render(HdRenderThread *renderThread)
{
    HD_TRACE_FUNCTION();

#if TBB_INTERFACE_VERSION_MAJOR < 12
    _ScopedThreadScheduler scheduler;
#endif

    _PreRenderSetup();

    _renderStartTime = std::chrono::steady_clock::now();

    // Compute the OpenQMC frame seed once per Render() call. An explicit
    // render setting or environment seed overrides the scene frame.
    const uint32_t baseSeed =
        HdEmbreeResolveFrameSeed(_randomNumberSeed, _sceneFrame);

    const unsigned int tileSize = HdEmbreeConfig::GetInstance().tileSize;
    const unsigned int numTilesX =
        (_dataWindow.GetWidth() + tileSize - 1) / tileSize;
    const unsigned int numTilesY =
        (_dataWindow.GetHeight() + tileSize - 1) / tileSize;

    // ---- Coarse preview passes ----
    // Render a sparse subset of pixels and block-fill the display buffer
    // so the user sees a mosaic preview almost immediately, then refine.
    {
        static const unsigned int kPreviewStrides[] = {8, 4, 2};
        for (unsigned int stride : kPreviewStrides) {
            if (renderThread->IsStopRequested()) {
                break;
            }

            // Only run coarse passes that are coarser than a single pixel.
            if (stride >= static_cast<unsigned int>(_dataWindow.GetWidth()) &&
                stride >= static_cast<unsigned int>(_dataWindow.GetHeight())) {
                continue;
            }

            {
                HD_TRACE_SCOPE("HdEmbreeRenderer::TracePreviewPass");
                WorkParallelForN(numTilesX * numTilesY,
                    std::bind(&HdEmbreeRenderer::_RenderTiles, this,
                        renderThread, /*sampleNum=*/0, baseSeed, stride,
                        std::placeholders::_1, std::placeholders::_2));
            }

            if (renderThread->IsStopRequested()) {
                break;
            }

            // Resolve sparse samples into the display buffer and
            // replicate each sampled pixel across its block.
            {
                HD_TRACE_SCOPE("HdEmbreeRenderer::ResolvePreviewPass");
                auto lock = renderThread->LockFramebuffer();
                for (size_t a = 0; a < _aovBindings.size(); ++a) {
                    HdEmbreeRenderBufferInterface *rb =
                        dynamic_cast<HdEmbreeRenderBufferInterface*>(
                            _aovBindings[a].renderBuffer);
                    rb->Resolve();
                    rb->BlockFill(stride);
                }
            }
        }

        // Clear the sample accumulation so the full-resolution passes
        // start from a clean slate, while the display buffer retains
        // the coarse preview for visual continuity.
        if (!renderThread->IsStopRequested()) {
            for (size_t a = 0; a < _aovBindings.size(); ++a) {
                HdEmbreeRenderBufferInterface *rb =
                    dynamic_cast<HdEmbreeRenderBufferInterface*>(
                        _aovBindings[a].renderBuffer);
                rb->ClearSamples();
            }
            // Reset adaptive sampling state that was partially filled
            // by the coarse passes.
            std::fill(_pixelMean.begin(), _pixelMean.end(), GfVec3f(0.0f));
            std::fill(_pixelM2.begin(), _pixelM2.end(), GfVec3f(0.0f));
            std::fill(
                _pixelSampleCount.begin(), _pixelSampleCount.end(), 0);
            std::fill(
                _pixelConverged.begin(), _pixelConverged.end(), false);
        }
    }

    // ---- Full-resolution multi-sample rendering ----
    // Each pass adds one sample per pixel.  After every pass we resolve
    // the accumulation buffer so the display shows progressively
    // improving quality.
    bool renderFinished = false;
    for (int i = 0; i < _samplesToConvergence; ++i) {
        // Pause point.
        while (renderThread->IsPauseRequested()) {
            if (renderThread->IsStopRequested()) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        // Cancellation point.
        if (renderThread->IsStopRequested()) {
            break;
        }

        {
            HD_TRACE_SCOPE("HdEmbreeRenderer::TraceSamplePass");
            WorkParallelForN(numTilesX * numTilesY,
                std::bind(&HdEmbreeRenderer::_RenderTiles, this,
                    renderThread, i, baseSeed, /*stride=*/1u,
                    std::placeholders::_1, std::placeholders::_2));
        }

        // Resolve intermediate results so the viewport shows progressive
        // refinement instead of staying blank until convergence.
        {
            HD_TRACE_SCOPE("HdEmbreeRenderer::ResolveSamplePass");
            auto lock = renderThread->LockFramebuffer();
            for (size_t a = 0; a < _aovBindings.size(); ++a) {
                HdEmbreeRenderBufferInterface *rb =
                    dynamic_cast<HdEmbreeRenderBufferInterface*>(
                        _aovBindings[a].renderBuffer);
                rb->Resolve();
            }
        }

        // After the first pass, mark the single-sampled attachments as
        // converged and unmap them. If there are no multisampled attachments,
        // we are done.
        if (i == 0) {
            bool moreWork = false;
            for (size_t a = 0; a < _aovBindings.size(); ++a) {
                HdEmbreeRenderBufferInterface *rb = dynamic_cast<HdEmbreeRenderBufferInterface*>(
                    _aovBindings[a].renderBuffer);
                if (rb->IsMultiSampled()) {
                    moreWork = true;
                }
            }
            if (!moreWork) {
                _completedSamples.store(i + 1);
                renderFinished = true;
                break;
            }
        }

        // Track the number of completed samples for external consumption.
        _completedSamples.store(i + 1);

        // If adaptive sampling is enabled, check if all pixels converged.
        if (_enableAdaptiveSampling && !_pixelConverged.empty()) {
            HD_TRACE_SCOPE("HdEmbreeRenderer::CheckConvergence");
            bool allConverged = true;
            for (size_t p = 0; p < _pixelConverged.size(); ++p) {
                if (!_pixelConverged[p]) {
                    allConverged = false;
                    break;
                }
            }
            if (allConverged) {
                renderFinished = true;
                break;
            }
        }

        // Cancellation point.
        if (renderThread->IsStopRequested()) {
            break;
        }

        // If this is the last iteration, rendering completed naturally.
        if (i == _samplesToConvergence - 1) {
            renderFinished = true;
        }
    }

    // Mark the multisampled attachments as converged and unmap all buffers.
    {
        HD_TRACE_SCOPE("HdEmbreeRenderer::FinalizeAovs");
        for (size_t i = 0; i < _aovBindings.size(); ++i) {
            HdEmbreeRenderBufferInterface *rb = dynamic_cast<HdEmbreeRenderBufferInterface*>(
                _aovBindings[i].renderBuffer);
            rb->Unmap();
            rb->SetConverged(true);
        }
    }

    // Print render statistics only when rendering completed (not interrupted).
    if (renderFinished) {
        const float elapsedSec = GetRenderElapsedSeconds();
        const int completedSamples = _completedSamples.load();
        const int w = _dataWindow.GetWidth();
        const int h = _dataWindow.GetHeight();
        const long long totalSamples =
            static_cast<long long>(w) * h * completedSamples;

        std::printf("\n");
        std::printf("===== hdEmbree Render Statistics =====\n");
        std::printf("  Resolution       : %d x %d\n", w, h);
        std::printf("  Samples/pixel    : %d / %d\n",
                    completedSamples, _samplesToConvergence);
        std::printf("  Total samples    : %lld\n", totalSamples);
        std::printf("  Render time      : %.3f s\n", elapsedSec);
        if (elapsedSec > 0.0f) {
            std::printf("  Samples/sec      : %.0f\n",
                        totalSamples / static_cast<double>(elapsedSec));
            std::printf("  Pixels/sec       : %.0f\n",
                        (static_cast<double>(w) * h * completedSamples)
                            / elapsedSec);
        }
        std::printf("  Max bounces      : %d\n", _maxBounces);
        std::printf("  Light samples    : %d\n", _lightSamplesPerHit);
        std::printf("  Sampler sequence : %s\n",
                    HdEmbreeGetSamplerSequenceToken(_samplerSequence).GetText());
        const uint64_t sssCalls = _sssCallCount.load();
        if (sssCalls > 0) {
            const uint64_t sssSuccesses = _sssSuccessCount.load();
            const uint64_t sssWalkSteps = _sssWalkStepCount.load();
            const uint64_t sssIntersections = _sssIntersectionCount.load();
            const double successRate =
                100.0 * static_cast<double>(sssSuccesses)
                / static_cast<double>(sssCalls);
            const double avgStepsPerCall =
                static_cast<double>(sssWalkSteps)
                / static_cast<double>(sssCalls);
            const double avgStepsPerSuccess =
                (sssSuccesses > 0)
                    ? static_cast<double>(sssWalkSteps)
                        / static_cast<double>(sssSuccesses)
                    : 0.0;
            const double avgIntersectionsPerCall =
                static_cast<double>(sssIntersections)
                / static_cast<double>(sssCalls);

            std::printf(
                "  SSS walks        : %llu calls, %llu success (%.1f%%)\n",
                static_cast<unsigned long long>(sssCalls),
                static_cast<unsigned long long>(sssSuccesses),
                successRate);
            std::printf(
                "  SSS avg steps    : %.2f / call, %.2f / success\n",
                avgStepsPerCall,
                avgStepsPerSuccess);
            std::printf(
                "  SSS intersects   : %llu total, %.2f / call\n",
                static_cast<unsigned long long>(sssIntersections),
                avgIntersectionsPerCall);
        }

        if (_enableAdaptiveSampling && !_pixelConverged.empty()) {
            size_t convergedCount = 0;
            double avgSamples = 0.0;
            for (size_t p = 0; p < _pixelConverged.size(); ++p) {
                if (_pixelConverged[p]) {
                    ++convergedCount;
                }
                avgSamples += _pixelSampleCount[p];
            }
            avgSamples /= _pixelConverged.size();
            const double convergedPct =
                100.0 * convergedCount / _pixelConverged.size();
            std::printf("  Adaptive sampling: on (threshold=%.4f)\n",
                        _adaptiveThreshold);
            std::printf("  Converged pixels : %zu / %zu (%.1f%%)\n",
                        convergedCount, _pixelConverged.size(), convergedPct);
            std::printf("  Avg samples/pixel: %.1f\n", avgSamples);
        }

        std::printf("======================================\n");
        std::fflush(stdout);
    }
}

void
HdEmbreeRenderer::_EvaluatePixelSample(
    unsigned int x, unsigned int y,
    GfVec3f const& origin, GfVec3f const& dir,
    HdEmbreeSampler const& sampler,
    HdEmbreeRayDifferential const& rayDiff)
{
    _PixelSampleResult result;
    if (_needColor) {
        result = _enableLighting
            ? _IntegratePath(origin, dir, rayDiff, sampler.RootDomain())
            : _IntegrateUnlit(origin, dir, rayDiff, sampler.RootDomain());
    } else {
        // Geometric AOV-only renders need the primary hit but no radiance.
        result.primaryHit.ray.flags = 0;
        _PopulateRayHit(
            &result.primaryHit, origin, dir, 0.0f,
            std::numeric_limits<float>::max(),
            HdEmbree_RayMask::Camera);
        rtcIntersect1(_scene, &result.primaryHit);
    }

    if (_enableAdaptiveSampling && !_pixelConverged.empty()) {
        const GfVec3f rgb(
            result.color[0], result.color[1], result.color[2]);
        _UpdateVariance(this, x, y, rgb);
    }

    for (const auto& writer : _aovWriters) {
        if (!writer.buffer->IsConverged()) {
            writer.writeFn(
                this, writer, result.primaryHit, result.color, x, y);
        }
    }
}

void
HdEmbreeRenderer::_RenderTiles(HdRenderThread *renderThread, int sampleNum,
                               uint32_t baseSeed, unsigned int stride,
                               size_t tileStart, size_t tileEnd)
{
    const unsigned int minX = _dataWindow.GetMinX();
    unsigned int minY = _dataWindow.GetMinY();
    const unsigned int maxX = _dataWindow.GetMaxX() + 1;
    unsigned int maxY = _dataWindow.GetMaxY() + 1;

    // If a client does not use AOVs and we have no render buffers,
    // _height is 0 and we shouldn't use it to flip the data window.
    if (_height > 0) {
        // The data window is y-Down but the image line order
        // is from bottom to top, so we need to flip it.
        std::swap(minY, maxY);
        minY = _height - minY;
        maxY = _height - maxY;
    }

    const unsigned int tileSize =
        HdEmbreeConfig::GetInstance().tileSize;
    const unsigned int numTilesX =
        (_dataWindow.GetWidth() + tileSize - 1) / tileSize;

    // _RenderTiles gets a range of tiles; iterate through them.
    for (unsigned int tile = tileStart; tile < tileEnd; ++tile) {
        // Cancellation point.
        if (renderThread && renderThread->IsStopRequested()) {
            break;
        }

        // Compute the pixel location of tile boundaries.
        const unsigned int tileY = tile / numTilesX;
        const unsigned int tileX = tile - tileY * numTilesX;
        // (Above is equivalent to: tileX = tile % numTilesX)
        const unsigned int x0 = tileX * tileSize + minX;
        const unsigned int y0 = tileY * tileSize + minY;
        // Clamp to data window, in case tileSize doesn't
        // neatly divide its with and height.
        const unsigned int x1 = std::min(x0 + tileSize, maxX);
        const unsigned int y1 = std::min(y0 + tileSize, maxY);

        // Loop over pixels casting rays.
        for (unsigned int y = y0; y < y1; ++y) {
            // For coarse preview passes, only render sparse pixels
            // whose data-window-relative coordinates are multiples
            // of the stride.
            if (stride > 1 && ((y - minY) % stride != 0)) {
                continue;
            }
            for (unsigned int x = x0; x < x1; ++x) {
                if (stride > 1 && ((x - minX) % stride != 0)) {
                    continue;
                }

                // Skip converged pixels in adaptive sampling mode.
                const size_t pixelIdx = y * _width + x;
                if (_enableAdaptiveSampling
                    && !_pixelConverged.empty()
                    && _pixelConverged[pixelIdx]) {
                    continue;
                }

                // Create a per-pixel OpenQMC sampler.
                HdEmbreeSampler sampler(
                    baseSeed,
                    x,
                    y,
                    sampleNum,
                    _samplerSequence);

                GfVec3f origin;
                GfVec3f dir;
                HdEmbreeRayDifferential rayDiff;
                _SampleCameraRay(
                    x, y, minX, minY, sampler,
                    origin, dir, rayDiff);

                // Evaluate and write this pixel sample.
                _EvaluatePixelSample(x, y, origin, dir, sampler, rayDiff);
            }
        }
    }
}

PXR_NAMESPACE_CLOSE_SCOPE
