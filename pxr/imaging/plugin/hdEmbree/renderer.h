//
// Copyright 2018 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_RENDERER_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_RENDERER_H

#include "pxr/pxr.h"

#include "pxr/imaging/plugin/hdEmbree/context.h"
#include "pxr/imaging/plugin/hdEmbree/light.h"
#include "pxr/imaging/plugin/hdEmbree/lightSamplers.h"
#include "pxr/imaging/plugin/hdEmbree/medium.h"
#include "pxr/imaging/plugin/hdEmbree/sampling.h"

#include "pxr/imaging/hd/aov.h"
#include "pxr/imaging/hd/renderThread.h"

#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/gf/rect2i.h"
#include "pxr/base/tf/token.h"

#include <embree4/rtcore.h>
#include <embree4/rtcore_device.h>
#include <embree4/rtcore_ray.h>

#include <atomic>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <mutex>

namespace mxcpp {
struct AdobeOpenPbrPreparedSurface;
struct ShadingContext;
struct SurfaceClosure;
class TextureSystem;
}  // namespace mxcpp

PXR_NAMESPACE_OPEN_SCOPE

class HdEmbreeRenderBuffer;
class HdEmbreeMesh;

enum HdEmbree_RayMask : uint32_t {
    None = 0,

    Camera = 1 << 0,
    Shadow = 1 << 1,

    All = UINT_MAX,
};

/// Ray differential for tracking pixel footprint through bounces.
struct HdEmbreeRayDifferential {
    bool hasDifferentials = false;
    GfVec3f rxOrigin, ryOrigin;       // offset ray origins (x/y pixel shift)
    GfVec3f rxDirection, ryDirection;  // offset ray directions
};

struct HdEmbreeMediumState {
    bool active = false;
    mxcpp::MediumProperties medium;
    HdEmbreeMesh* ownerMesh = nullptr;
};

/// \class HdEmbreeRenderer
///
/// HdEmbreeRenderer implements a renderer on top of Embree's raycasting
/// abilities.  This is currently a very simple renderer.  It breaks the
/// framebuffer into tiles for multithreading; sends out jittered camera
/// rays; and implements the following shading:
///  - Colors via the "color" primvar.
///  - Lighting via N dot Camera-ray, simulating a point light at the camera
///    origin.
///  - Ambient occlusion.
///
class HdEmbreeRenderer final
{
public:
    using WriteMutex = std::mutex;
    using ScopedLock = std::scoped_lock<WriteMutex>;

    /// Renderer constructor.
    HdEmbreeRenderer();

    /// Renderer destructor.
    ~HdEmbreeRenderer();

    /// Set the embree scene that this renderer should raycast into.
    ///   \param scene The embree scene to use.
    void SetScene(RTCScene scene);

    /// Set the data window to fill (same meaning as in CameraUtilFraming
    /// with coordinate system also being y-Down).
    void SetDataWindow(const GfRect2i& dataWindow);

    /// Set the camera to use for rendering.
    ///   \param viewMatrix The camera's world-to-view matrix.
    ///   \param projMatrix The camera's view-to-NDC projection matrix.
    void SetCamera(const GfMatrix4d& viewMatrix, const GfMatrix4d& projMatrix);

    /// Set the application frame/time values exposed to MaterialX shading.
    void SetSceneFrameAndTime(float frame, float time);

    /// Set the aov bindings to use for rendering.
    ///   \param aovBindings A list of aov bindings.
    void SetAovBindings(HdRenderPassAovBindingVector const& aovBindings);

    /// Add a light
    void AddLight(SdfPath const& lightPath, HdEmbree_Light* light);

    /// Remove a light
    void RemoveLight(SdfPath const& lightPath, HdEmbree_Light* light);

    /// Get the aov bindings being used for rendering.
    ///   \return the current aov bindings.
    HdRenderPassAovBindingVector const& GetAovBindings() const {
        return _aovBindings;
    }

    /// Set how many samples to render before considering an image converged.
    ///   \param samplesToConvergence How many samples are needed, per-pixel,
    ///                               before the image is considered finished.
    void SetSamplesToConvergence(int samplesToConvergence);

    /// Set how many samples to use for ambient occlusion.
    ///   \param ambientOcclusionSamples How many samples are needed for
    ///                                  ambient occlusion? 0 = disable.
    void SetAmbientOcclusionSamples(int ambientOcclusionSamples);

    /// Sets whether dome light direct camera visibility should be enabled.
    ///   \param domeLightCameraVisibility Whether dome lights should be
    ///                                    directly visible; i.e. contribute
    ///                                    color to camera ray misses.
    void SetDomeLightCameraVisibility(bool domeLightCameraVisibility);

    /// Sets whether to use scene colors while rendering.
    ///   \param enableSceneColors Whether drawing should sample color, or draw
    ///                            everything as white.
    void SetEnableSceneColors(bool enableSceneColors);

    /// Sets a number to seed the random number generator with.
    ///   \param randomNumberSeed If -1, then the random number generator
    ///                           is seeded in a non-deterministic way;
    ///                           otherwise, it is seeded with this value.
    void SetRandomNumberSeed(int randomNumberSeed);

    /// Sets whether to enable direct lighting (disables ambient occlusion).
    ///   \param enableLighting Whether drawing should evaluate direct lighting.
    void SetEnableLighting(bool enableLighting);

    /// Set path tracing parameters.
    void SetMaxBounces(int maxBounces);
    void SetMinBouncesBeforeRR(int minBounces);

    /// Legacy compatibility shim for selecting sobol vs random.
    void SetUseSobol(bool useSobol);

    /// Set the sampler sequence used for per-pixel sample generation.
    void SetSamplerSequence(HdEmbreeSamplerSequence sequence);

    /// Set adaptive sampling parameters.
    void SetEnableAdaptiveSampling(bool enable);
    void SetAdaptiveThreshold(float threshold);
    void SetMinSamplesBeforeAdaptive(int minSamples);

    /// Set light sampling parameters.
    void SetLightSamplesPerHit(int samples);
    void SetStratifyLightSamples(bool stratify);

    /// Set whether to show the adaptive sampling heatmap.
    void SetShowAdaptiveHeatmap(bool show);

    /// Set the firefly clamping threshold (max sample luminance).
    /// Values <= 0 disable clamping.
    void SetFireflyClampThreshold(float threshold);

    /// Set caustic path handling.
    void SetEnableCaustics(bool enable);
    void SetCausticsClampThreshold(float threshold);
    void SetApproxTransparentShadows(bool enable);

    /// Set whether GGX reflection uses microfacet multiple scattering.
    void SetEnableGgxMicrofacetMultipleScattering(bool enable);

    /// Set the rough dielectric layer throughput estimate mode.
    void SetDielectricLayerThroughputMode(TfToken const& mode);

    /// Set whether MaterialX OpenPBR uses the Adobe reference backend.
    void SetUseAdobeOpenPBR(bool enable);

    /// Rendering entrypoint: add one sample per pixel to the whole sample
    /// buffer, and then loop until the image is converged.  After each pass,
    /// the image will be resolved into a color buffer.
    ///   \param renderThread A handle to the render thread, used for checking
    ///                       for cancellation and locking the color buffer.
    void Render(HdRenderThread* renderThread);

    /// Clear the bound aov buffers (typically before rendering).
    void Clear();

    /// Reset progressive accumulation while preserving the resolved output
    /// buffers so the previous image remains visible until new samples arrive.
    void ResetAccumulation();

    /// Mark the aov buffers as unconverged.
    void MarkAovBuffersUnconverged();

    /// Get the number of samples completed so far.
    int GetCompletedSamples() const;

    /// Get elapsed render time in seconds since the last Render() call.
    float GetRenderElapsedSeconds() const;

    /// Get accumulated SSS random-walk statistics for the current render.
    uint64_t GetSssCallCount() const;
    uint64_t GetSssSuccessCount() const;
    uint64_t GetSssWalkStepCount() const;
    uint64_t GetSssIntersectionCount() const;

private:
    // Perform validation and setup immediately before starting a render
    void _PreRenderSetup();

    // Validate the internal consistency of aov bindings provided to
    // SetAovBindings. If the aov bindings are invalid, this will issue
    // appropriate warnings. If the function returns false, Render() will fail
    // early.
    //
    // This function thunks itself using _aovBindingsNeedValidation and
    // _aovBindingsValid.
    //   \return True if the aov bindings are valid for rendering.
    bool _ValidateAovBindings();

    // Return the clear color to use for the given VtValue.
    static GfVec4f _GetClearColor(VtValue const& clearValue);

    // Render square tiles of pixels. This function is one unit of threadpool
    // work. For each tile, iterate over pixels in the tile, generating camera
    // rays, and following them/calculating color with _TraceRay. This function
    // renders all tiles between tileStart and tileEnd.
    // When \p stride > 1, only pixels whose data-window-relative coordinates
    // are multiples of stride are rendered (used for coarse preview passes).
    void _RenderTiles(HdRenderThread* renderThread, int sampleNum,
                      uint32_t baseSeed, unsigned int stride,
                      size_t tileStart, size_t tileEnd);

    // Cast a ray into the scene and if it hits an object, write to the bound
    // aov buffers.
    void _TraceRay(unsigned int x, unsigned int y,
                   GfVec3f const& origin, GfVec3f const& dir,
                   HdEmbreeSampler const& sampler,
                   HdEmbreeRayDifferential const& rayDiff);

    // Compute the color at the given ray hit.
    GfVec4f _ComputeColor(RTCRayHit const& rayHit,
                          HdEmbreeRayDifferential const& rayDiff,
                          HdEmbreeSampler const& sampler,
                          GfVec4f const& clearColor);
    // Compute the depth at the given ray hit.
    bool _ComputeDepth(RTCRayHit const& rayHit, float* depth, bool clip);
    // Compute the given ID at the given ray hit.
    bool _ComputeId(RTCRayHit const& rayHit,
                    TfToken const& idType,
                    int32_t* id);
    // Compute the normal at the given ray hit.
    bool _ComputeNormal(RTCRayHit const& rayHit, GfVec3f* normal, bool eye);
    // Compute a primvar at the given ray hit.
    bool _ComputePrimvar(RTCRayHit const& rayHit, TfToken const& primvar,
                         GfVec3f* value);

    // Compute the ambient occlusion term at a given point by firing rays
    // from "position" in the hemisphere centered on "normal"; the occlusion
    // factor is the fraction of those rays that are visible.
    //
    // Modulating surface color by occlusionFactor is similar to taking
    // the light contribution of an infinitely far, pure white dome light.
    float _ComputeAmbientOcclusion(GfVec3f const& position,
                                   GfVec3f const& normal,
                                   HdEmbreeSampleDomain const& domain);

    /// Evaluate direct lighting from all scene lights using MIS.
    /// If \p closure is non-null, uses the MaterialXCpp BSDF evaluation;
    /// otherwise falls back to a simple Lambertian BRDF.
    /// \p normal is the BSDF normal; \p visibilityNormal is used only for
    /// shadow-ray origin bias.
    GfVec3f _ComputeDirectLightingMIS(
        GfVec3f const& position,
        GfVec3f const& normal,
        GfVec3f const& visibilityNormal,
        GfVec3f const& wo,
        HdEmbreeSampleDomain const& domain,
        bool doubleSided,
        mxcpp::SurfaceClosure const* closure,
        HdEmbreeMediumState const& mediumState = HdEmbreeMediumState(),
        bool spectralActive = false,
        float heroWavelengthNm = 0.0f,
        float heroWavelengthPdf = 0.0f,
        mxcpp::AdobeOpenPbrPreparedSurface const*
            adobeOpenPbrSurface = nullptr) const;

    GfVec3f _ComputeMediumDirectLighting(
        GfVec3f const& position,
        GfVec3f const& wo,
        HdEmbreeMediumState const& mediumState,
        HdEmbreeSampleDomain const& domain,
        bool spectralActive = false,
        float heroWavelengthNm = 0.0f,
        float heroWavelengthPdf = 0.0f) const;

    enum class _VolumeTransmissionResult {
        ContinueSurface,
        ContinueRay,
        Terminate
    };

    struct _VolumeTransmissionInput {
        GfVec3f rayOrigin;
        GfVec3f rayDir;
        float surfaceDist = std::numeric_limits<float>::infinity();
        bool hasFiniteLightHit = false;
        HdEmbreeLightSampler::LightSample finiteLightHit;
        float finiteLightDist = std::numeric_limits<float>::infinity();
        int bounce = 0;
        bool spectralActive = false;
        float heroWavelengthNm = 0.0f;
        float heroWavelengthPdf = 0.0f;
    };

    struct _VolumeTransmissionState {
        GfVec3f radiance;
        GfVec3f throughput;
        float spectralThroughput = 1.0f;
        GfVec3f rayOrigin;
        GfVec3f rayDir;
        HdEmbreeRayDifferential rayDiff;
        float lastBsdfPdf = 0.0f;
        bool lastScatterWasMedium = false;
        bool anyNonSpecularBounces = false;
        bool hasDiffuseLikeAncestor = false;
        bool currentPathIsCaustic = false;
        bool isFirstBounce = false;
    };

    _VolumeTransmissionResult _TraceVolumeTransmission(
        _VolumeTransmissionInput const& input,
        HdEmbreeMediumState const& mediumState,
        HdEmbreeSampleDomain const& domain,
        _VolumeTransmissionState* state) const;

    /// Multi-bounce path tracer with MIS.
    GfVec3f _TracePath(
        GfVec3f const& origin,
        GfVec3f const& dir,
        HdEmbreeRayDifferential const& rayDiff,
        HdEmbreeSampleDomain const& domain) const;

    // Return the visibility from `position` along `direction`
    GfVec3f _Visibility(GfVec3f const& position,
                        GfVec3f const& normal,
                        GfVec3f const& direction,
                        float dist,
                        HdEmbreeMediumState const& mediumState =
                            HdEmbreeMediumState()) const;

    bool _FindNearestFiniteLightHit(
        GfVec3f const& position,
        GfVec3f const& direction,
        float maxDist,
        HdEmbreeLightSampler::LightSample* outSample) const;

    // Should the ray continue based on the possibly intersected prim's visibility settings?
    bool _RayShouldContinue(RTCRayHit const& rayHit) const;

    // Build a ShadingContext from a ray hit, sampling primvars (normal,
    // texcoord, displayColor) and constructing the tangent frame.
    // The caller supplies the world-space normal (already transformed and
    // normalized) so that double-sided flipping can be handled externally.
    struct _ShadingContextOptions {
        explicit _ShadingContextOptions(
            bool computeScreenSpaceDerivatives = true)
            : computeScreenSpaceDerivatives(computeScreenSpaceDerivatives)
        {
        }

        bool computeScreenSpaceDerivatives;
    };

    mxcpp::ShadingContext _BuildShadingContext(
        RTCRayHit const& rayHit,
        HdEmbreeRayDifferential const& rayDiff,
        HdEmbreeInstanceContext const* instanceContext,
        HdEmbreePrototypeContext const* prototypeContext,
        GfVec3f const& hitPos,
        GfVec3f const& normal,
        GfVec3f* outDndu = nullptr,
        GfVec3f* outDndv = nullptr,
        _ShadingContextOptions options = _ShadingContextOptions()) const;

    // Evaluate a material closure at a ray hit.
    // Returns false if no material is bound or evaluation fails.
    bool _TryEvalSurfaceClosureAtHit(
        RTCRayHit const& rayHit,
        mxcpp::SurfaceClosure* outClosure,
        GfVec3f* outGeometricNormal = nullptr,
        HdEmbreeMesh** outMesh = nullptr) const;

    // ---- AOV dispatch table (built once per frame in _PreRenderSetup) ----

    struct _AovWriter;

    using _AovWriteFn = void (*)(HdEmbreeRenderer*,
                                 _AovWriter const&,
                                 RTCRayHit const&,
                                 GfVec4f const&,
                                 unsigned int, unsigned int);
    struct _AovWriter {
        HdEmbreeRenderBuffer* buffer = nullptr;
        _AovWriteFn writeFn = nullptr;
        TfToken token;
    };

    void _BuildAovDispatchTable();

    static void _WriteColor(HdEmbreeRenderer*, _AovWriter const&,
                            RTCRayHit const&, GfVec4f const&,
                            unsigned int, unsigned int);
    static void _WriteColorHeatmap(HdEmbreeRenderer*, _AovWriter const&,
                                   RTCRayHit const&, GfVec4f const&,
                                   unsigned int, unsigned int);
    static void _WriteDepth(HdEmbreeRenderer*, _AovWriter const&,
                            RTCRayHit const&, GfVec4f const&,
                            unsigned int, unsigned int);
    static void _WriteClipDepth(HdEmbreeRenderer*, _AovWriter const&,
                                RTCRayHit const&, GfVec4f const&,
                                unsigned int, unsigned int);
    static void _WriteId(HdEmbreeRenderer*, _AovWriter const&,
                         RTCRayHit const&, GfVec4f const&,
                         unsigned int, unsigned int);
    static void _WriteNormal(HdEmbreeRenderer*, _AovWriter const&,
                             RTCRayHit const&, GfVec4f const&,
                             unsigned int, unsigned int);
    static void _WriteNormalEye(HdEmbreeRenderer*, _AovWriter const&,
                                RTCRayHit const&, GfVec4f const&,
                                unsigned int, unsigned int);
    static void _WritePrimvar(HdEmbreeRenderer*, _AovWriter const&,
                              RTCRayHit const&, GfVec4f const&,
                              unsigned int, unsigned int);
    static void _WriteAdaptiveHeatmap(HdEmbreeRenderer*, _AovWriter const&,
                                      RTCRayHit const&, GfVec4f const&,
                                      unsigned int, unsigned int);

    static GfVec4f _HeatmapColor(float t);

    static void _UpdateVariance(HdEmbreeRenderer*,
                                unsigned int, unsigned int,
                                GfVec3f const&);

    // The bound aovs for this renderer.
    HdRenderPassAovBindingVector _aovBindings;
    // Parsed AOV name tokens.
    HdParsedAovTokenVector _aovNames;

    // Do the aov bindings need to be re-validated?
    bool _aovBindingsNeedValidation;
    // Are the aov bindings valid?
    bool _aovBindingsValid;

    // Data window - as in CameraUtilFraming.
    GfRect2i _dataWindow;

    // The width of the render buffers.
    unsigned int _width;
    // The height of the render buffers.
    unsigned int _height;

    // View matrix: world space to camera space.
    GfMatrix4d _viewMatrix;
    // Projection matrix: camera space to NDC space.
    GfMatrix4d _projMatrix;
    // The inverse view matrix: camera space to world space.
    GfMatrix4d _inverseViewMatrix;
    // The inverse projection matrix: NDC space to camera space.
    GfMatrix4d _inverseProjMatrix;

    // Our handle to the embree scene.
    RTCScene _scene;

    // How many samples should we render to convergence?
    int _samplesToConvergence;
    // How many samples should we use for ambient occlusion?
    int _ambientOcclusionSamples;
    // Should we enable scene colors?
    bool _enableSceneColors;
    // Should we sample dome lights on ray miss?
    bool _domeLightCameraVisibility;
    // If other than -1, use this to seed the random number generator with.
    int _randomNumberSeed;
    // Should we enable direct lighting from the scene?
    bool _enableLighting;

    // Path tracing parameters.
    int _maxBounces;
    int _minBouncesBeforeRR;

    // Active sampler sequence for per-pixel sample generation.
    HdEmbreeSamplerSequence _samplerSequence;

    // Adaptive sampling parameters.
    bool _enableAdaptiveSampling;
    float _adaptiveThreshold;
    int _minSamplesBeforeAdaptive;

    // Light sampling parameters.
    int _lightSamplesPerHit;
    bool _stratifyLightSamples;

    // Whether to visualize adaptive sampling as a heatmap.
    bool _showAdaptiveHeatmap;

    // Firefly clamping threshold (max sample luminance). <= 0 disables.
    float _fireflyClampThreshold;

    // Caustic path handling. When enabled, indirect caustic paths are
    // regularized and optionally clamped; when disabled, they are suppressed.
    bool _enableCaustics;
    float _causticsClampThreshold;

    // Biased straight-through shadow visibility for transparent surfaces.
    bool _approxTransparentShadows;

    // Whether GGX reflection uses microfacet multiple scattering compensation.
    bool _enableGgxMicrofacetMultipleScattering;

    // Rough dielectric top-layer throughput estimate mode.
    TfToken _dielectricLayerThroughputMode;

    // Whether MaterialX OpenPBR uses the Adobe reference backend.
    bool _useAdobeOpenPBR;

    // Shared MaterialX texture backend for the whole renderer.
    std::unique_ptr<mxcpp::TextureSystem> _textureSystem;

    // Application frame/time values propagated into MaterialX shading.
    float _sceneFrame;
    float _sceneTime;

    // Per-pixel adaptive sampling state (Welford online variance).
    std::vector<GfVec3f> _pixelMean;
    std::vector<GfVec3f> _pixelM2;
    std::vector<uint32_t> _pixelSampleCount;
    std::vector<bool> _pixelConverged;

    // How many samples have been completed.
    std::atomic<int> _completedSamples;

    // SSS random-walk statistics accumulated over the current render.
    mutable std::atomic<uint64_t> _sssCallCount;
    mutable std::atomic<uint64_t> _sssSuccessCount;
    mutable std::atomic<uint64_t> _sssWalkStepCount;
    mutable std::atomic<uint64_t> _sssIntersectionCount;

    // Render start time for elapsed time tracking.
    std::chrono::steady_clock::time_point _renderStartTime;

    // Lights
    mutable WriteMutex _lightsWriteMutex; // protects the 2 below
    std::map<SdfPath, HdEmbree_Light*> _lightMap;
    std::vector<HdEmbree_Light*> _domes;

    // Pre-resolved per-frame state (built in _PreRenderSetup).
    bool _needColor = false;
    GfVec4f _colorClearValue;
    std::vector<_AovWriter> _aovWriters;
};

PXR_NAMESPACE_CLOSE_SCOPE

#endif  // PXR_IMAGING_PLUGIN_HD_EMBREE_RENDERER_H
