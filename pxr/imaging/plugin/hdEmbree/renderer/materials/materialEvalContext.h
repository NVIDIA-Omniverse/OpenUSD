//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_MATERIAL_EVAL_CONTEXT_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_MATERIAL_EVAL_CONTEXT_H

#include <renderer/colorManagement.h>

#include "pxr/base/gf/vec3f.h"
#include "pxr/pxr.h"

namespace mxcpp {
class TextureSystem;
}  // namespace mxcpp

PXR_NAMESPACE_OPEN_SCOPE
namespace ty {

/// Renderer-owned services shared by geometry-build and hit-time material
/// evaluation. The render delegate stops rendering before updating frame or
/// time, and Embree callbacks only receive a const observer to this state.
struct MaterialEvalServices
{
    mxcpp::TextureSystem const* textureSystem = nullptr;
    float frame = 0.0f;
    float time = 0.0f;
    RenderColorSpace renderColorSpace =
        RenderColorSpace::LinearRec709;
    GfVec3f luminanceCoefficients =
        GetLuminanceCoefficients(
            RenderColorSpace::LinearRec709);
};

} // namespace ty
PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_IMAGING_PLUGIN_HD_EMBREE_MATERIAL_EVAL_CONTEXT_H
