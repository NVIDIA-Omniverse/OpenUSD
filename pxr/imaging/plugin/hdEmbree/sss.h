//
// hdEmbree random-walk SSS helpers.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_SSS_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_SSS_H

#include "pxr/pxr.h"

#include "pxr/base/gf/vec3f.h"
#include "pxr/imaging/plugin/hdEmbree/MaterialXCpp/surfaceClosure.h"

PXR_NAMESPACE_OPEN_SCOPE

struct HdEmbreeSubsurfaceEntry
{
    GfVec3f origin = GfVec3f(0.0f);
    GfVec3f direction = GfVec3f(0.0f, 0.0f, -1.0f);
};

HdEmbreeSubsurfaceEntry
HdEmbreeSampleSubsurfaceEntry(
    GfVec3f const& position,
    GfVec3f const& outwardNormal,
    float u1,
    float u2);

mxcpp::SurfaceClosure
HdEmbreeMakeSubsurfaceExitClosure(
    mxcpp::SurfaceClosure const& surfaceClosure);

PXR_NAMESPACE_CLOSE_SCOPE

#endif  // PXR_IMAGING_PLUGIN_HD_EMBREE_SSS_H
