//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_RENDERER_RENDER_BUFFER_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_RENDERER_RENDER_BUFFER_H

#include "pxr/base/gf/vec3i.h"
#include "pxr/imaging/hd/enums.h"
#include "pxr/imaging/hd/types.h"
#include "pxr/pxr.h"

#include <cstddef>

PXR_NAMESPACE_OPEN_SCOPE

class HdEmbreeRenderBufferInterface
{
public:
    virtual ~HdEmbreeRenderBufferInterface() = default;

    virtual unsigned int GetWidth() const = 0;
    virtual unsigned int GetHeight() const = 0;
    virtual HdFormat GetFormat() const = 0;
    virtual bool IsMultiSampled() const = 0;
    virtual void* Map() = 0;
    virtual void Unmap() = 0;
    virtual void Resolve() = 0;
    virtual bool IsConverged() const = 0;
    virtual void SetConverged(bool converged) = 0;
    virtual void ClearSamples() = 0;
    virtual void BlockFill(unsigned int blockSize) = 0;
    virtual void Write(
        GfVec3i const& pixel, size_t components, float const* value) = 0;
    virtual void Write(
        GfVec3i const& pixel, size_t components, int const* value) = 0;
    virtual void WriteOutput(
        GfVec3i const& pixel, size_t components, float const* value) = 0;
    virtual void Clear(size_t components, float const* value) = 0;
    virtual void Clear(size_t components, int const* value) = 0;
};

PXR_NAMESPACE_CLOSE_SCOPE

#endif
