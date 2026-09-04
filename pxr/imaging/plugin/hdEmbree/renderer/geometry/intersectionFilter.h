//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_INTERSECTION_FILTER_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_INTERSECTION_FILTER_H

#include <renderer/embreeCompat.h>

#include "pxr/pxr.h"

PXR_NAMESPACE_OPEN_SCOPE
namespace ty {

struct PrototypeContext;

/// Return whether a candidate is one of Embree's terminal round-linear
/// spheres that must be removed to keep authored BasisCurves endpoints open.
/// Invalid primitive IDs and all non-round-linear representations return
/// false.
bool IsOpenCurveEndpoint(
    PrototypeContext const& context,
    unsigned int primitiveId,
    float localU) noexcept;

/// Unified geometry filter. It preserves invalid packet lanes, removes only
/// authored terminal round-linear spheres, and then applies display culling.
/// FaceCullBypassRayId bypasses only culling, not endpoint removal.
void PrototypeGeometryFilter(
    RTCFilterFunctionNArguments const* arguments);

/// Bind PrototypeGeometryFilter for both intersection and occlusion queries.
void BindPrototypeGeometryFilter(RTCGeometry geometry);

} // namespace ty
PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_IMAGING_PLUGIN_HD_EMBREE_INTERSECTION_FILTER_H
