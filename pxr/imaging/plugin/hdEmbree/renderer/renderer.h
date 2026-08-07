//
// Copyright 2018 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_RENDERER_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_RENDERER_H

#include "colorManagement.h"
#include "renderSettings.h"

#include <renderer/geometry/context.h>
#include <renderer/geometry/displacementEvaluation.h>
#include <renderer/integrator/medium.h>
#include <renderer/integrator/sss.h>
#include <renderer/lights/light.h>
#include <renderer/lights/lightLinking.h>
#include <renderer/lights/lightRegistry.h>
#include <renderer/lights/lightSampler.h>
#include <renderer/materials/materialEvalContext.h>
#include <renderer/sampling/sampling.h>

#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/gf/rect2i.h"
#include "pxr/base/tf/token.h"
#include "pxr/imaging/hd/aov.h"
#include "pxr/imaging/hd/renderThread.h"
#include "pxr/pxr.h"

#include <embree4/rtcore.h>
#include <embree4/rtcore_device.h>
#include <embree4/rtcore_ray.h>

#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

namespace mxcpp {
struct AdobeOpenPbrPreparedSurface;
struct ShadingContext;
struct SurfaceClosure;
class TextureSystem;
}  // namespace mxcpp

PXR_NAMESPACE_OPEN_SCOPE
namespace ty {

#define HDEMBREE_AOV_TOKENS \
    (adaptiveHeatmap) \
    (ambocc)

TF_DECLARE_PUBLIC_TOKENS(AovTokens, HDEMBREE_AOV_TOKENS);

class RenderBufferInterface;

enum RayMask : uint32_t {
    None = 0,

    Camera = 1 << 0,
    Shadow = 1 << 1,
    Light = 1 << 2,

    Scene = Camera | Shadow,

    All = UINT_MAX,
};

/// Ray differential for tracking pixel footprint through bounces.
struct RayDifferential {
    bool hasDifferentials = false;
    GfVec3f rxOrigin, ryOrigin;       // offset ray origins (x/y pixel shift)
    GfVec3f rxDirection, ryDirection;  // offset ray directions
};

/// Physical depth-of-field state for the active camera.
struct CameraDepthOfField {
    float fStop = 0.0f;
    float focusDistance = 0.0f;
    float focalLength = 0.0f;

    /// \brief Compare all depth-of-field parameters exactly.
    ///
    /// \param other Value to compare; no tolerance or enablement rules apply.
    /// \return True when f-stop, focus distance, and focal length are equal.
    bool operator==(CameraDepthOfField const& other) const {
        return fStop == other.fStop &&
               focusDistance == other.focusDistance &&
               focalLength == other.focalLength;
    }

    /// \brief Test whether any depth-of-field parameter differs.
    ///
    /// \param other Value to compare using exact floating-point equality.
    /// \return Logical negation of \ref operator==.
    bool operator!=(CameraDepthOfField const& other) const {
        return !(*this == other);
    }
};

struct MediumState {
    bool active = false;
    mxcpp::MediumProperties medium;
    PrototypeContext const* ownerGeometry = nullptr;
    CategorySet const* categories = nullptr;
};

/// Hero-wavelength sampling state shared by path-transport helpers.
struct HeroWavelengthState {
    bool active = false;
    float wavelengthNm = 0.0f;
    float pdf = 0.0f;
};

/// \class Renderer
/// \brief Progressive CPU path tracer built on Embree.
///
/// Owns progressive sampling state and MaterialX texture services while
/// borrowing the Embree scene, renderer light records, and Hydra AOV buffers.
/// The delegate must stop rendering before mutating or replacing borrowed data.
class Renderer final
{
public:
    /// \brief Construct a renderer with hard-coded render-setting defaults.
    ///
    /// No scene or AOV buffers are bound until their setters are called.
    Renderer();

    /// \brief Destroy renderer-owned sampling and texture state.
    ///
    /// Does not release the borrowed Embree scene, lights, or AOV buffers.
    ~Renderer();

    /// \brief Select the Embree scene used for ray traversal.
    ///
    /// \param scene Borrowed scene handle. It must be non-null, remain valid,
    /// and not be mutated from the time \ref Render starts until it returns.
    void SetScene(RTCScene scene);

    /// \brief Set the y-down pixel region rendered into the AOV buffers.
    ///
    /// The window uses CameraUtilFraming semantics and triggers AOV
    /// revalidation. It must be non-empty and contained by every bound buffer.
    /// \param dataWindow Inclusive pixel bounds in render-buffer coordinates.
    void SetDataWindow(GfRect2i const& dataWindow);

    /// \brief Set the camera transforms used to generate rays.
    ///
    /// Stores both matrices and their inverses for subsequent renders.
    /// \param viewMatrix Invertible world-to-camera transform.
    /// \param projMatrix Invertible camera-to-NDC projection transform.
    void SetCamera(GfMatrix4d const& viewMatrix,
                   GfMatrix4d const& projMatrix);

    /// \brief Set the linear exposure multiplier for color samples.
    ///
    /// \param cameraExposureScale Finite, normally non-negative scale applied
    /// after path evaluation.
    void SetCameraExposureScale(float cameraExposureScale);

    /// \brief Set physical depth-of-field parameters for the active camera.
    ///
    /// Depth of field is enabled only for a perspective camera when all three
    /// values are finite and positive; other values select a pinhole camera.
    /// \param cameraDepthOfField Value copied for subsequent renders.
    void SetCameraDepthOfField(
        CameraDepthOfField const& cameraDepthOfField);

    /// \brief Set application frame and time values visible to materials.
    ///
    /// The frame also supplies the sampler seed when no explicit seed is set.
    /// \param frame Finite application frame value.
    /// \param time Finite application time value.
    void SetSceneFrameAndTime(float frame, float time);

    /// \brief Select the renderer's working color space.
    ///
    /// Supported values are Linear Rec.709, Linear AP1, and Raw. Raw bypasses
    /// all renderer-managed color transforms while retaining Rec.709 as the
    /// fallback basis for numerical algorithms that require RGB primaries.
    void SetRenderColorSpace(RenderColorSpace colorSpace);

    /// \brief Return renderer-owned services used for material evaluation.
    ///
    /// The returned observer has a stable address for this renderer's
    /// lifetime. The delegate must stop rendering before changing its frame
    /// or time values through \ref SetSceneFrameAndTime.
    MaterialEvalServices const* GetMaterialEvalServices() const {
        return &_materialEvalServices;
    }

    /// \brief Bind the AOVs written by subsequent renders.
    ///
    /// Copies the bindings but borrows each render-buffer pointer. Every
    /// non-null buffer must implement RenderBufferInterface and remain
    /// valid and unmodified until rendering stops or bindings are replaced.
    /// \param aovBindings Hydra AOV bindings to copy and later validate.
    void SetAovBindings(HdRenderPassAovBindingVector const& aovBindings);

    /// \brief Add or replace a renderer light under a scene path.
    ///
    /// Dome bookkeeping is updated atomically with the path map.
    /// \param lightPath Stable, unique key for the light.
    /// \param light Borrowed non-null record that must remain valid and
    /// unchanged until removed while rendering is stopped.
    void AddLight(SdfPath const& lightPath, LightData const* light);

    /// \brief Remove a renderer light and any matching dome entry.
    ///
    /// \param lightPath Path previously passed to \ref AddLight.
    /// \param light Exact borrowed pointer registered for that path; it is
    /// used to remove dome bookkeeping and must be passed before destruction.
    void RemoveLight(SdfPath const& lightPath,
                     LightData const* light);

    /// \brief Associate top-level Embree geometry with a finite light.
    ///
    /// Invalid geometry IDs and null light pointers are ignored.
    /// \param geometryId Committed top-level scene geometry ID.
    /// \param light Borrowed light record that must outlive the registration.
    void AddLightGeometry(unsigned int geometryId,
                          LightData const* light);

    /// \brief Remove a finite-light geometry association.
    ///
    /// \param geometryId Previously registered top-level geometry ID;
    /// RTC_INVALID_GEOMETRY_ID is ignored.
    /// \param light Expected registered pointer. A null pointer removes any
    /// record for \p geometryId; a non-null mismatch leaves it unchanged.
    void RemoveLightGeometry(unsigned int geometryId,
                             LightData const* light);

    /// \brief Return the currently copied AOV bindings.
    ///
    /// \return Reference owned by this renderer, valid until the next
    /// SetAovBindings call or renderer destruction. Its buffer pointers remain
    /// borrowed from the delegate.
    HdRenderPassAovBindingVector const& GetAovBindings() const {
        return _aovBindings;
    }

    /// \brief Return the identity of the current copied binding set.
    ///
    /// Every SetAovBindings call advances this value, including replacement
    /// with an equal vector. The app thread uses it to detect when another
    /// render pass replaced the shared renderer's borrowed buffers.
    uint64_t GetAovBindingsVersion() const {
        return _aovBindingsVersion;
    }

    /// \brief Apply resolved render settings while rendering is stopped.
    ///
    /// Clamps renderer invariants, reapplies process-wide MaterialXCpp and
    /// texture-cache policy, and stores the normalized settings. The caller
    /// must not race this operation with rendering.
    /// \param settings Fully resolved Hydra values with typed token settings.
    void SetRenderSettings(RenderSettings const& settings);

    /// Return the normalized settings used by subsequent renders.
    RenderSettings const& GetRenderSettings() const {
        return _settings;
    }

    /// \brief Select Hydra's lit or unlit presentation mode.
    ///
    /// This application state is independent of scene-light availability and
    /// renderer settings. The caller must stop rendering before changing it.
    /// \param lightingEnabled True for path-traced radiance; false for
    /// authored display-color presentation.
    void SetLightingEnabled(bool lightingEnabled);

    /// Return the Hydra lighting state used by subsequent renders.
    bool GetLightingEnabled() const {
        return _lightingEnabled;
    }

    /// \brief Set Hydra's display wire color and default line width.
    ///
    /// Mesh repr descriptors decide whether a hit uses these values. A zero
    /// color retains Hydra's default edge-on-surface dimming behavior.
    /// \param color Linear render-space RGB and blend amount in alpha.
    /// \param lineWidth Positive screen-space width in pixels.
    void SetWireframeStyle(GfVec4f const& color, float lineWidth);

    /// \brief Progressively render the current scene into bound AOVs.
    ///
    /// Commits the scene, maps AOVs, runs coarse previews and full-resolution
    /// passes, resolves after each pass, then unmaps and marks AOVs converged.
    /// Scene, light, camera, and AOV state must remain unchanged until return.
    /// \param renderThread Non-null render-thread controller that must outlive
    /// the call; used for pause/stop checks and framebuffer locking.
    void Render(HdRenderThread* renderThread);

    /// \brief Mark the next frame as pending before starting its render thread.
    ///
    /// The render pass must call this immediately before every StartRender()
    /// so convergence checks cannot reuse the previous frame's validity.
    void MarkFramePending();

    /// \brief Report whether setup succeeded for the current or last frame.
    ///
    /// Safe to call from a client thread while Render runs on HdRenderThread.
    /// \return True only after the current Render invocation completed setup.
    bool DidLastFrameProduceValidPixels() const;

    /// \brief Clear authored AOV values and adaptive accumulation.
    ///
    /// Validates bindings, clears buffers that have non-empty clear values,
    /// and marks those buffers unconverged. Borrowed buffers must be writable.
    void Clear();

    /// \brief Reset progressive accumulation without erasing display output.
    ///
    /// Clears sample storage and adaptive statistics, preserving resolved
    /// pixels so the previous image remains visible until new samples arrive.
    /// An empty binding set has no accumulation to reset and returns silently.
    void ResetAccumulation();

    /// \brief Mark every currently bound AOV buffer unconverged.
    ///
    /// Null bindings and buffers of another implementation are skipped.
    void MarkAovBuffersUnconverged();

    /// \brief Get the completed full-resolution sample-pass count.
    ///
    /// \return Atomic snapshot for the current or most recent Render call;
    /// coarse preview passes are excluded.
    int GetCompletedSamples() const;

    /// \brief Get elapsed wall-clock time for the current render invocation.
    ///
    /// \return Seconds since the most recent Render call initialized its
    /// timer. The value continues increasing after that call returns.
    float GetRenderElapsedSeconds() const;

    /// \brief Get the number of SSS random walks attempted.
    ///
    /// \return Atomic count for the current or most recent Render call.
    uint64_t GetSssCallCount() const;

    /// \brief Get the number of successful SSS random walks.
    ///
    /// \return Atomic count for the current or most recent Render call.
    uint64_t GetSssSuccessCount() const;

    /// \brief Get total steps taken by SSS random walks.
    ///
    /// \return Atomic count for the current or most recent Render call.
    uint64_t GetSssWalkStepCount() const;

    /// \brief Get total Embree intersections issued by SSS walks.
    ///
    /// \return Atomic count for the current or most recent Render call.
    uint64_t GetSssIntersectionCount() const;

    /// \brief Get ambient-visibility rays issued for the current frame.
    ///
    /// \return Atomic ray count for the current or most recent Render call.
    uint64_t GetAmbientOcclusionRayCount() const;

private:
    struct _SurfaceInteraction;

    /// Setup state shared by the render thread and convergence client.
    enum class _FrameStatus {
        Pending,
        Valid,
        Failed
    };

    /// Result produced by exactly one selected camera-ray integrator.
    struct _PixelSampleResult {
        /// First Embree intersection, retained unchanged for geometric AOVs.
        RTCRayHit primaryHit{};
        /// Linear, unexposed RGBA radiance; alpha is one when computed.
        GfVec4f color = GfVec4f(0.0f);
        /// Binary ambient visibility, or zero when no AO ray is issued.
        float ambientVisibility = 0.0f;
    };

    /// \brief Prepare shared state immediately before tracing.
    ///
    /// The borrowed scene must be non-null. At least one AOV must be bound;
    /// every binding must expose RenderBufferInterface, have a
    /// supported format and matching non-zero dimensions, and contain the
    /// non-empty data window.
    ///
    /// On success, commits the scene, allocates adaptive state, builds AOV
    /// dispatch, maps every binding exactly once, and returns true. On
    /// failure, emits a specific diagnostic, marks each usable bound buffer
    /// converged, leaves every buffer unmapped, clears invocation-derived
    /// state, and returns false.
    bool _PreRenderSetup();

    /// \brief Validate the current bindings for rendering and clearing.
    ///
    /// Rechecks buffer implementation, format, dimensions, clear values, and
    /// data-window containment on every call because buffer properties can
    /// change without replacing the binding.
    /// \return True when every required buffer, format, and clear value is
    /// compatible; false when Render must stop early.
    bool _ValidateAovBindings();

    /// \brief Convert a Hydra color clear value to float RGBA.
    ///
    /// \param clearValue Scalar vec3/vec4 float or double value.
    /// \return Converted RGBA, adding alpha one for vec3; unsupported or
    /// array values produce opaque black.
    static GfVec4f _GetClearColor(VtValue const& clearValue);

    /// \brief Sample one primary camera ray for a render pixel.
    ///
    /// Applies pixel jitter, projection, depth of field, world transformation,
    /// and ray-differential scaling. Sampling advances only the camera domains
    /// of \p sampler.
    /// \param x Render-buffer x coordinate inside the active data window.
    /// \param y Render-buffer y coordinate inside the active data window.
    /// \param imageMinX X origin used to normalize the active data window.
    /// \param imageMinY Y origin after renderer line-order conversion.
    /// \param sampler Per-pixel sampler; must remain valid for this call and
    /// is advanced through its camera-jitter and camera-lens domains.
    /// \param outPosRayOrgWld Receives the finite world-space primary-ray
    /// origin.
    /// \param outDirRayWld Receives a normalized finite world-space
    /// primary-ray direction.
    /// \param outDiffRay Receives matching primary-ray differentials;
    /// hasDifferentials is false if depth-of-field projection is invalid.
    void _SampleCameraRay(
        unsigned int x, unsigned int y,
        unsigned int imageMinX, unsigned int imageMinY,
        Sampler& sampler,
        GfVec3f& outPosRayOrgWld, GfVec3f& outDirRayWld,
        RayDifferential& outDiffRay) const;

    /// \brief Render a half-open range of square tiles.
    ///
    /// Generates camera rays and dispatches AOV writes for selected pixels;
    /// a stride above one samples only the preview lattice.
    /// \param renderThread Optional controller used only for cancellation;
    /// when non-null it must outlive this call.
    /// \param sampleNum Zero-based full-resolution sample index.
    /// \param baseSeed Frame-wide deterministic sampler seed.
    /// \param stride Positive pixel stride; one renders every eligible pixel.
    /// \param tileStart First linear tile index, inclusive.
    /// \param tileEnd Final linear tile index, exclusive.
    void _RenderTiles(HdRenderThread* renderThread, int sampleNum,
                      uint32_t baseSeed, unsigned int stride,
                      size_t tileStart, size_t tileEnd);

    /// \brief Evaluate and write one selected pixel sample.
    ///
    /// Selects the lit or unlit integrator when radiance is required; an
    /// AOV-only sample performs just one primary intersection. The selected
    /// integrator owns its camera intersection and returns the retained hit.
    /// \param x Render-buffer x coordinate within the active data window.
    /// \param y Render-buffer y coordinate within the active data window.
    /// \param posRayOrgWld Finite world-space camera-ray origin.
    /// \param dirRayWld Normalized finite world-space camera-ray direction.
    /// \param sampler Per-pixel sampler that remains valid for the call.
    /// \param diffRay Pixel-footprint differentials for this camera ray.
    void _EvaluatePixelSample(
        unsigned int x, unsigned int y,
        GfVec3f const& posRayOrgWld, GfVec3f const& dirRayWld,
        Sampler const& sampler,
        RayDifferential const& diffRay);

    /// \brief Integrate a single-hit display-color sample.
    ///
    /// This integrator owns the primary intersection, samples only authored
    /// display color, and performs no material or light evaluation.
    /// \param posRayOrgWld Finite world-space camera-ray origin.
    /// \param dirRayWld Normalized finite world-space camera-ray direction.
    /// \return Unlit radiance and the unchanged primary intersection.
    _PixelSampleResult _IntegrateUnlit(
        GfVec3f const& posRayOrgWld,
        GfVec3f const& dirRayWld);

    /// Return true when the camera hit requests unlit edge-only display.
    bool _IsEdgeOnlyWireframeHit(RTCRayHit const& primaryHit) const;

    /// Composite the active mesh repr's display wire over one camera sample.
    void _ApplyWireframe(
        RTCRayHit const& primaryHit,
        RayDifferential const& diffRay,
        GfVec4f* color) const;

    /// \brief Compute camera or normalized clip depth for a hit.
    ///
    /// \param rayHit Initialized Embree intersection result.
    /// \param depth Non-null output written only on success.
    /// \param clip True for [0,1] projected depth; false for ray distance.
    /// \return True when geometry was hit and \p depth was written.
    bool _ComputeDepth(RTCRayHit const& rayHit, float* depth, bool clip);

    /// \brief Resolve a Hydra ID AOV value for a geometry hit.
    ///
    /// \param rayHit Valid renderer geometry hit; finite-light hits fail.
    /// \param idType One of `primId`, `elementId`, or `instanceId`.
    /// \param id Non-null output written only on success.
    /// \return True when \p idType is supported and \p id was written.
    bool _ComputeId(RTCRayHit const& rayHit,
                    TfToken const& idType,
                    int32_t* id);

    /// \brief Resolve and normalize a smooth hit normal.
    ///
    /// \param rayHit Valid renderer geometry hit; misses and lights fail.
    /// \param normal Non-null output written only on success.
    /// \param eye True for camera space; false for world space.
    /// \return True when a geometry normal was written.
    bool _ComputeNormal(RTCRayHit const& rayHit,
                        GfVec3f* normal,
                        bool eye);

    /// \brief Sample a numeric primvar at an Embree hit.
    ///
    /// Vec2 and scalar values are packed into the leading components.
    /// \param rayHit Valid renderer geometry hit; misses and lights fail.
    /// \param primvar Name to look up in the hit prototype context.
    /// \param value Non-null vec3 output written only on success.
    /// \return True when the primvar exists and has a supported numeric type.
    bool _ComputePrimvar(RTCRayHit const& rayHit,
                         TfToken const& primvar,
                         GfVec3f* value);

    /// \brief Draw one cosine-weighted ambient-visibility sample for a hit.
    ///
    /// Uses the resolved smooth/displaced surface normal without evaluating a
    /// material closure. Misses, finite lights, and invalid renderer geometry
    /// issue no visibility ray and return zero.
    /// \param rayHit Initialized primary intersection result.
    /// \param domain Deterministic sample domain reserved for AO draws.
    /// \return Zero when occluded or no ray is issued; one when unoccluded.
    float _ComputeAmbientOcclusion(
        RTCRayHit const& rayHit,
        SampleDomain const& domain);

    /// \brief Estimate direct surface lighting from all linked scene lights.
    ///
    /// Uses MIS and MaterialXCpp BSDF evaluation when a closure is supplied;
    /// otherwise it evaluates a synthetic Lambertian response.
    /// \param interaction Immutable hit position, geometric/smooth normals,
    /// smooth-shadow offset, and geometric side for this surface event.
    /// \param normalShdWldOut Normalized material-resolved shading normal.
    /// \param omegaOutWld Normalized world-space direction toward the previous
    /// vertex.
    /// \param domain Sample domain reserved for this lighting event.
    /// \param includeBsdfSamplingMis Whether to weight light samples against
    /// the competing BSDF-sampling technique.
    /// \param closure Optional borrowed closure valid for the call.
    /// \param receiverCategories Light-link categories of the receiver.
    /// \param mediumState Current participating medium for shadow attenuation.
    /// \param spectralActive Whether hero-wavelength evaluation is active.
    /// \param heroWavelengthNm Hero wavelength in nanometres when active.
    /// \param heroWavelengthPdf Positive wavelength PDF when active.
    /// \param adobeOpenPbrSurface Optional borrowed prepared Adobe surface,
    /// valid for the call and corresponding to \p closure.
    /// \return Linear RGB direct-light contribution before path throughput.
    GfVec3f _ComputeDirectLightingMIS(
        _SurfaceInteraction const& interaction,
        GfVec3f const& normalShdWldOut, GfVec3f const& omegaOutWld,
        SampleDomain const& domain,
        bool includeBsdfSamplingMis, mxcpp::SurfaceClosure const* closure,
        CategorySet const& receiverCategories,
        MediumState const& mediumState = MediumState(),
        bool spectralActive = false, float heroWavelengthNm = 0.0f,
        float heroWavelengthPdf = 0.0f,
        mxcpp::AdobeOpenPbrPreparedSurface const* adobeOpenPbrSurface =
            nullptr) const;

    /// \brief Estimate direct lighting at a participating-medium event.
    ///
    /// Samples every linked light and evaluates the active volume phase model.
    /// \param posWld World-space scattering position.
    /// \param omegaOutWld Normalized direction toward the previous path vertex.
    /// \param mediumState Active, scattering medium and receiver categories.
    /// \param domain Sample domain reserved for this medium-lighting event.
    /// \param includePhaseSamplingMis Whether to weight light samples against
    /// the competing phase-sampling technique.
    /// \param spectralActive Whether hero-wavelength evaluation is active.
    /// \param heroWavelengthNm Hero wavelength in nanometres when active.
    /// \param heroWavelengthPdf Positive wavelength PDF when active.
    /// \return Linear RGB direct-light contribution before path throughput;
    /// black when the medium is inactive or absorption-only.
    GfVec3f _ComputeMediumDirectLighting(GfVec3f const& posWld,
                                         GfVec3f const& omegaOutWld,
                                         MediumState const& mediumState,
                                         SampleDomain const& domain,
                                         bool includePhaseSamplingMis,
                                         bool spectralActive = false,
                                         float heroWavelengthNm = 0.0f,
                                         float heroWavelengthPdf = 0.0f) const;

    /// Mutable transport state shared by the main loop and focused event
    /// handlers. Borrowed category and geometry pointers remain scene-owned.
    struct _PathState {
        GfVec3f radianceAccumulated = GfVec3f(0.0f);
        GfVec3f throughputRgb = GfVec3f(1.0f);
        float throughputSpectral = 1.0f;
        HeroWavelengthState hero;

        GfVec3f posRayOrgWld = GfVec3f(0.0f);
        GfVec3f dirRayWld = GfVec3f(0.0f);
        RayDifferential diffRay;
        MediumState medium;

        float lastBsdfPdf = 0.0f;
        bool lastScatterWasMedium = false;
        CategorySet const* lastScatterCategories = nullptr;
        LightSampler::SamplingMode lastLightSamplingMode =
            LightSampler::SamplingMode::FullSphere;
        GfVec3f lastLightSamplingNormal = GfVec3f(0.0f);

        bool isFirstBounce = true;
        bool hasDiffuseLikeAncestor = false;
        bool currentPathIsCaustic = false;

        bool useSyntheticLambertian = false;
        SssOutput syntheticLambertianExit;
    };

    struct _VolumeTransmissionInput {
        float surfaceDist = std::numeric_limits<float>::infinity();
        bool hasFiniteLightHit = false;
        LightSampler::LightSample finiteLightHit;
        TfToken finiteLightLink;
        float finiteLightDist = std::numeric_limits<float>::infinity();
        int bounce = 0;
    };

    enum class _VolumeTransmissionResult {
        ContinueSurface,
        ContinueRay,
        Terminate
    };

    /// \brief Apply an RGB transport weight in the active path representation.
    ///
    /// \param weight Finite, non-negative RGB transport multiplier.
    /// \param state Non-null path state updated in RGB or hero-wavelength mode.
    /// The inline definition lives in `heroWavelength.h`.
    void _ApplyPathWeight(GfVec3f const& weight, _PathState* state) const;

    /// \brief Convert current path throughput to display RGB.
    ///
    /// \param state Path state with a valid hero PDF when spectral mode is active.
    /// \return RGB representation of the current transport throughput.
    /// The inline definition lives in `heroWavelength.h`.
    GfVec3f _GetPathThroughputRgb(_PathState const& state) const;

    /// \brief Weight RGB radiance by current path throughput.
    ///
    /// \param radiance Linear RGB radiance at the current path vertex.
    /// \param state Current path throughput and spectral representation.
    /// \return Throughput-weighted linear RGB radiance.
    GfVec3f _WeightPathRadiance(GfVec3f const& radiance,
                                _PathState const& state) const;

    /// \brief Accumulate a pre-weighted path contribution with firefly clamps.
    ///
    /// \param radianceContribution Linear RGB contribution after path
    /// throughput.
    /// \param state Non-null state whose radiance accumulator is updated.
    void _AddPathRadiance(GfVec3f radianceContribution,
                          _PathState* state) const;

    /// \brief Transport a path segment through the current medium.
    ///
    /// Applies transmittance, may accumulate a finite-light or scattering
    /// contribution, and may replace the ray after a volume event.
    /// \param input Immutable endpoint distances, light data, and bounce index.
    /// \param domain Sample domain reserved for volume transport.
    /// \param state Non-null complete path state; its medium must be active.
    /// \return ContinueSurface to shade the pending surface, ContinueRay for
    /// a new volume-scattered ray, or Terminate to end the path.
    _VolumeTransmissionResult _TraceVolumeTransmission(
        _VolumeTransmissionInput const& input,
        SampleDomain const& domain,
        _PathState* state) const;

    /// \brief Enter or leave the single active interior medium.
    ///
    /// \param closure Surface closure that may define an interior medium.
    /// \param geometry Non-null scene-owned boundary geometry.
    /// \param categories Scene-owned receiver categories for the medium.
    /// \param directionDotNormal Signed outgoing direction versus the
    /// world-oriented geometric normal; negative enters and positive exits.
    /// \param state Non-null path state whose medium ownership is updated.
    void _UpdatePathMedium(
        mxcpp::SurfaceClosure const& closure,
        PrototypeContext const* geometry,
        CategorySet const& categories,
        float directionDotNormal,
        _PathState* state) const;

    /// \brief Accumulate the camera background or indirect environment.
    ///
    /// Applies camera visibility policy, light linking, and emitter-hit MIS.
    /// \param state Non-null path state containing the missed ray direction
    /// and previous scattering data.
    void _AccumulateEnvironment(_PathState* state) const;

    enum class _SubsurfaceResult {
        ContinueAtExit,
        Terminate
    };

    struct _SubsurfaceInput {
        RTCRayHit const* rayHit = nullptr;
        InstanceContext const* instanceContext = nullptr;
        mxcpp::SurfaceClosure const* closure = nullptr;
        GfVec3f posHitWld = GfVec3f(0.0f);
        GfVec3f normalShdLobeWldOut = GfVec3f(0.0f);
        GfVec3f normalSrfWldOut = GfVec3f(0.0f);
        GfVec3f normalGeomWldOut = GfVec3f(0.0f);
        GfVec3f omegaOutWld = GfVec3f(0.0f);
        GfVec3f dirEntryWld = GfVec3f(0.0f);
        GfVec3f entryWeight = GfVec3f(0.0f);
        bool hasSampledEntryDirection = false;
    };

    /// \brief Execute a selected subsurface event and schedule its exit.
    ///
    /// \param input Non-null hit, instance, closure, and sampled-entry data.
    /// Borrowed pointers remain valid for the call.
    /// \param domain Sample domain reserved for subsurface transport.
    /// \param state Non-null complete path state updated on success.
    /// \return ContinueAtExit when state contains a synthetic exit event;
    /// Terminate when entry validation or the random walk fails.
    _SubsurfaceResult _TraceSubsurface(
        _SubsurfaceInput const& input,
        SampleDomain const& domain,
        _PathState* state) const;

    enum class _BaseNormalDerivativeStatus {
        Deferred,
        Ready,
        Failed
    };

    enum class _ResolvedNormalDerivativeProvenance {
        None,
        ExactMaterial,
        BaseApproximation
    };

    struct _SurfaceDifferentials {
        GfVec3f dndu = GfVec3f(0.0f);
        GfVec3f dndv = GfVec3f(0.0f);
        GfVec3f dpdx = GfVec3f(0.0f);
        GfVec3f dpdy = GfVec3f(0.0f);
        float dudx = 0.0f;
        float dvdx = 0.0f;
        float dudy = 0.0f;
        float dvdy = 0.0f;
        _BaseNormalDerivativeStatus baseNormalStatus =
            _BaseNormalDerivativeStatus::Failed;
        _ResolvedNormalDerivativeProvenance resolvedNormalProvenance =
            _ResolvedNormalDerivativeProvenance::None;
    };

    /// Hit-local surface data with topology and shading meanings kept
    /// separate. Both normals are normalized world-space values with authored
    /// exterior orientation and are never faced toward the current path.
    struct _SurfaceInteraction {
        GfVec3f posHitWld = GfVec3f(0.0f);
        GfVec3f normalGeomWldExt = GfVec3f(0.0f);
        GfVec3f normalSrfWldExt = GfVec3f(0.0f);
        InstanceContext const* instanceContext = nullptr;
        PrototypeContext const* prototypeContext = nullptr;
        unsigned int primitiveId =
            std::numeric_limits<unsigned int>::max();
        float baryU = 0.0f;
        float baryV = 0.0f;
        DisplacedSubdivFrame displacedFrame;
        bool frontFacing = true;
        bool doubleSided = false;

        GfVec3f
        GetNormalGeomWldOut() const
        {
            return frontFacing ? normalGeomWldExt : -normalGeomWldExt;
        }

        GfVec3f
        GetNormalSrfWldOut() const
        {
            return frontFacing ? normalSrfWldExt : -normalSrfWldExt;
        }
    };

    /// Compute the smooth-triangle visibility-origin lift on first demand.
    /// Genuine mesh corners and Embree barycentrics are used; authored-st
    /// derivatives are deliberately unrelated to this calculation.
    GfVec3f _ComputeSmoothShadowOffsetOut(
        _SurfaceInteraction const& interaction) const;

    /// \brief Propagate or discard ray differentials after a BSDF sample.
    ///
    /// \param surface Differential geometry at the sampled surface.
    /// \param posHitWld World-space surface position.
    /// \param normalShdWldOut Finite unit world-space shading normal on the
    /// incident transport side; it need not face `omegaOutWld` at grazing.
    /// \param omegaOutWld Normalized direction toward the previous path vertex.
    /// \param omegaInWld Normalized sampled continuation direction.
    /// \param eta Explicit iorIn/iorOut ratio. One denotes reflection; zero
    /// disables refracted differential propagation.
    /// \param specular Whether the sampled event is delta/specular.
    /// \param diffRay Non-null in/out differential state.
    void _PropagateRayDifferential(
        _SurfaceDifferentials const& surface, GfVec3f const& posHitWld,
        GfVec3f const& normalShdWldOut, GfVec3f const& omegaOutWld,
        GfVec3f const& omegaInWld, float eta, bool specular,
        RayDifferential* diffRay) const;

    /// \brief Integrate a complete multi-bounce camera path with MIS.
    ///
    /// Owns the primary intersection and handles camera/secondary light hits
    /// and misses in the same path loop with segment-specific policy.
    /// \param posRayOrgWld Finite world-space camera-ray origin.
    /// \param dirRayWld Normalized finite world-space camera-ray direction.
    /// \param diffRay Initial pixel-footprint differential state.
    /// \param domain Root path sample domain with lifetime covering the call.
    /// \return Path radiance and the unchanged primary intersection.
    _PixelSampleResult _IntegratePath(
        GfVec3f const& posRayOrgWld,
        GfVec3f const& dirRayWld,
        RayDifferential const& diffRay,
        SampleDomain const& domain) const;

    /// \brief Trace colored shadow visibility along a segment.
    ///
    /// Accounts for linking, transparent surfaces, and participating media.
    /// \param posWld World-space segment origin at a surface or medium
    /// event.
    /// \param dirOffsetReferenceWld Direction used to bias the ray origin.
    /// Surface events supply their geometric normal; medium events supply
    /// \p dirShadowWld because no surface frame exists.
    /// \param dirShadowWld Normalized direction toward the light.
    /// \param distanceWld Positive maximum trace distance.
    /// \param shadowLink Light shadow-link token to test against blockers.
    /// \param mediumState Medium initially containing the shadow segment.
    /// \param conservativeOriginInstance Optional NEE-origin instance whose
    /// own thick dielectric may not use approximate straight traversal.
    /// \param conservativeOriginPrototype Matching prototype for that origin.
    /// \return Per-channel visibility in [0,1].
    GfVec3f _Visibility(
        GfVec3f const& posWld, GfVec3f const& dirOffsetReferenceWld,
        GfVec3f const& dirShadowWld, float distanceWld,
        TfToken const& shadowLink,
        MediumState const& mediumState = MediumState(),
        InstanceContext const* conservativeOriginInstance = nullptr,
        PrototypeContext const* conservativeOriginPrototype = nullptr) const;

    /// \brief Find the nearest analytic finite light along a ray.
    ///
    /// \param position World-space ray origin.
    /// \param direction Normalized world-space ray direction.
    /// \param maxDist Positive exclusive search limit.
    /// \param outSample Non-null output written only when a light is found.
    /// \param outLightLink Optional output for the found light-link token.
    /// \return True when outputs describe a visible finite light before
    /// \p maxDist; false leaves outputs unchanged.
    bool _FindNearestFiniteLightHit(
        GfVec3f const& position,
        GfVec3f const& direction,
        float maxDist,
        LightSampler::LightSample* outSample,
        TfToken* outLightLink) const;

    /// \brief Resolve a top-level Embree hit to its registered finite light.
    ///
    /// \param rayHit Initialized hit; instanced geometry is not a light hit.
    /// \return Borrowed registered light pointer, valid until unregistered, or
    /// null when the hit is not finite-light geometry.
    LightData const* _GetLightGeometryHit(
        RTCRayHit const& rayHit) const;

    /// \brief Evaluate radiance for a registered finite-light geometry hit.
    ///
    /// \param rayHit Initialized top-level light-geometry hit.
    /// \param position World-space ray origin used for light evaluation.
    /// \param direction Normalized world-space direction to the hit.
    /// \param outSample Non-null output written on success; its distance is
    /// forced to the Embree hit distance.
    /// \param outLightLink Optional output for the light-link token.
    /// \return True for registered light geometry with a writable output.
    bool _EvaluateLightGeometryHit(
        RTCRayHit const& rayHit,
        GfVec3f const& position,
        GfVec3f const& direction,
        LightSampler::LightSample* outSample,
        TfToken* outLightLink = nullptr) const;

    struct _ShadingContextOptions {
        /// \brief Construct shading-context feature options.
        ///
        /// \param computeScreenSpaceDerivatives True to derive texture
        /// differentials from \ref RayDifferential.
        explicit _ShadingContextOptions(
            bool computeScreenSpaceDerivatives = true,
            bool computeObjectSpacePosition = true)
            : computeScreenSpaceDerivatives(computeScreenSpaceDerivatives),
              computeObjectSpacePosition(computeObjectSpacePosition)
        {
        }

        bool computeScreenSpaceDerivatives;
        bool computeObjectSpacePosition;
    };

    /// \brief Build MaterialX inputs for a renderer geometry hit.
    ///
    /// Samples primvars and derivatives and constructs a world-space tangent
    /// frame. Borrowed context pointers and sampler data must remain valid for
    /// the call; the returned context borrows this renderer's texture system.
    /// \param rayHit Valid hit belonging to \p prototypeContext.
    /// \param diffRay Differential state for the incident ray.
    /// \param instanceContext Non-null instance context for the hit.
    /// \param prototypeContext Non-null prototype context for the hit.
    /// \param interaction Valid central hit state. Its outward base frame
    /// is side-transformed exactly once for material evaluation.
    /// \param outDndu Optional world-space normal-u derivative output.
    /// \param outDndv Optional world-space normal-v derivative output.
    /// \param options Controls optional derivative work.
    /// \return MaterialX shading context valid while this renderer and all
    /// pointers installed by the caller remain alive.
    mxcpp::ShadingContext _BuildShadingContext(
        RTCRayHit const& rayHit,
        RayDifferential const& diffRay,
        InstanceContext const* instanceContext,
        PrototypeContext const* prototypeContext,
        _SurfaceInteraction const& interaction,
        GfVec3f* outDndu = nullptr,
        GfVec3f* outDndv = nullptr,
        _ShadingContextOptions options = _ShadingContextOptions()) const;

    /// \brief Construct topology and outward base shading state for a hit.
    ///
    /// Reuses the shared smooth/displaced normal resolver.
    /// `normalGeomWldExt` comes only from the orientation-correct Embree facet
    /// normal. Invalid normals fail.
    bool _TryBuildSurfaceInteraction(
        RTCRayHit const& rayHit, GfVec3f const& omegaOutWld,
        _SurfaceInteraction* outInteraction,
        InstanceContext const** outInstance = nullptr,
        PrototypeContext const** outPrototype = nullptr) const;

    /// \brief Evaluate the visibility-only material closure at a hit.
    ///
    /// Catches graph-evaluation failures and does not retain output pointers.
    /// \param rayHit Valid renderer geometry hit; misses and lights fail.
    /// \param outClosure Non-null output written only after successful graph
    /// evaluation.
    /// \param normalGeomWldExtOutput Optional normalized authored-exterior
    /// geometric normal output.
    /// \param outInstance Optional borrowed instance pointer output, valid
    /// while its scene geometry user data remains registered.
    /// \param outGeometry Optional borrowed prototype pointer output, valid
    /// while the scene geometry user data remains registered; it may be set
    /// even when no material graph is bound.
    /// \return True only when a bound graph evaluates successfully.
    bool _TryEvalSurfaceClosureAtHit(
        RTCRayHit const& rayHit, GfVec3f const& omegaOutWld,
        mxcpp::SurfaceClosure* outClosure,
        GfVec3f* normalGeomWldExtOutput = nullptr,
        InstanceContext const** outInstance = nullptr,
        PrototypeContext const** outGeometry = nullptr) const;

    // ---- AOV output classification (built once in _PreRenderSetup) ----

    enum class _AovKind {
        Color,
        CameraDepth,
        Depth,
        Id,
        Normal,
        EyeNormal,
        Primvar,
        AdaptiveHeatmap,
        AmbientOcclusion
    };

    struct _AovOutput {
        RenderBufferInterface* buffer;
        _AovKind kind;
        TfToken token;
    };

    /// \brief Classify the current validated AOVs for direct output.
    ///
    /// Borrows buffer interfaces and stores them until the next setup.
    void _ClassifyAovOutputs();

    /// \brief Write one sample to a classified AOV.
    ///
    /// The output buffer must be valid and unconverged. Hit-dependent AOVs
    /// retain their cleared value on misses except ID AOVs, which write -1,
    /// and ambient occlusion, which writes its specified miss value of zero.
    /// \param aov Classified output borrowing a non-null buffer.
    /// \param result Current sample values and initialized primary hit.
    /// \param x In-bounds render-buffer x coordinate.
    /// \param y In-bounds render-buffer y coordinate.
    void _WriteAov(_AovOutput const& aov,
                   _PixelSampleResult const& result,
                   unsigned int x,
                   unsigned int y);

    /// \brief Map a normalized sample-count fraction to heatmap RGBA.
    ///
    /// \param t Fraction normally in [0,1]; values above one are clamped.
    /// \return Opaque blue-to-cyan-to-green-to-yellow-to-red color.
    static GfVec4f _HeatmapColor(float t);

    /// \brief Add one color sample to a pixel's adaptive statistics.
    ///
    /// Updates Welford mean/variance and may mark the pixel converged.
    /// \param x In-bounds render-buffer x coordinate.
    /// \param y In-bounds render-buffer y coordinate.
    /// \param rgb Finite linear RGB sample.
    void _UpdateVariance(unsigned int x,
                         unsigned int y,
                         GfVec3f const& rgb);

    // The bound aovs for this renderer.
    HdRenderPassAovBindingVector _aovBindings;
    // Parsed AOV name tokens.
    HdParsedAovTokenVector _aovNames;
    // App-thread binding identity; advances even for equal replacement sets.
    uint64_t _aovBindingsVersion;

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
    // Linear exposure scale from the active camera.
    float _cameraExposureScale;
    // Physical depth-of-field state from the active camera.
    CameraDepthOfField _cameraDepthOfField;

    // Our handle to the embree scene.
    RTCScene _scene;

    // Normalized renderer-consumed settings applied as one stopped update.
    RenderSettings _settings;

    // Pass-owned Hydra presentation state, separate from render settings and
    // scene-light availability.
    bool _lightingEnabled;

    // Hydra display wire style. Per-mesh reprs decide whether it is active.
    GfVec4f _wireframeColor;
    float _wireframeLineWidth;

    // Shared MaterialX texture backend for the whole renderer.
    std::unique_ptr<mxcpp::TextureSystem> _textureSystem;

    // Working color space selected by UsdRenderSettings.
    RenderColorSpace _renderColorSpace;

    // Stable-address services shared by geometry-build and hit-time material
    // evaluation. _textureSystem is declared first so it is initialized before
    // this non-owning observer is constructed.
    MaterialEvalServices _materialEvalServices;

    // Per-pixel adaptive sampling state (Welford online variance).
    std::vector<GfVec3f> _pixelMean;
    std::vector<GfVec3f> _pixelM2;
    std::vector<uint32_t> _pixelSampleCount;
    std::vector<bool> _pixelConverged;

    // How many samples have been completed.
    std::atomic<int> _completedSamples;

    // Whether the current frame passed setup and may write RenderProducts.
    std::atomic<_FrameStatus> _frameStatus{_FrameStatus::Pending};

    // SSS random-walk statistics accumulated over the current render.
    mutable std::atomic<uint64_t> _sssCallCount;
    mutable std::atomic<uint64_t> _sssSuccessCount;
    mutable std::atomic<uint64_t> _sssWalkStepCount;
    mutable std::atomic<uint64_t> _sssIntersectionCount;
    std::atomic<uint64_t> _ambientOcclusionRayCount;

    // Render start time for elapsed time tracking.
    std::chrono::steady_clock::time_point _renderStartTime;

    // Renderer-side light lookup and dome/geometry registration.
    LightRegistry _lights;

    // Pre-resolved per-frame state (built in _PreRenderSetup).
    bool _needRadiance = false;
    bool _needAmbientOcclusion = false;
    GfVec4f _colorClearValue;
    std::vector<_AovOutput> _aovOutputs;
};

} // namespace ty
PXR_NAMESPACE_CLOSE_SCOPE

#endif  // PXR_IMAGING_PLUGIN_HD_EMBREE_RENDERER_H
