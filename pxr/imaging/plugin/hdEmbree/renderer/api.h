//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_RENDERER_API_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_RENDERER_API_H

#include "pxr/base/arch/export.h"

#if defined(PXR_STATIC)
#   define HDEMBREE_API
#else
#   if defined(HDEMBREE_EXPORTS)
#       define HDEMBREE_API ARCH_EXPORT
#   else
#       define HDEMBREE_API ARCH_IMPORT
#   endif
#endif

#endif // PXR_IMAGING_PLUGIN_HD_EMBREE_RENDERER_API_H
