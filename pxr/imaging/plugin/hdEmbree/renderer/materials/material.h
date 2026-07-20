//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_RENDERER_MATERIAL_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_RENDERER_MATERIAL_H

#include "pxr/pxr.h"

namespace mxcpp { class EvalGraph; }

PXR_NAMESPACE_OPEN_SCOPE

struct HdEmbreeMaterialData
{
    ::mxcpp::EvalGraph* evalGraph = nullptr;
    ::mxcpp::EvalGraph* displacementGraph = nullptr;
};

PXR_NAMESPACE_CLOSE_SCOPE

#endif
