//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
// Normal and normalized-normal-derivative instance transforms.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_GEOMETRY_NORMAL_TRANSFORMS_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_GEOMETRY_NORMAL_TRANSFORMS_H

#include "context.h"

#include <renderer/rendererMath.h>

#include "pxr/base/gf/matrix4f.h"
#include "pxr/base/gf/vec3f.h"

#include <cmath>

PXR_NAMESPACE_OPEN_SCOPE
namespace ty {

/// Transform a semantic object-space normal to world space.
///
/// Normals are covectors, so non-uniform transforms require the inverse
/// transpose rather than the object-to-world linear transform. Reflections do
/// not negate a semantic USD normal; Hydra accounts for winding reversal
/// separately when it classifies faces.
inline GfVec3f
TransformNormalToWorld(
    GfMatrix4f const& worldToObjectMatrix,
    GfVec3f const& objectNormal)
{
    GfVec3f worldNormal =
        worldToObjectMatrix.GetTranspose().TransformDir(objectNormal);

    GfVec3f normalizedWorldNormal;
    if (!TryNormalizeDirection(worldNormal, &normalizedWorldNormal)) {
        // A singular transform has no mathematically unique world normal.
        // Preserve a valid authored/geometric direction rather than replacing
        // it with an unrelated axis.
        if (TryNormalizeDirection(objectNormal, &normalizedWorldNormal)) {
            return normalizedWorldNormal;
        }
        return GfVec3f(0.0f, 0.0f, 1.0f);
    }
    return normalizedWorldNormal;
}

inline GfVec3f
TransformNormalToWorld(
    HdEmbreeInstanceContext const* instanceContext,
    GfVec3f const& objectNormal)
{
    if (!instanceContext) {
        return objectNormal;
    }
    return TransformNormalToWorld(
        instanceContext->worldToObjectMatrix,
        objectNormal);
}

/// Recover the oriented object-space normal corresponding to a world-space
/// normal. This is the inverse direction of TransformNormalToWorld and keeps
/// a face-forward sign applied by the caller.
inline GfVec3f
TransformNormalToObject(
    GfMatrix4f const& objectToWorldMatrix,
    GfVec3f const& worldNormal)
{
    GfVec3f objectNormal =
        objectToWorldMatrix.GetTranspose().TransformDir(worldNormal);

    GfVec3f normalizedObjectNormal;
    if (!TryNormalizeDirection(objectNormal, &normalizedObjectNormal)) {
        if (TryNormalizeDirection(worldNormal, &normalizedObjectNormal)) {
            return normalizedObjectNormal;
        }
        return GfVec3f(0.0f, 0.0f, 1.0f);
    }
    return normalizedObjectNormal;
}

inline GfVec3f
TransformNormalToObject(
    HdEmbreeInstanceContext const* instanceContext,
    GfVec3f const& worldNormal)
{
    if (!instanceContext) {
        return worldNormal;
    }
    return TransformNormalToObject(
        instanceContext->objectToWorldMatrix, worldNormal);
}

/// Transform the derivative of a normalized normal through an instance.
/// Applying the inverse transpose to dN alone is insufficient under
/// non-uniform scale: the result must also include the derivative of the
/// normalization operation.
inline GfVec3f
TransformNormalDerivativeToWorld(
    HdEmbreeInstanceContext const* instanceContext,
    GfVec3f const& objectNormal,
    GfVec3f const& objectNormalDerivative)
{
    if (!instanceContext) {
        return objectNormalDerivative;
    }

    const GfMatrix4f normalTransform =
        instanceContext->worldToObjectMatrix.GetTranspose();
    GfVec3f transformedNormal =
        normalTransform.TransformDir(objectNormal);
    GfVec3f transformedDerivative =
        normalTransform.TransformDir(objectNormalDerivative);

    GfVec3f worldNormal;
    double normalLength = 0.0;
    if (!IsFinite(transformedDerivative) ||
        !TryNormalizeDirection(
            transformedNormal, &worldNormal, &normalLength) ||
        !std::isfinite(normalLength) || normalLength == 0.0) {
        return GfVec3f(0.0f);
    }

    const double projection =
        static_cast<double>(worldNormal[0]) * transformedDerivative[0] +
        static_cast<double>(worldNormal[1]) * transformedDerivative[1] +
        static_cast<double>(worldNormal[2]) * transformedDerivative[2];
    const GfVec3f worldDerivative(
        static_cast<float>(
            (transformedDerivative[0] - worldNormal[0] * projection) /
            normalLength),
        static_cast<float>(
            (transformedDerivative[1] - worldNormal[1] * projection) /
            normalLength),
        static_cast<float>(
            (transformedDerivative[2] - worldNormal[2] * projection) /
            normalLength));
    return IsFinite(worldDerivative) ? worldDerivative : GfVec3f(0.0f);
}

} // namespace ty
PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_IMAGING_PLUGIN_HD_EMBREE_GEOMETRY_NORMAL_TRANSFORMS_H
