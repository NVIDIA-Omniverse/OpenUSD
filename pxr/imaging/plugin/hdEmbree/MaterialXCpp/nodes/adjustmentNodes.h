//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_MXCPP_NODES_ADJUSTMENT_NODES_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_MXCPP_NODES_ADJUSTMENT_NODES_H

#include "pxr/pxr.h"

PXR_NAMESPACE_OPEN_SCOPE
namespace mxcpp {
class NodeRegistry;
void RegisterAdjustmentNodes(NodeRegistry& reg);
} // namespace mxcpp
PXR_NAMESPACE_CLOSE_SCOPE

#endif
