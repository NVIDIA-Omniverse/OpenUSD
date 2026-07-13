//
// Copyright 2016 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/imaging/plugin/hdEmbree/delegate/instancer.h"

#include "pxr/imaging/plugin/hdEmbree/renderer/geometry/primvarSampler.h"
#include "pxr/imaging/hd/sceneDelegate.h"
#include "pxr/imaging/hd/tokens.h"

#include "pxr/base/gf/vec3f.h"
#include "pxr/base/gf/vec4f.h"
#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/gf/rotation.h"
#include "pxr/base/gf/quaternion.h"
#include "pxr/base/tf/staticTokens.h"

PXR_NAMESPACE_OPEN_SCOPE


HdEmbreeInstancer::HdEmbreeInstancer(HdSceneDelegate* delegate,
                                     SdfPath const& id)
    : HdInstancer(delegate, id)
    , _visible(true)
{
}

HdEmbreeInstancer::~HdEmbreeInstancer()
{
    TF_FOR_ALL(it, _primvarMap) {
        delete it->second;
    }
    _primvarMap.clear();
}

HdDirtyBits
HdEmbreeInstancer::GetInitialDirtyBitsMask() const
{
    return HdInstancer::GetInitialDirtyBitsMask() |
        HdChangeTracker::DirtyCategories;
}

void
HdEmbreeInstancer::Sync(HdSceneDelegate* delegate,
                        HdRenderParam* renderParam,
                        HdDirtyBits* dirtyBits)
{
    if (*dirtyBits & HdChangeTracker::DirtyVisibility) {
        _visible = delegate->GetVisible(GetId());
    }

    if (*dirtyBits & HdChangeTracker::DirtyCategories) {
        _categories = delegate->GetCategories(GetId());
        _instanceCategories = delegate->GetInstanceCategories(GetId());
    }

    _UpdateInstancer(delegate, dirtyBits);

    if (HdChangeTracker::IsAnyPrimvarDirty(*dirtyBits, GetId())) {
        _SyncPrimvars(delegate, *dirtyBits);
    }
}

void
HdEmbreeInstancer::_SyncPrimvars(HdSceneDelegate* delegate,
                                 HdDirtyBits dirtyBits)
{
    HD_TRACE_FUNCTION();
    HF_MALLOC_TAG_FUNCTION();

    SdfPath const& id = GetId();

    HdPrimvarDescriptorVector primvars =
        delegate->GetPrimvarDescriptors(id, HdInterpolationInstance);

    for (HdPrimvarDescriptor const& pv: primvars) {
        if (HdChangeTracker::IsPrimvarDirty(dirtyBits, id, pv.name)) {
            VtValue value = delegate->Get(id, pv.name);
            if (!value.IsEmpty()) {
                if (_primvarMap.count(pv.name) > 0) {
                    delete _primvarMap[pv.name];
                }
                _primvarMap[pv.name] =
                    new HdVtBufferSource(pv.name, value);
            }
        }
    }
}

std::vector<HdEmbreeInstanceData>
HdEmbreeInstancer::ComputeInstanceData(SdfPath const &prototypeId)
{
    HD_TRACE_FUNCTION();
    HF_MALLOC_TAG_FUNCTION();

    // The transforms for this level of instancer are computed by:
    // foreach(index : indices) {
    //     instancerTransform
    //     * hydra:instanceTranslations(index)
    //     * hydra:instanceRotations(index)
    //     * hydra:instanceScales(index)
    //     * hydra:instanceTransforms(index)
    // }
    // If any transform isn't provided, it's assumed to be the identity.

    if (!_visible) {
        return {};
    }

    GfMatrix4d instancerTransform =
        GetDelegate()->GetInstancerTransform(GetId());
    VtIntArray instanceIndices =
        GetDelegate()->GetInstanceIndices(GetId(), prototypeId);

    std::vector<HdEmbreeInstanceData> instances(instanceIndices.size());
    for (size_t i = 0; i < instanceIndices.size(); ++i) {
        HdEmbreeInstanceData& instance = instances[i];
        instance.transform = instancerTransform;
        instance.sourceInstanceIndex = instanceIndices[i];
        HdEmbreeMergeCategories(_categories, &instance.categories);
        const int sourceIndex = instanceIndices[i];
        if (sourceIndex >= 0 &&
            static_cast<size_t>(sourceIndex) < _instanceCategories.size()) {
            HdEmbreeMergeCategories(
                _instanceCategories[sourceIndex], &instance.categories);
        } else if (!_instanceCategories.empty()) {
            TF_CODING_ERROR(
                "Instance category index %d is out of range for instancer %s",
                sourceIndex, GetId().GetText());
        }
    }

    // "hydra:instanceTranslations" holds a translation vector for each index.
    if (_primvarMap.count(HdInstancerTokens->instanceTranslations) > 0) {
        HdEmbreeBufferSampler
                sampler(*_primvarMap[HdInstancerTokens->instanceTranslations]);
        for (size_t i = 0; i < instanceIndices.size(); ++i) {
            GfVec3f translate;
            if (sampler.Sample(instanceIndices[i], &translate)) {
                GfMatrix4d translateMat(1);
                translateMat.SetTranslate(GfVec3d(translate));
                instances[i].transform =
                    translateMat * instances[i].transform;
            }
        }
    }

    // "hydra:instanceRotations" holds a quaternion in <real, i, j, k>
    // format for each index.
    if (_primvarMap.count(HdInstancerTokens->instanceRotations) > 0) {
        HdEmbreeBufferSampler sampler(*_primvarMap[HdInstancerTokens->instanceRotations]);
        for (size_t i = 0; i < instanceIndices.size(); ++i) {
            GfVec4f quat;
            if (sampler.Sample(instanceIndices[i], &quat)) {
                GfMatrix4d rotateMat(1);
                rotateMat.SetRotate(GfQuatd(
                    quat[0], quat[1], quat[2], quat[3]));
                instances[i].transform = rotateMat * instances[i].transform;
            }
        }
    }

    // "hydra:instanceScales" holds an axis-aligned scale vector for each index.
    if (_primvarMap.count(HdInstancerTokens->instanceScales) > 0) {
        HdEmbreeBufferSampler sampler(*_primvarMap[HdInstancerTokens->instanceScales]);
        for (size_t i = 0; i < instanceIndices.size(); ++i) {
            GfVec3f scale;
            if (sampler.Sample(instanceIndices[i], &scale)) {
                GfMatrix4d scaleMat(1);
                scaleMat.SetScale(GfVec3d(scale));
                instances[i].transform = scaleMat * instances[i].transform;
            }
        }
    }

    // "hydra:instanceTransforms" holds a 4x4 transform matrix for each index.
    if (_primvarMap.count(HdInstancerTokens->instanceTransforms) > 0) {
        HdEmbreeBufferSampler
                sampler(*_primvarMap[HdInstancerTokens->instanceTransforms]);
        for (size_t i = 0; i < instanceIndices.size(); ++i) {
            GfMatrix4d instanceTransform;
            if (sampler.Sample(instanceIndices[i], &instanceTransform)) {
                instances[i].transform =
                    instanceTransform * instances[i].transform;
            }
        }
    }

    if (GetParentId().IsEmpty()) {
        return instances;
    }

    HdInstancer *parentInstancer =
        GetDelegate()->GetRenderIndex().GetInstancer(GetParentId());
    if (!TF_VERIFY(parentInstancer)) {
        return instances;
    }

    // The transforms taking nesting into account are computed by:
    // parentInstances = parentInstancer->ComputeInstanceData(GetId())
    // foreach (parent : parentInstances, instance : instances) {
    //     compose parent and child transform/category data
    // }
    std::vector<HdEmbreeInstanceData> parentInstances =
        static_cast<HdEmbreeInstancer*>(parentInstancer)->
            ComputeInstanceData(GetId());

    std::vector<HdEmbreeInstanceData> final(
        parentInstances.size() * instances.size());
    for (size_t i = 0; i < parentInstances.size(); ++i) {
        for (size_t j = 0; j < instances.size(); ++j) {
            HdEmbreeInstanceData& output = final[i * instances.size() + j];
            output.transform = instances[j].transform *
                parentInstances[i].transform;
            output.sourceInstanceIndex = instances[j].sourceInstanceIndex;
            HdEmbreeMergeCategories(
                parentInstances[i].categories, &output.categories);
            HdEmbreeMergeCategories(instances[j].categories, &output.categories);
        }
    }
    return final;
}

PXR_NAMESPACE_CLOSE_SCOPE
