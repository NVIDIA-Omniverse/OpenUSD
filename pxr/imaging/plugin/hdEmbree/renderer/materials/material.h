//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_RENDERER_MATERIAL_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_RENDERER_MATERIAL_H

#include "pxr/pxr.h"
#include "pxr/base/tf/token.h"

#include <string>
#include <vector>

namespace mxcpp { class EvalGraph; }

PXR_NAMESPACE_OPEN_SCOPE

struct HdEmbreeMaterialData
{
    ::mxcpp::EvalGraph* surfaceGraph = nullptr;
    ::mxcpp::EvalGraph* displacementGraph = nullptr;
    /// Handle space shared by both graphs. Prototype resolved bindings are
    /// sized and indexed against this table.
    std::vector<std::string> geomPropNames;
    /// Interned form of geomPropNames used when prototypes refresh bindings.
    /// Equal in size and order to geomPropNames.
    std::vector<TfToken> geomPropTokens;
};

PXR_NAMESPACE_CLOSE_SCOPE

#endif
