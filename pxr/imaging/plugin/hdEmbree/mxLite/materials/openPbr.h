//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_MXLITE_MATERIALS_OPEN_PBR_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_MXLITE_MATERIALS_OPEN_PBR_H

#include "pxr/pxr.h"
#include "pxr/imaging/plugin/hdEmbree/mxLite/types.h"

PXR_NAMESPACE_OPEN_SCOPE

/// Evaluate the OpenPBR Surface material model.
/// Maps OpenPBR parameters to a unified surface closure.
MxLiteSurfaceClosure MxLiteEvalOpenPbr(const MxLiteParamMap& params);

PXR_NAMESPACE_CLOSE_SCOPE

#endif
