//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
// Embree ray initialization, hit-position, and origin-offset helpers.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_RAY_UTIL_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_RAY_UTIL_H

#include "renderer.h"

#include <embree4/rtcore_ray.h>

#include <limits>

PXR_NAMESPACE_OPEN_SCOPE
namespace ty {

inline GfVec3f
CalculateHitPosition(RTCRayHit const& rayHit)
{
    return GfVec3f(rayHit.ray.org_x + rayHit.ray.tfar * rayHit.ray.dir_x,
                   rayHit.ray.org_y + rayHit.ray.tfar * rayHit.ray.dir_y,
                   rayHit.ray.org_z + rayHit.ray.tfar * rayHit.ray.dir_z);
}

/// Fill in an RTCRay structure from the given parameters.
inline void
PopulateRay(RTCRay* ray, GfVec3f const& origin, GfVec3f const& directionLocal,
             float nearest,
             float furthest = std::numeric_limits<float>::infinity(),
             RayMask mask = RayMask::All)
{
    ray->org_x = origin[0];
    ray->org_y = origin[1];
    ray->org_z = origin[2];
    ray->tnear = nearest;

    ray->dir_x = directionLocal[0];
    ray->dir_y = directionLocal[1];
    ray->dir_z = directionLocal[2];
    ray->time = 0.0f;

    ray->tfar = furthest;
    ray->mask = static_cast<uint32_t>(mask);
    ray->id = 0;
    ray->flags = 0;
}

/// \brief Bias a ray origin off a surface to avoid self-intersection.
///
/// \param positionWld World-space point to offset.
/// \param directionOffsetReferenceWld Axis to push along, faced toward
/// \p directionRayWld. Surface events supply their geometric normal. A
/// zero-length value is a supported input meaning "no surface frame is
/// known"; callers rely on it, so keep that branch.
/// \param directionRayWld Normalized direction the offset ray travels.
/// \param bias Positive offset distance.
/// \return The biased origin; \p positionWld advanced along
/// \p directionRayWld when no reference axis is available.
inline GfVec3f
OffsetRayOrigin(
    GfVec3f const& positionWld,
    GfVec3f const& directionOffsetReferenceWld,
    GfVec3f const& directionRayWld,
    float bias = 1.0e-4f)
{
    if (directionOffsetReferenceWld.GetLengthSq() < 1e-18f) {
        return positionWld + directionRayWld * bias;
    }

    GfVec3f offsetNormal = directionOffsetReferenceWld.GetNormalized();
    if (GfDot(offsetNormal, directionRayWld) < 0.0f) {
        offsetNormal = -offsetNormal;
    }
    return positionWld + offsetNormal * bias;
}

/// Fill in an RTCRayHit structure from the given parameters.
// note this containts a Ray and a RayHit
inline void
PopulateRayHit(RTCRayHit* rayHit, GfVec3f const& origin,
                GfVec3f const& directionLocal, float nearest,
                float furthest = std::numeric_limits<float>::infinity(),
                RayMask mask = RayMask::All)
{
    // Fill in defaults for the ray
    PopulateRay(&rayHit->ray, origin, directionLocal, nearest, furthest, mask);

    // Fill in defaults for the hit
    rayHit->hit.primID = RTC_INVALID_GEOMETRY_ID;
    rayHit->hit.geomID = RTC_INVALID_GEOMETRY_ID;
    for (unsigned int i = 0; i < RTC_MAX_INSTANCE_LEVEL_COUNT; ++i) {
        rayHit->hit.instID[i] = RTC_INVALID_GEOMETRY_ID;
    }
}

} // namespace ty
PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_IMAGING_PLUGIN_HD_EMBREE_RAY_UTIL_H
