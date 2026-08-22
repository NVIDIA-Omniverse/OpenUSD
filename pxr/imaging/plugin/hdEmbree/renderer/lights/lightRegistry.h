//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_LIGHT_REGISTRY_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_LIGHT_REGISTRY_H

#include "light.h"

#include <renderer/embreeCompat.h>

#include "pxr/pxr.h"
#include "pxr/usd/sdf/path.h"

#include <map>
#include <mutex>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE
namespace ty {

/// Owns renderer-side light lookup containers and their synchronization.
/// Light records remain borrowed from the delegate and must be immutable while
/// a render consumes the const views returned by this registry.
class LightRegistry final
{
public:
    using LightMap = std::map<SdfPath, LightData const*>;
    using DomeVector = std::vector<LightData const*>;

    /// Add or replace the light registered at \p path.
    void Add(SdfPath const& path, LightData const* light);

    /// Remove \p path and matching dome entries for \p light.
    void Remove(SdfPath const& path, LightData const* light);

    /// Associate top-level Embree geometry with a finite light.
    void AddGeometry(unsigned int geometryId,
                     LightData const* light);

    /// Remove a matching finite-light geometry association.
    void RemoveGeometry(unsigned int geometryId,
                        LightData const* light);

    /// Return the finite light represented by \p geometryId, or null.
    LightData const* FindGeometry(unsigned int geometryId) const;

    /// Return the path-keyed lights. Mutations must be stopped while retained.
    LightMap const& GetLights() const { return _lights; }

    /// Return dome lights. Mutations must be stopped while retained.
    DomeVector const& GetDomes() const { return _domes; }

private:
    mutable std::mutex _mutex;
    LightMap _lights;
    std::map<unsigned int, LightData const*> _geometryLights;
    DomeVector _domes;
};

} // namespace ty
PXR_NAMESPACE_CLOSE_SCOPE

#endif  // PXR_IMAGING_PLUGIN_HD_EMBREE_LIGHT_REGISTRY_H
