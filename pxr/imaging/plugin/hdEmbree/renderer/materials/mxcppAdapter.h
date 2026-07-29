//
// Adapter: converts pxr types (HdMaterialNetwork2, GfVec, TfToken, VtValue)
// into pxr-independent mxcpp types (MaterialGraph, Vec3f, string, Value).
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_MXCPP_ADAPTER_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_MXCPP_ADAPTER_H

#include <renderer/colorManagement.h>
#include <renderer/materials/MaterialXCpp/graphTypes.h>

#include "pxr/imaging/hd/material.h"
#include "pxr/pxr.h"

PXR_NAMESPACE_OPEN_SCOPE
namespace ty {

/// Convert an HdMaterialNetwork2 to a pxr-independent mxcpp::MaterialGraph.
mxcpp::MaterialGraph
ConvertHdNetworkToMxcppGraph(
    const HdMaterialNetwork2& network,
    RenderColorSpace renderColorSpace =
        RenderColorSpace::LinearRec709);

} // namespace ty
PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_IMAGING_PLUGIN_HD_EMBREE_MXCPP_ADAPTER_H
