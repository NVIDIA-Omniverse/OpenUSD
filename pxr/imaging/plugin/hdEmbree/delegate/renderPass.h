//
// Copyright 2016 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_RENDER_PASS_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_RENDER_PASS_H

#include "renderBuffer.h"

#include <renderer/renderer.h>

#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/gf/rect2i.h"
#include "pxr/base/tf/hashmap.h"
#include "pxr/base/tf/token.h"
#include "pxr/base/vt/value.h"
#include "pxr/imaging/hd/aov.h"
#include "pxr/imaging/hd/renderPass.h"
#include "pxr/imaging/hd/renderThread.h"
#include "pxr/pxr.h"
#include "pxr/usd/sdf/path.h"

#include <atomic>
#include <cstdint>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

/// \class HdEmbreeRenderPass
///
/// HdRenderPass represents a single render iteration, rendering a view of the
/// scene (the HdRprimCollection) for a specific viewer (the camera/viewport
/// parameters in HdRenderPassState) to the current draw target.
///
/// This class does so by raycasting into the embree scene via ty::Renderer.
///
class HdEmbreeRenderPass final : public HdRenderPass
{
public:
    /// Renderpass constructor.
    ///   \param index The render index containing scene data to render.
    ///   \param collection The initial rprim collection for this renderpass.
    ///   \param renderThread A handle to the global render thread.
    ///   \param renderer A handle to the global renderer.
    HdEmbreeRenderPass(HdRenderIndex *index,
                       HdRprimCollection const &collection,
                       HdRenderThread *renderThread,
                       ty::Renderer *renderer,
                       std::atomic<int> *sceneVersion,
                       std::atomic<int> *materialVersion);

    /// Renderpass destructor.
    ~HdEmbreeRenderPass() override;

    // -----------------------------------------------------------------------
    // HdRenderPass API

    /// Report whether every active output has reached its parking/completion
    /// state. This may be true after failed renderer setup and therefore does
    /// not imply a valid frame. Offline products are written only when this is
    /// true and the renderer atomically reports valid pixels.
    bool IsConverged() const override;

protected:

    // -----------------------------------------------------------------------
    // HdRenderPass API

    /// Draw the scene with the bound renderpass state.
    ///   \param renderPassState Input parameters (including viewer parameters)
    ///                          for this renderpass.
    ///   \param renderTags Which rendertags should be drawn this pass.
    void _Execute(HdRenderPassStateSharedPtr const& renderPassState,
                  TfTokenVector const &renderTags) override;

    /// Update internal tracking to reflect a dirty collection.
    void _MarkCollectionDirty() override;

private:
    /// Report whether all AOV bindings installed by this pass have reached
    /// their parking/completion state. Returns false before the first
    /// Execute(). This says nothing about whether renderer setup produced a
    /// valid frame.
    ///
    /// The app thread reads buffer convergence while the render thread writes
    /// it atomically. A renderer binding-generation match proves this pass is
    /// current; binding generation/vector replacement is app-thread-only.
    bool _HasConverged() const;

    /// Reconcile active RenderSettings opinions owned by this bridge with the
    /// delegate settings map.
    ///
    /// A delegate value equal to the effective value produced by its last
    /// bridged opinion remains bridge-owned: it may be updated or reset when
    /// the authored opinion disappears, using its descriptor default when
    /// present and an empty value otherwise. Authored and effective snapshots
    /// are separate because setting boundaries may normalize values. Differing
    /// direct delegate/UI values are preserved. Returns true only when the
    /// delegate settings version changed.
    bool _UpdateRenderSettingsFromActiveRenderSettingsPrim();

    /// Write supported color/raw raster products to their authored productName
    /// paths when enableInteractive is false and an active RenderSettings prim
    /// supplies products.
    ///
    /// IsConverged calls this only after both AOV convergence and atomic valid
    /// frame publication. Missing scene-index, product, or color-AOV
    /// prerequisites return without output; no fallback product is written.
    /// The pass attempts this at most once after each render start, even when
    /// the attempt writes nothing. usdrender independently treats every
    /// missing expected product as an error.
    void _WriteActiveRenderProducts();

    // The collection repr state whose resolved mesh modes were last synced.
    HdReprSelector _lastCollectionReprSelector;
    bool _lastCollectionForcedRepr;

    // A handle to the render thread.
    HdRenderThread *_renderThread;

    // A handle to the global renderer.
    ty::Renderer *_renderer;

    // A reference to the global scene version.
    std::atomic<int> *_sceneVersion;

    // The last scene version we rendered with.
    int _lastSceneVersion;

    // A reference to and the last observed compiled-material version.
    std::atomic<int> *_materialVersion;
    int _lastMaterialVersion;

    // The last settings version we rendered with.
    int _lastSettingsVersion;
    // Whether renderer settings have been applied at least once.
    bool _hasAppliedRendererSettings;

    // Identity and presence of the last active RenderSettings prim.
    SdfPath _lastRenderSettingsPrimPath;
    bool _hasAppliedRenderSettingsPrim;
    // Authored values still owned by the RenderSettings-to-delegate bridge.
    // These remain unnormalized so scene-index changes are compared exactly.
    TfHashMap<TfToken, VtValue, TfToken::HashFunctor>
        _lastBridgedRenderSettings;
    // Effective delegate values produced by those authored opinions. A
    // matching current delegate value remains eligible for update/reset even
    // when a setting boundary (for example minCurveWidth) normalized it.
    TfHashMap<TfToken, VtValue, TfToken::HashFunctor>
        _lastBridgedDelegateValues;

    // The last material render-context priority order seen by this pass.
    TfTokenVector _lastMaterialRenderContexts;

    // The last application frame/time forwarded to MaterialX shading.
    double _lastFrame;
    double _lastTime;

    // The pixels written to. Like viewport in OpenGL,
    // but coordinates are y-Down.
    GfRect2i _dataWindow;

    // The view matrix: world space to camera space
    GfMatrix4d _viewMatrix;
    // The projection matrix: camera space to NDC space (with
    // respect to the data window).
    GfMatrix4d _projMatrix;

    // Camera state frozen for adaptive subdivision unless dynamic updates are
    // explicitly enabled.
    bool _hasSubdivisionCamera;
    bool _dynamicSubdivisionTessellation;
    bool _subdivisionSceneUpdatePending;
    bool _subdivisionDisplacementUpdatePending;
    GfMatrix4d _subdivisionViewMatrix;
    GfMatrix4d _subdivisionProjMatrix;
    GfRect2i _subdivisionDataWindow;

    // The linear camera exposure scale applied to color output.
    float _cameraExposureScale;
    // The active camera's physical depth-of-field state.
    ty::CameraDepthOfField _cameraDepthOfField;
    // Last Hydra lit/unlit presentation state forwarded to the renderer.
    bool _lightingEnabled;
    // Last Hydra display wire style forwarded to the renderer.
    GfVec4f _wireframeColor;
    float _wireframeLineWidth;

    // The list of aov buffers this renderpass should write to.
    HdRenderPassAovBindingVector _aovBindings;

    // Whether this pass has installed its bindings in the shared renderer.
    // A replacement pass must not observe or reuse its predecessor's buffers.
    bool _hasInstalledAovBindings;
    // Renderer binding identity recorded by this pass's last installation.
    uint64_t _aovBindingsVersion;

    // If no attachments are provided, provide an anonymous renderbuffer for
    // color and depth output.
    HdEmbreeRenderBuffer _colorBuffer;
    HdEmbreeRenderBuffer _depthBuffer;

    // Whether product output has already been attempted for this render.
    bool _renderProductsWritten;
};

PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_IMAGING_PLUGIN_HD_EMBREE_RENDER_PASS_H
