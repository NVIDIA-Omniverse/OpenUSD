//
// Copyright 2016 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_BASE_VT_ARRAY_PY_BUFFER_H
#define PXR_BASE_VT_ARRAY_PY_BUFFER_H

#include "pxr/pxr.h"
#include "pxr/base/vt/api.h"
#include "pxr/base/vt/array.h"

PXR_NAMESPACE_OPEN_SCOPE

// Installs the producer slots and conversion callbacks in the _vt module.
VT_LOCAL void Vt_AddBufferProtocolSupportToVtArrays();

// Private set of builtin array buffer types, used only by _vt.
#define VT_ARRAY_PYBUFFER_TYPES                 \
    VT_BUILTIN_NUMERIC_VALUE_TYPES              \
    VT_VEC_VALUE_TYPES                          \
    VT_MATRIX_VALUE_TYPES                       \
    VT_GFRANGE_VALUE_TYPES                      \
    ((GfRect2i, Rect2i))                        \
    ((GfQuath, Quath))                          \
    ((GfQuatf, Quatf))                          \
    ((GfQuatd, Quatd))                          \
    ((GfDualQuath, DualQuath))                  \
    ((GfDualQuatf, DualQuatf))                  \
    ((GfDualQuatd, DualQuatd))


PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_BASE_VT_ARRAY_PY_BUFFER_H
