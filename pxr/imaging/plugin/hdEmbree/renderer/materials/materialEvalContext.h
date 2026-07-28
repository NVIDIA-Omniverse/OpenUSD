//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_MATERIAL_EVAL_CONTEXT_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_MATERIAL_EVAL_CONTEXT_H

#include "pxr/pxr.h"
#include "pxr/imaging/plugin/hdEmbree/renderer/colorManagement.h"

#include "pxr/base/gf/vec3f.h"

namespace mxcpp {
class TextureSystem;
}  // namespace mxcpp

PXR_NAMESPACE_OPEN_SCOPE

/// Renderer-owned services shared by geometry-build and hit-time material
/// evaluation. The render delegate stops rendering before updating frame or
/// time, and Embree callbacks only receive a const observer to this state.
struct HdEmbreeMaterialEvalServices
{
    mxcpp::TextureSystem const* textureSystem = nullptr;
    float frame = 0.0f;
    float time = 0.0f;
    HdEmbreeRenderColorSpace renderColorSpace =
        HdEmbreeRenderColorSpace::LinearRec709;
    GfVec3f luminanceCoefficients =
        HdEmbreeGetLuminanceCoefficients(
            HdEmbreeRenderColorSpace::LinearRec709);
};

PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_IMAGING_PLUGIN_HD_EMBREE_MATERIAL_EVAL_CONTEXT_H
