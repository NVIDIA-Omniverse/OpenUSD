//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef MXCPP_MATERIALS_OPEN_PBR_H
#define MXCPP_MATERIALS_OPEN_PBR_H

#include "../paramMap.h"
#include "../surfaceClosure.h"

namespace mxcpp {

/// Evaluate the OpenPBR Surface material model.
/// Maps OpenPBR parameters to a unified surface closure.
SurfaceClosure EvalOpenPbr(const ParamMap& params);

}  // namespace mxcpp

#endif
