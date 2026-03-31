//
// Copyright 2016 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_CONTEXT_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_CONTEXT_H

#include "pxr/pxr.h"

#include "pxr/imaging/plugin/hdEmbree/sampler.h"

#include "pxr/base/gf/matrix4f.h"
#include "pxr/base/vt/array.h"

#include <unordered_map>
#include <string>
#include "pxr/imaging/plugin/hdEmbree/MaterialXCpp/mxcpp_value.h"

#include <embree4/rtcore.h>

PXR_NAMESPACE_OPEN_SCOPE

class HdRprim;
class HdEmbreeMaterial;

/// \class HdEmbreePrototypeContext
///
/// A small bit of state attached to each bit of prototype geometry in embree,
/// for the benefit of HdEmbreeRenderer::_TraceRay.
///
struct HdEmbreePrototypeContext
{
    /// A pointer back to the owning HdEmbree rprim.
    HdRprim *rprim;
    /// A name-indexed map of primvar samplers.
    TfHashMap<TfToken, HdEmbreePrimvarSampler*, TfToken::HashFunctor>
        primvarMap;
    /// A copy of the primitive params for this rprim.
    VtIntArray primitiveParams;
    /// The bound material, or nullptr if none.
    HdEmbreeMaterial *material = nullptr;
    /// Per-mesh uniform primvar values for geompropvalueuniform nodes.
    /// Built once during Sync from HdInterpolationConstant primvars.
    std::unordered_map<std::string, mxcpp::Value> uniformPrimvarMap;
};

///
/// \class HdEmbreeInstanceContext
///
/// A small bit of state attached to each bit of instanced geometry in embree,
/// for the benefit of HdEmbreeRenderer::_TraceRay.
///
struct HdEmbreeInstanceContext
{
    /// The object-to-world transform, for transforming normals to worldspace.
    GfMatrix4f objectToWorldMatrix;
    /// The inverse world-to-object transform.
    GfMatrix4f worldToObjectMatrix;
    /// The scene the prototype geometry lives in, for passing to
    /// rtcInterpolate.
    RTCScene rootScene;
    /// The instance id of this instance.
    int32_t instanceId;
};


PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_IMAGING_PLUGIN_HD_EMBREE_CONTEXT_H
