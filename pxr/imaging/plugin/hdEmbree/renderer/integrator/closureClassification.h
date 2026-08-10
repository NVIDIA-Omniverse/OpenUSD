//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
// Transport classification of compiled material closures.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_INTEGRATOR_CLOSURE_CLASSIFICATION_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_INTEGRATOR_CLOSURE_CLASSIFICATION_H

#include "pxr/pxr.h"

namespace mxcpp {
struct SurfaceClosure;
}

PXR_NAMESPACE_OPEN_SCOPE
namespace ty {

struct InstanceContext;
struct PrototypeContext;

/// Return whether an evaluated closure permits reflection transport only.
///
/// The closure must be fully evaluated. Malformed or unclassified BSDF trees
/// conservatively return false.
bool IsReflectionOnlyClosure(mxcpp::SurfaceClosure const& closure);

/// Return whether a closure is a transparent, scattering-free volume boundary.
bool IsVolumeOnlyBoundary(mxcpp::SurfaceClosure const& closure);

/// Return whether a direct-light shadow ray may use approximate straight
/// traversal through thick transmissive blockers.
/// `closure` is the evaluated NEE origin, or null for a medium or synthetic
/// diffuse origin. Coupled thick dielectric origins always return false;
/// other origins follow `settingEnabled`.
bool AllowApproximateTransparentShadowsForNeeOrigin(
    mxcpp::SurfaceClosure const* closure, bool settingEnabled);

/// Return whether a thick transmissive hit may use approximate straight
/// traversal for the current NEE segment. A coupled origin is identified by
/// both stable instance and prototype pointers; only that same object is
/// conservative. Null origin pointers impose no per-object exception.
bool AllowApproximateTransparentShadowAtHit(
    bool settingEnabled,
    InstanceContext const* conservativeOriginInstance,
    PrototypeContext const* conservativeOriginPrototype,
    InstanceContext const* hitInstance,
    PrototypeContext const* hitPrototype);

} // namespace ty
PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_IMAGING_PLUGIN_HD_EMBREE_INTEGRATOR_CLOSURE_CLASSIFICATION_H
