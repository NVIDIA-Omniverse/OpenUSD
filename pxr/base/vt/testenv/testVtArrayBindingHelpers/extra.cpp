//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/base/vt/boost/wrapArray.h"
#include "pxr/base/vt/boost/wrapArrayEdit.h"
PXR_NAMESPACE_USING_DIRECTIVE

std::string BuiltinNameFromOtherTU() {
    return Vt_WrapArray::GetVtArrayName<VtIntArray>();
}
unsigned int RankFromOtherTU(size_t size, size_t dimension, size_t *last) {
    Vt_ShapeData shape{};
    shape.totalSize = size;
    shape.otherDims[0] = static_cast<unsigned int>(dimension);
    return Vt_WrapArray::Vt_ComputeEffectiveRankAndLastDimSize(&shape, last);
}
