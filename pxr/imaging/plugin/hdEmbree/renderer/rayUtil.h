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
#include "embreeCompat.h"

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

/// Fill an RTCRay using an origin and direction in the same caller-selected
/// coordinate space.
inline void
PopulateRay(RTCRay* ray, GfVec3f const& posRayOrg, GfVec3f const& dirRay,
             float nearest,
             float furthest = std::numeric_limits<float>::infinity(),
             RayMask mask = RayMask::All)
{
    ray->org_x = posRayOrg[0];
    ray->org_y = posRayOrg[1];
    ray->org_z = posRayOrg[2];
    ray->tnear = nearest;

    ray->dir_x = dirRay[0];
    ray->dir_y = dirRay[1];
    ray->dir_z = dirRay[2];
    ray->time = 0.0f;

    ray->tfar = furthest;
    ray->mask = static_cast<uint32_t>(mask);
    ray->id = 0;
    ray->flags = 0;
}

/// \brief Bias a ray origin off a surface to avoid self-intersection.
///
/// \param posWld World-space point to offset.
/// \param dirOffsetReferenceWld Axis to push along, faced toward
/// \p dirRayWld. Surface events supply their geometric normal. A
/// zero-length value is a supported input meaning "no surface frame is
/// known"; callers rely on it, so keep that branch.
/// \param dirRayWld Normalized direction the offset ray travels.
/// \param bias Positive offset distance.
/// \return The biased origin; \p posWld advanced along
/// \p dirRayWld when no reference axis is available.
inline GfVec3f
OffsetRayOrigin(
    GfVec3f const& posWld,
    GfVec3f const& dirOffsetReferenceWld,
    GfVec3f const& dirRayWld,
    float bias = 1.0e-4f)
{
    if (dirOffsetReferenceWld.GetLengthSq() < 1e-18f) {
        return posWld + dirRayWld * bias;
    }

    GfVec3f offsetNormal = dirOffsetReferenceWld.GetNormalized();
    if (GfDot(offsetNormal, dirRayWld) < 0.0f) {
        offsetNormal = -offsetNormal;
    }
    return posWld + offsetNormal * bias;
}

/// Fill an RTCRayHit using an origin and direction in the same caller-selected
/// coordinate space.
inline void
PopulateRayHit(RTCRayHit* rayHit, GfVec3f const& posRayOrg,
                GfVec3f const& dirRay, float nearest,
                float furthest = std::numeric_limits<float>::infinity(),
                RayMask mask = RayMask::All)
{
    // Fill in defaults for the ray
    PopulateRay(&rayHit->ray, posRayOrg, dirRay, nearest, furthest, mask);

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
