//
// Copyright 2016 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_INSTANCER_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_INSTANCER_H

#include <renderer/lights/lightLinking.h>

#include "pxr/base/tf/hashmap.h"
#include "pxr/base/tf/token.h"
#include "pxr/imaging/hd/instancer.h"
#include "pxr/imaging/hd/vtBufferSource.h"
#include "pxr/pxr.h"

PXR_NAMESPACE_OPEN_SCOPE

/// \class HdEmbreeInstancer
///
/// HdEmbree implements instancing by adding prototype geometry to the BVH
/// multiple times within HdEmbreeMesh::Sync(). Transform and resolved Hydra
/// category membership travel together so nested flattening cannot misalign
/// light-linking identity with an instance transform.
///
/// Nested instancing can be handled by recursion, and by taking the
/// cartesian product of the transform arrays at each nesting level, to
/// create a flattened transform array.
///
struct HdEmbreeInstanceData
{
    GfMatrix4d transform{1.0};
    HdEmbreeCategorySet categories;
    /// Source index at the leaf/current instancer level after flattening.
    int sourceInstanceIndex = -1;
};

class HdEmbreeInstancer : public HdInstancer
{
public:
    /// Constructor.
    ///   \param delegate The scene delegate backing this instancer's data.
    ///   \param id The unique id of this instancer.
    HdEmbreeInstancer(HdSceneDelegate* delegate, SdfPath const& id);

    /// Destructor.
    ~HdEmbreeInstancer();

    /// Computes all instance transforms and effective category memberships
    /// for the provided prototype id,
    /// taking into account the scene delegate's instancerTransform and the
    /// instance primvars "hydra:instanceTransforms",
    /// "hydra:instanceTranslations", "hydra:instanceRotations", and
    /// "hydra:instanceScales". Computes and flattens nested transforms,
    /// if necessary.
    ///   \param prototypeId The prototype to compute transforms for.
    ///   \return One record per flattened instance, to apply when drawing.
    std::vector<HdEmbreeInstanceData> ComputeInstanceData(
        SdfPath const &prototypeId);

    HdDirtyBits GetInitialDirtyBitsMask() const override;

    /// Updates cached primvar data from the scene delegate.
    ///   \param sceneDelegate The scene delegate for this prim.
    ///   \param renderParam The hdEmbree render param.
    ///   \param dirtyBits The dirty bits for this instancer.
    void Sync(HdSceneDelegate *sceneDelegate,
              HdRenderParam   *renderParam,
              HdDirtyBits     *dirtyBits) override;

private:
    // Updates the cached primvars in _primvarMap based on scene delegate
    // data.  This is a helper function for Sync().
    void _SyncPrimvars(HdSceneDelegate *delegate, HdDirtyBits dirtyBits);

    // Map of the latest primvar data for this instancer, keyed by
    // primvar name. Primvar values are VtValue, an any-type; they are
    // interpreted at consumption time (here, in ComputeInstanceData).
    TfHashMap<TfToken,
              HdVtBufferSource*,
              TfToken::HashFunctor> _primvarMap;

    HdEmbreeCategorySet _categories;
    std::vector<HdEmbreeCategorySet> _instanceCategories;

    bool _visible;
};


PXR_NAMESPACE_CLOSE_SCOPE

#endif  // PXR_IMAGING_PLUGIN_HD_EMBREE_INSTANCER_H
