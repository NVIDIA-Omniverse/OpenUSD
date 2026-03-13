//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_MXCPP_MATERIALS_OPEN_PBR_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_MXCPP_MATERIALS_OPEN_PBR_H

#include "pxr/pxr.h"
#include "pxr/imaging/plugin/hdEmbree/MaterialXCpp/types.h"

PXR_NAMESPACE_OPEN_SCOPE
namespace mxcpp {

/// Evaluate the OpenPBR Surface material model.
/// Maps OpenPBR parameters to a unified surface closure.
SurfaceClosure EvalOpenPbr(const ParamMap& params);

} // namespace mxcpp
PXR_NAMESPACE_CLOSE_SCOPE

#endif
