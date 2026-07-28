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

/// Return whether an evaluated closure permits reflection transport only.
///
/// The closure must be fully evaluated. Malformed or unclassified BSDF trees
/// conservatively return false.
bool IsReflectionOnlyClosure(mxcpp::SurfaceClosure const& closure);

/// Return whether a closure is a transparent, scattering-free volume boundary.
bool IsVolumeOnlyBoundary(mxcpp::SurfaceClosure const& closure);

} // namespace ty
PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_IMAGING_PLUGIN_HD_EMBREE_INTEGRATOR_CLOSURE_CLASSIFICATION_H
