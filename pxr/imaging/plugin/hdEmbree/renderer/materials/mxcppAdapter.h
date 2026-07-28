//
// Adapter: converts pxr types (HdMaterialNetwork2, GfVec, TfToken, VtValue)
// into pxr-independent mxcpp types (MaterialGraph, Vec3f, string, Value).
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_MXCPP_ADAPTER_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_MXCPP_ADAPTER_H

#include "pxr/pxr.h"
#include "pxr/imaging/hd/material.h"
#include "pxr/imaging/plugin/hdEmbree/renderer/colorManagement.h"
#include "pxr/imaging/plugin/hdEmbree/renderer/materials/MaterialXCpp/graphTypes.h"

PXR_NAMESPACE_OPEN_SCOPE

/// Convert an HdMaterialNetwork2 to a pxr-independent mxcpp::MaterialGraph.
mxcpp::MaterialGraph
ConvertHdNetworkToMxcppGraph(
    const HdMaterialNetwork2& network,
    HdEmbreeRenderColorSpace renderColorSpace =
        HdEmbreeRenderColorSpace::LinearRec709);

PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_IMAGING_PLUGIN_HD_EMBREE_MXCPP_ADAPTER_H
