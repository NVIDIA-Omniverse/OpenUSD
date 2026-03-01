//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_MXLITE_MATERIALS_STANDARD_SURFACE_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_MXLITE_MATERIALS_STANDARD_SURFACE_H

#include "pxr/pxr.h"
#include "pxr/imaging/plugin/hdEmbree/mxLite/types.h"

PXR_NAMESPACE_OPEN_SCOPE

/// Evaluate the Autodesk Standard Surface material model.
/// Maps Standard Surface parameters to a unified surface closure.
MxLiteSurfaceClosure MxLiteEvalStandardSurface(const MxLiteParamMap& params);

PXR_NAMESPACE_CLOSE_SCOPE

#endif
