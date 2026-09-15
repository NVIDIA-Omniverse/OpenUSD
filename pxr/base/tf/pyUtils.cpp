//
// Copyright 2016 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//

#include "pxr/pxr.h"

#include "pxr/base/tf/pyError.h"
#include "pxr/base/tf/pyUtils.h"

PXR_NAMESPACE_OPEN_SCOPE

int64_t
TfPyNormalizeIndex(int64_t index, uint64_t size, bool throwError)
{
    if (index < 0) 
        index += size;

    if (throwError && (index < 0 || static_cast<uint64_t>(index) >= size)) {
        TfPyThrowIndexError("Index out of range.");
    }

    return index < 0 ? 0 :
                   static_cast<uint64_t>(index) >= size ? size - 1 : index;
}

PXR_NAMESPACE_CLOSE_SCOPE
