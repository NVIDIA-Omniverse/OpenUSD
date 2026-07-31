//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_IMAGING_PLUGIN_HDEMBREE_MATERIALXCPP_BSDF_CLOSURETRAVERSAL_H
#define PXR_IMAGING_PLUGIN_HDEMBREE_MATERIALXCPP_BSDF_CLOSURETRAVERSAL_H

#include <renderer/materials/MaterialXCpp/materials/bsdf.h>

#include <cstddef>

namespace mxcpp {
namespace Bsdf {
namespace detail {

/// Returns a copy of `tree` with caustic-class leaves removed and surviving
/// node references remapped. `tree` must contain only valid child references.
/// An entirely removed tree is returned with an invalid root; allocation
/// failure may propagate as `std::bad_alloc`.
Bsdf::ClosureTree PruneCausticClassLobes(const Bsdf::ClosureTree& tree);

/// Stores the diffuse/specular defaults once for later Eval/Sample/PDF
/// traversals and prepares only leaves recorded with authored normals.
/// Authored normals are normalized and accepted only in the hemisphere of the
/// final finite unit exterior material `normalShdWldExt`; invalid values fall
/// back without negation. This deliberately validates the hierarchy in order:
/// the graph normal against the exterior smooth frame, then each lobe normal
/// against the resolved exterior graph normal. The complete normals are faced
/// to the incident side selected by `frontFacing`. Reflective normals are then
/// raised toward finite unit incident-side `normalGeomWldOut` when needed to
/// keep mirror reflection of finite unit `omegaOutWld` above the geometric
/// surface. `tree` must be non-null. Returns the number of invalid authored
/// values replaced. Tree construction must keep the authored-normal linked
/// index and `hasDefaultSpecularNormalNodes` synchronized with `nodes`.
/// Allocation-free and does not throw.
std::size_t PrepareShadingNormals(
    Bsdf::ClosureTree* tree,
    const Vec3f& normalShdWldExt,
    const Vec3f& normalGeomWldOut,
    const Vec3f& omegaOutWld,
    bool frontFacing = true);

/// Evaluates the closure subtree rooted at `nodeId`.
/// `nodeId` may be invalid, in which case zero is returned. `normalShdWldOut`,
/// `omegaInWld`, and `omegaOutWld` must be finite unit vectors; directions
/// point away from the surface. `heroWavelengthNm` must be finite and is zero
/// when spectral dispersion is disabled. `frontFacing` is the immutable
/// geometric interface side and must not be inferred from a shading normal.
/// Returns a finite non-negative RGB BSDF value; malformed or degenerate nodes
/// contribute zero. Does not throw.
Vec3f EvalNode(
    const Bsdf::ClosureTree& tree, Bsdf::NodeId nodeId,
    const Vec3f& normalShdWldOut, const Vec3f& omegaInWld,
    const Vec3f& omegaOutWld, float heroWavelengthNm,
    bool frontFacing = true);

/// Evaluates the solid-angle PDF of the closure subtree rooted at `nodeId`.
/// `nodeId` may be invalid, in which case zero is returned. The normal and
/// directions must be finite unit vectors pointing away from the surface;
/// the normal selects the incident transport side and need not face
/// `omegaOutWld` at grazing.
/// `heroWavelengthNm` must be finite and is zero when dispersion is disabled.
/// `frontFacing` has `EvalNode`'s geometric-side meaning. Returns a finite
/// non-negative density; degenerate nodes return zero. Does not throw.
float PdfNode(
    const Bsdf::ClosureTree& tree, Bsdf::NodeId nodeId,
    const Vec3f& normalShdWldOut, const Vec3f& omegaInWld,
    const Vec3f& omegaOutWld, float heroWavelengthNm,
    bool frontFacing = true);

/// Samples the closure subtree rooted at `nodeId`.
/// `nodeId` may be invalid. `normalShdWldOut` and `omegaOutWld` must be finite
/// unit directions pointing away from the surface; the normal selects the
/// incident transport side. `u1`, `u2`, and `uChoice` must
/// be finite values in [0,1). `heroWavelengthNm` must be finite and is zero
/// when dispersion is disabled. `frontFacing` has `EvalNode`'s geometric-side
/// meaning. Returns a sample whose direction is unit length when valid.
/// Failure or a degenerate subtree returns
/// `pdfSolidAngle == 0`; callers must test the PDF before division. Does not
/// throw.
Bsdf::BsdfSample SampleNode(
    const Bsdf::ClosureTree& tree, Bsdf::NodeId nodeId,
    const Vec3f& normalShdWldOut, const Vec3f& omegaOutWld, float u1,
    float u2, float uChoice, float heroWavelengthNm,
    bool frontFacing = true);

}  // namespace detail
}  // namespace Bsdf
}  // namespace mxcpp

#endif  // PXR_IMAGING_PLUGIN_HDEMBREE_MATERIALXCPP_BSDF_CLOSURETRAVERSAL_H
