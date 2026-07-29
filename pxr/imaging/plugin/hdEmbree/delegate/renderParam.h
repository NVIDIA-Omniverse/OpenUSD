//
// Copyright 2017 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_RENDER_PARAM_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_RENDER_PARAM_H

#include <renderer/materials/materialEvalContext.h>

#include "pxr/imaging/hd/renderDelegate.h"
#include "pxr/imaging/hd/renderThread.h"
#include "pxr/pxr.h"

#include <embree4/rtcore.h>

PXR_NAMESPACE_OPEN_SCOPE

namespace ty {
class Renderer;
} // namespace ty

///
/// \class HdEmbreeRenderParam
///
/// The render delegate can create an object of type HdRenderParam, to pass
/// to each prim during Sync(). HdEmbree uses this class to pass top-level
/// embree state around.
///
class HdEmbreeRenderParam final : public HdRenderParam
{
public:
    HdEmbreeRenderParam(RTCDevice device, RTCScene scene,
                        HdRenderThread *renderThread,
                        ty::Renderer *renderer,
                        ty::MaterialEvalServices const* materialEvalServices,
                        std::atomic<int> *sceneVersion,
                        std::atomic<int> *materialVersion)
        : _scene(scene), _device(device)
        , _renderThread(renderThread), _renderer(renderer)
        , _materialEvalServices(materialEvalServices)
        , _sceneVersion(sceneVersion)
        , _materialVersion(materialVersion)
    {}

    /// Stop rendering, publish a scene-version change, and return the borrowed
    /// top-level scene for mutation. Callers must complete edits before
    /// rendering is restarted.
    RTCScene AcquireSceneForEdit() {
        NotifySceneChange();
        return _scene;
    }
    /// Stop rendering and publish a scene-version change for renderer-readable
    /// state that does not require direct access to the Embree scene.
    void NotifySceneChange() {
        _renderThread->StopRender();
        (*_sceneVersion)++;
    }
    /// Stop rendering and publish both scene and material version changes.
    /// The material version makes the render pass refresh stable prototype
    /// bindings before rendering can resume.
    void NotifyMaterialChange() {
        _renderThread->StopRender();
        (*_sceneVersion)++;
        (*_materialVersion)++;
    }
    /// Accessor for the top-level embree device (library handle).
    RTCDevice GetEmbreeDevice() { return _device; }

    ty::Renderer* GetRenderer() { return _renderer; }

    /// Return non-owning renderer services shared by all material evaluation.
    ty::MaterialEvalServices const* GetMaterialEvalServices() const {
        return _materialEvalServices;
    }

private:
    /// A handle to the top-level embree scene.
    RTCScene _scene;
    /// A handle to the top-level embree device (library handle).
    RTCDevice _device;
    /// A handle to the global render thread.
    HdRenderThread *_renderThread;
    ty::Renderer* _renderer;
    /// Renderer-owned state; valid for this render parameter's lifetime.
    ty::MaterialEvalServices const* _materialEvalServices;
    /// A version counter for edits to _scene.
    std::atomic<int> *_sceneVersion;
    /// A narrower version for compiled material edits.
    std::atomic<int> *_materialVersion;
};

PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_IMAGING_PLUGIN_HD_EMBREE_RENDER_PARAM_H
