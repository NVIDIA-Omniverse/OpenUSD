//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_RENDERER_EMBREE_COMPAT_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_RENDERER_EMBREE_COMPAT_H

#include "pxr/pxr.h"

#include <cstddef>

#if defined(TYPHOON_HOUDINI_BUILD)
#include <embree3/rtcore.h>
#elif __has_include(<embree4/rtcore.h>)
#include <embree4/rtcore.h>
#elif __has_include(<embree3/rtcore.h>)
#include <embree3/rtcore.h>
#else
#error "hdEmbree requires Embree 3 or Embree 4 headers"
#endif

// SideFX namespaces its Embree symbols. This macro is empty in upstream
// Embree builds and imports the configured API namespace in Houdini builds.
RTC_NAMESPACE_USE

PXR_NAMESPACE_OPEN_SCOPE

namespace ty {

/// Bind external immutable storage to an Embree geometry. Keeping this call
/// behind the compatibility layer lets record code share one buffer contract
/// between the Embree 3 and 4 builds.
inline void
SetSharedGeometryBuffer(
    RTCGeometry geometry,
    RTCBufferType type,
    unsigned int slot,
    RTCFormat format,
    void const* data,
    size_t byteStride,
    size_t itemCount)
{
    rtcSetSharedGeometryBuffer(
        geometry, type, slot, format, data, 0, byteStride, itemCount);
}

/// Install one decision function for camera/intersection and
/// shadow/occlusion traversal.
inline void
SetGeometryFilterFunctions(
    RTCGeometry geometry,
    RTCFilterFunctionN filter)
{
    rtcSetGeometryIntersectFilterFunction(geometry, filter);
    rtcSetGeometryOccludedFilterFunction(geometry, filter);
}

inline void
Intersect1(RTCScene scene, RTCRayHit* rayHit)
{
#if RTC_VERSION_MAJOR == 4
    rtcIntersect1(scene, rayHit);
#elif RTC_VERSION_MAJOR == 3
    RTCIntersectContext context;
    rtcInitIntersectContext(&context);
    rtcIntersect1(scene, &context, rayHit);
#else
#error "hdEmbree supports only Embree 3 and Embree 4"
#endif
}

inline void
Occluded1(RTCScene scene, RTCRay* ray)
{
#if RTC_VERSION_MAJOR == 4
    rtcOccluded1(scene, ray);
#elif RTC_VERSION_MAJOR == 3
    RTCIntersectContext context;
    rtcInitIntersectContext(&context);
    rtcOccluded1(scene, &context, ray);
#else
#error "hdEmbree supports only Embree 3 and Embree 4"
#endif
}

} // namespace ty

PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_IMAGING_PLUGIN_HD_EMBREE_RENDERER_EMBREE_COMPAT_H
