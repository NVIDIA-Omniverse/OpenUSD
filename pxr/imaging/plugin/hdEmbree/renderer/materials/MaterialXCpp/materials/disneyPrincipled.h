//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef MXCPP_MATERIALS_DISNEY_PRINCIPLED_H
#define MXCPP_MATERIALS_DISNEY_PRINCIPLED_H

#include "../paramMap.h"
#include "../surfaceClosure.h"

namespace mxcpp {

/// Evaluate the MaterialX Disney Principled surface model.
SurfaceClosure EvalDisneyPrincipled(const ParamMap& params);

}  // namespace mxcpp

#endif
