//
// Copyright 2017 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_MESH_SAMPLERS_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_MESH_SAMPLERS_H

#include "primvarSampler.h"

#include "pxr/base/vt/types.h"
#include "pxr/imaging/hd/meshUtil.h"
#include "pxr/pxr.h"

#include <embree4/rtcore.h>
#include <embree4/rtcore_geometry.h>

#include <bitset>

PXR_NAMESPACE_OPEN_SCOPE
namespace ty {

/// \class RtcBufferAllocator
///
/// Utility class to track which embree user vertex buffers are currently
/// in use.
class RtcBufferAllocator
{
public:
    /// Constructor. By default, set everything to unallocated.
    RtcBufferAllocator()
        : _bitset(0) {}

    /// Allocate a buffer by finding the first clear bit, using that as
    /// the buffer number, and setting the bit to mark it as used.
    /// \return An unused RTC user vertex buffer id, or -1 on failure.
    int Allocate();

    /// Free a buffer by clearing its bit.
    /// \param buffer The buffer to mark as unused.
    void Free(int buffer);

    /// Query how many buffers are currently in user for this geometry
    unsigned int NumBuffers();

    /// As of Embree3 the number of buffers was greatly increased
    /// however the maximum is only defined locally to the library
    /// as of v3.4.0 this was the number.
    static constexpr int PXR_MAX_USER_VERTEX_BUFFERS = 16;

private:
    std::bitset<PXR_MAX_USER_VERTEX_BUFFERS> _bitset;
};



// ----------------------------------------------------------------------
// The classes below implement the PrimvarSampler interface for
// the different interpolation modes that hydra supports. In some cases,
// implementations are broken out by geometry type (e.g. triangles vs
// subdiv).

/// \class ConstantSampler
///
/// This class implements the PrimvarSampler interface for primvars
/// with "constant" interpolation mode. This means that the buffer only has
/// one item, which should be returned for any (element, u, v) tuple.
class ConstantSampler : public PrimvarSampler {
public:
    /// Constructor.
    /// \param name The name of the primvar.
    /// \param value The buffer data for the primvar.
    ConstantSampler(TfToken const& name,
                            VtValue const& value)
        : _buffer(name, value)
        , _sampler(_buffer) {}

    /// Sample the primvar at an (element, u, v) location. For constant
    /// primvars, the buffer only contains one item, so we always return
    /// that item.
    /// \param element The element index to sample.
    /// \param u The u coordinate to sample.
    /// \param v The v coordinate to sample.
    /// \param value The memory to write the value to (only written on success).
    /// \param dataType The HdTupleType describing element values.
    /// \return True if the value was successfully sampled.
    virtual bool Sample(unsigned int element, float u, float v, void* value,
                        HdTupleType dataType) const;

private:
    HdVtBufferSource const _buffer;
    BufferSampler const _sampler;
};

/// \class UniformSampler
///
/// This class implements the PrimvarSampler interface for primvars
/// with "uniform" interpolation mode. This means that the buffer has one
/// item per authored face. For unrefined meshes, HdEmbree will convert
/// mesh polygons to triangles, so this class optionally takes an array
/// called "primitiveParams" which maps from the face index embree reports
/// to the original authored face in the scene data. If primitiveParams is not
/// provided, this translation step is skipped.
class UniformSampler : public PrimvarSampler {
public:
    /// Constructor.
    /// \param name The name of the primvar.
    /// \param value The buffer data for the primvar.
    /// \param primitiveParams A mapping from geometry face index to authored
    ///                        face index.
    UniformSampler(TfToken const& name,
                           VtValue const& value,
                           VtIntArray const& primitiveParams)
        : _buffer(name, value)
        , _sampler(_buffer)
        , _primitiveParams(primitiveParams) {}

    /// Constructor.
    /// \param name The name of the primvar.
    /// \param value The buffer data for the primvar.
    UniformSampler(TfToken const& name,
                           VtValue const& value)
        : _buffer(name, value)
        , _sampler(_buffer) {}

    /// Sample the primvar at an (element, u, v) location. For uniform
    /// primvars, optionally look up the authored face index in
    /// _primitiveParams[element] (which is stored encoded); then return
    /// _buffer[element].
    ///
    /// \param element The element index to sample.
    /// \param u The u coordinate to sample.
    /// \param v The v coordinate to sample.
    /// \param value The memory to write the value to (only written on success).
    /// \param dataType The HdTupleType describing element values.
    /// \return True if the value was successfully sampled.
    virtual bool Sample(unsigned int element, float u, float v, void* value,
                        HdTupleType dataType) const;

private:
    HdVtBufferSource const _buffer;
    BufferSampler const _sampler;
    VtIntArray const _primitiveParams;
};

/// \class TriangleVertexSampler
///
/// This class implements the PrimvarSampler interface for primvars on
/// triangle meshes with "vertex" or "varying" interpolation modes. This means
/// the buffer has one item per vertex, and the result of sampling is a
/// barycentric interpolation of the hit face vertices. This class
/// requires the triangulated mesh topology, to map from the triangle index
/// (in "element") to the triangle vertices.
class TriangleVertexSampler : public PrimvarSampler {
public:
    /// Constructor.
    /// \param name The name of the primvar.
    /// \param value The buffer data for the primvar.
    /// \param indices A map from triangle index to vertex indices in the
    ///                triangulated geometry.
    TriangleVertexSampler(TfToken const& name,
                                  VtValue const& value,
                                  VtVec3iArray const& indices)
        : _buffer(name, value)
        , _sampler(_buffer)
        , _indices(indices) {}

    /// Sample the primvar at an (element, u, v) location. For vertex primvars,
    /// the vertex indices of the triangle are stored in _indices[element][0-2].
    /// After fetching the primvar value for each of the three vertices,
    /// they are interpolated as follows, per Embree specification:
    /// t_uv = (1-u-v)*t0 + u*t1 + v*t2
    /// 
    /// \param element The element index to sample.
    /// \param u The u coordinate to sample.
    /// \param v The v coordinate to sample.
    /// \param value The memory to write the value to (only written on success).
    /// \param dataType The HdTupleType describing element values.
    /// \return True if the value was successfully sampled.
    virtual bool Sample(unsigned int element, float u, float v, void* value,
                        HdTupleType dataType) const;

    /// Retrieve the three raw (un-interpolated) vertex values for a triangle.
    bool SampleVertices(unsigned int element,
                        void* v0, void* v1, void* v2,
                        HdTupleType dataType) const;

    /// Templated convenience overload (auto-deduces HdTupleType).
    template<typename T>
    bool SampleVertices(unsigned int element, T* v0, T* v1, T* v2) const {
        return SampleVertices(element,
            static_cast<void*>(v0), static_cast<void*>(v1),
            static_cast<void*>(v2),
            TypeHelper::GetTupleType<T>());
    }

private:
    HdVtBufferSource const _buffer;
    BufferSampler const _sampler;
    VtVec3iArray const _indices;
};

/// \class TriangleFaceVaryingSampler
///
/// This class implements the PrimvarSampler interface for primvars on
/// triangle meshes with "face-varying" interpolation modes. This means that
/// each vertex of each face gets its own buffer item: vertex 0 as part of
/// face 0 might have value 1.0f, but vertex 0 as part of face 1 might have
/// value 2.0f. The primvar's memory layout is grouped by face, with one item
/// per vertex.
///
/// Concretely, a cube with 8 vertices would have 24 items
/// (6 faces * 4 vertices) in a face-varying primvar, and the index of the
/// item for face 2, vertex 3, would be (2 * 4 + 3) = 11.
///
/// Face-varying primvars are provided to the sampler un-triangulated, but
/// the size of the buffer is tied to the size of the topology, so
/// this class triangulates the input buffer before sampling.
class TriangleFaceVaryingSampler : public PrimvarSampler {
public:
    /// Constructor. Triangulates the provided buffer data.
    /// \param name The name of the primvar.
    /// \param value The buffer data for the primvar.
    /// \param meshUtil An HdMeshUtil instance that knows how to triangulate
    ///                 the input buffer data.
    TriangleFaceVaryingSampler(TfToken const& name,
                                       VtValue const& value,
                                       HdMeshUtil &meshUtil)
        : _buffer(name, _Triangulate(name, value, meshUtil))
        , _sampler(_buffer) {}

    /// Sample the primvar at an (element, u, v) location. For face varying
    /// primvars, the vertex indices are simply (element * 3 + 0->2), since
    /// all faces are triangles. After fetching the primvar value for each of
    /// the three vertices, they are interpolated as follows, per Embree
    /// specification:
    /// t_uv = (1-u-v)*t0 + u*t1 + v*t2
    /// 
    /// \param element The element index to sample.
    /// \param u The u coordinate to sample.
    /// \param v The v coordinate to sample.
    /// \param value The memory to write the value to (only written on success).
    /// \param dataType The HdTupleType describing element values.
    /// \return True if the value was successfully sampled.
    virtual bool Sample(unsigned int element, float u, float v, void* value,
                        HdTupleType dataType) const;

    /// Retrieve the three raw (un-interpolated) vertex values for a triangle.
    bool SampleVertices(unsigned int element,
                        void* v0, void* v1, void* v2,
                        HdTupleType dataType) const;

    /// Templated convenience overload.
    template<typename T>
    bool SampleVertices(unsigned int element, T* v0, T* v1, T* v2) const {
        return SampleVertices(element,
            static_cast<void*>(v0), static_cast<void*>(v1),
            static_cast<void*>(v2),
            TypeHelper::GetTupleType<T>());
    }

private:
    HdVtBufferSource const _buffer;
    BufferSampler const _sampler;

    // Pass the "value" parameter through HdMeshUtils'
    // ComputeTriangulatedFaceVaryingPrimvar(), which adjusts the primvar
    // buffer data for the triangulated topology. HdMeshUtil is provided
    // the source topology at construction time, so this class doesn't need
    // to provide it.
    static VtValue _Triangulate(TfToken const& name, VtValue const& value,
                                HdMeshUtil &meshUtil);
};

/// Shared storage and sampling for Embree subdivision attributes. Keeping the
/// retained geometry here matters because displacement callbacks may sample
/// attributes while scene mutation makes scene/id lookups unsafe.
class SubdivSampler : public PrimvarSampler {
public:
    using PrimvarSampler::Sample;

    virtual ~SubdivSampler();

    bool Sample(unsigned int element, float u, float v, void* value,
                HdTupleType dataType) const override;

    /// Derivatives share the padded sampling path with values so Embree's
    /// SIMD-width writes cannot overwrite compact GfVec2f/GfVec3f objects.
    bool SampleWithDerivatives(unsigned int element, float u, float v,
                               void* value, void* dPdu, void* dPdv,
                               HdTupleType dataType) const;

    template<typename T>
    bool SampleWithDerivatives(unsigned int element, float u, float v,
                               T* value, T* dPdu, T* dPdv) const {
        return SampleWithDerivatives(element, u, v,
            static_cast<void*>(value), static_cast<void*>(dPdu),
            static_cast<void*>(dPdv),
            TypeHelper::GetTupleType<T>());
    }

protected:
    SubdivSampler(TfToken const& name,
                          VtValue const& value,
                          RTCGeometry geometry,
                          RtcBufferAllocator* allocator,
                          char const* interpolation,
                          int topologyId = -1);

private:
    int _embreeBufferId;
    HdVtBufferSource const _buffer;
    RTCGeometry _geometry;
    RtcBufferAllocator* _allocator;
};

/// Smooth subdivision-basis interpolation for one value per cage vertex.
class SubdivVertexSampler : public SubdivSampler {
public:
    SubdivVertexSampler(TfToken const& name,
                                VtValue const& value,
                                RTCGeometry geometry,
                                RtcBufferAllocator* allocator);
};

/// Piecewise-linear subdivision interpolation for one value per cage vertex.
/// Topology 1 is a copy of the mesh topology configured with PIN_ALL because
/// varying data must not inherit the smooth position basis.
class SubdivVaryingSampler : public SubdivSampler {
public:
    SubdivVaryingSampler(TfToken const& name,
                                 VtValue const& value,
                                 RTCGeometry geometry,
                                 RtcBufferAllocator* allocator);
};

/// Face-varying interpolation on a primvar-specific topology. Independent
/// topologies preserve seams because different primvars need not share indices.
class SubdivFaceVaryingSampler : public SubdivSampler {
public:
    SubdivFaceVaryingSampler(TfToken const& name,
                                     VtValue const& value,
                                     RTCGeometry geometry,
                                     unsigned int topologyId,
                                     RtcBufferAllocator* allocator);
};

} // namespace ty
PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_IMAGING_PLUGIN_HD_EMBREE_MESH_SAMPLERS_H
