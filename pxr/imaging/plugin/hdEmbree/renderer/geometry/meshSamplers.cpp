//
// Copyright 2017 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "meshSamplers.h"

#include "pxr/imaging/hd/meshUtil.h"
#include "pxr/imaging/hd/vtBufferSource.h"

#include <cstring>

PXR_NAMESPACE_OPEN_SCOPE

// HdEmbreeRTCBufferAllocator

int
HdEmbreeRTCBufferAllocator::Allocate()
{
    for (size_t i = 0; i < _bitset.size(); ++i) {
        if (!_bitset.test(i)) {
            _bitset.set(i);
            return static_cast<int>(i);
        }
    }
    return -1;
}

void
HdEmbreeRTCBufferAllocator::Free(int bufferIndex)
{
    _bitset.reset(bufferIndex);
}


unsigned int
HdEmbreeRTCBufferAllocator::NumBuffers()
{
    // Technically this may overcount, since a buffer may have been freed
    // but we don't move back to fill the slot, however it will be filled
    // before more are allocated. Now that there are possible a "large"
    // number of buffers it might want to be handled differently in the
    // future.
    for (int i = _bitset.size() - 1; i >= 0; i--) {
        if (_bitset.test(i)) {
            return i+1;
        }
    }
    return 0;
}

// HdEmbreeConstantSampler

bool
HdEmbreeConstantSampler::Sample(unsigned int element, float u, float v,
    void* value, HdTupleType dataType) const
{
    return _sampler.Sample(0, value, dataType);
}

// HdEmbreeUniformSampler

bool
HdEmbreeUniformSampler::Sample(unsigned int element, float u, float v,
    void* value, HdTupleType dataType) const
{
    if (_primitiveParams.empty()) {
        return _sampler.Sample(element, value, dataType);
    }
    if (element >= _primitiveParams.size()) {
        return false;
    }
    return _sampler.Sample(
        HdMeshUtil::DecodeFaceIndexFromCoarseFaceParam(
            _primitiveParams[element]),
        value, dataType);
}

// HdEmbreeTriangleVertexSampler

bool
HdEmbreeTriangleVertexSampler::Sample(unsigned int element, float u, float v,
    void* value, HdTupleType dataType) const
{
    if (element >= _indices.size()) {
        return false;
    }
    HdEmbreeTypeHelper::PrimvarTypeContainer corners[3];
    if (!_sampler.Sample(_indices[element][0], &corners[0], dataType) ||
        !_sampler.Sample(_indices[element][1], &corners[1], dataType) ||
        !_sampler.Sample(_indices[element][2], &corners[2], dataType)) {
        return false;
    }
    void* samples[3] = { static_cast<void*>(&corners[0]),
                         static_cast<void*>(&corners[1]),
                         static_cast<void*>(&corners[2]) };
    // Embree specification of triangle interpolation:
    // t_uv = (1-u-v)*t0 + u*t1 + v*t2
    float weights[3] = { 1.0f - u - v, u, v };
    return _Interpolate(value, samples, weights, 3, dataType);
}

bool
HdEmbreeTriangleVertexSampler::SampleVertices(unsigned int element,
    void* v0, void* v1, void* v2, HdTupleType dataType) const
{
    if (element >= _indices.size()) {
        return false;
    }
    return _sampler.Sample(_indices[element][0], v0, dataType) &&
           _sampler.Sample(_indices[element][1], v1, dataType) &&
           _sampler.Sample(_indices[element][2], v2, dataType);
}

// HdEmbreeTriangleFaceVaryingSampler

bool
HdEmbreeTriangleFaceVaryingSampler::Sample(unsigned int element, float u,
    float v, void* value, HdTupleType dataType) const
{
    HdEmbreeTypeHelper::PrimvarTypeContainer corners[3];
    if (!_sampler.Sample(element*3 + 0, &corners[0], dataType) ||
        !_sampler.Sample(element*3 + 1, &corners[1], dataType) ||
        !_sampler.Sample(element*3 + 2, &corners[2], dataType)) {
        return false;
    }
    void* samples[3] = { static_cast<void*>(&corners[0]),
                         static_cast<void*>(&corners[1]),
                         static_cast<void*>(&corners[2]) };
    // Embree specification of triangle interpolation:
    // t_uv = (1-u-v)*t0 + u*t1 + v*t2
    float weights[3] = { 1.0f - u - v, u, v };
    return _Interpolate(value, samples, weights, 3, dataType);
}

bool
HdEmbreeTriangleFaceVaryingSampler::SampleVertices(unsigned int element,
    void* v0, void* v1, void* v2, HdTupleType dataType) const
{
    return _sampler.Sample(element * 3 + 0, v0, dataType) &&
           _sampler.Sample(element * 3 + 1, v1, dataType) &&
           _sampler.Sample(element * 3 + 2, v2, dataType);
}

/* static */ VtValue
HdEmbreeTriangleFaceVaryingSampler::_Triangulate(TfToken const& name,
    VtValue const& value, HdMeshUtil &meshUtil)
{
    HdVtBufferSource buffer(name, value);
    VtValue triangulated;
    const HdMeshComputationResult status =
        meshUtil.ComputeTriangulatedFaceVaryingPrimvar(
            buffer.GetData(),
            buffer.GetNumElements(),
            buffer.GetTupleType().type,
            &triangulated);
    switch (status) {
    case HdMeshComputationResult::Error:
        TF_CODING_ERROR("[%s] Could not triangulate face-varying data.",
            name.GetText());
        return triangulated;
    case HdMeshComputationResult::Success:
        return triangulated;
    case HdMeshComputationResult::Unchanged:
    default:
        return value;
    }
}

namespace {

RTCFormat
_GetSubdivAttributeFormat(HdTupleType tupleType)
{
    switch (HdGetComponentType(tupleType.type)) {
    case HdTypeFloat:     return RTC_FORMAT_FLOAT;
    case HdTypeFloatVec2: return RTC_FORMAT_FLOAT2;
    case HdTypeFloatVec3: return RTC_FORMAT_FLOAT3;
    case HdTypeFloatVec4: return RTC_FORMAT_FLOAT4;
    default:              return RTC_FORMAT_UNDEFINED;
    }
}

bool
_UploadSubdivAttribute(
    RTCGeometry geometry,
    int bufferId,
    HdVtBufferSource const& buffer,
    char const* interpolation)
{
    if (buffer.GetTupleType().count != 1) {
        TF_WARN("Unsupported array size for %s primvar", interpolation);
        return false;
    }

    RTCFormat const format = _GetSubdivAttributeFormat(buffer.GetTupleType());
    if (format == RTC_FORMAT_UNDEFINED) {
        TF_WARN("Embree subdivision meshes only support float-based primvars "
                "for %s interpolation", interpolation);
        return false;
    }

    // rtcInterpolate1 may issue a 16-byte load for the final element even for
    // scalar/vec2/vec3 attributes. Embree-owned, 16-byte-strided storage keeps
    // that load inside the buffer instead of relying on VtArray tail padding.
    constexpr size_t paddedStride = 4 * sizeof(float);
    char* destination = static_cast<char*>(rtcSetNewGeometryBuffer(
        geometry,
        RTC_BUFFER_TYPE_VERTEX_ATTRIBUTE,
        static_cast<unsigned int>(bufferId),
        format,
        paddedStride,
        buffer.GetNumElements()));
    if (!destination && buffer.GetNumElements() != 0) {
        return false;
    }

    size_t const sourceStride = HdDataSizeOfTupleType(buffer.GetTupleType());
    char const* source = static_cast<char const*>(buffer.GetData());
    for (size_t i = 0; i < buffer.GetNumElements(); ++i) {
        std::memset(destination + i * paddedStride, 0, paddedStride);
        std::memcpy(
            destination + i * paddedStride,
            source + i * sourceStride,
            sourceStride);
    }
    return true;
}

bool
_SampleSubdivAttribute(
    RTCGeometry geometry,
    int bufferId,
    HdTupleType storedType,
    unsigned int element,
    float u,
    float v,
    void* value,
    void* dPdu,
    void* dPdv,
    HdTupleType requestedType)
{
    if (!geometry || bufferId == -1 || requestedType != storedType) {
        return false;
    }

    unsigned int const componentCount =
        HdGetComponentCount(requestedType.type) * requestedType.count;
    if (componentCount > 4) {
        return false;
    }

    // Embree documents the output arrays as 16-byte padded. Sampling into
    // local float4s avoids overwriting compact GfVec2f/GfVec3f destinations.
    alignas(16) float sampled[4] = {};
    alignas(16) float sampledDu[4] = {};
    alignas(16) float sampledDv[4] = {};
    rtcInterpolate1(
        geometry,
        element,
        u,
        v,
        RTC_BUFFER_TYPE_VERTEX_ATTRIBUTE,
        static_cast<unsigned int>(bufferId),
        sampled,
        dPdu ? sampledDu : nullptr,
        dPdv ? sampledDv : nullptr,
        componentCount);

    size_t const resultSize = componentCount * sizeof(float);
    std::memcpy(value, sampled, resultSize);
    if (dPdu) {
        std::memcpy(dPdu, sampledDu, resultSize);
    }
    if (dPdv) {
        std::memcpy(dPdv, sampledDv, resultSize);
    }
    return true;
}

} // namespace

// HdEmbreeSubdivSampler

HdEmbreeSubdivSampler::HdEmbreeSubdivSampler(
    TfToken const& name,
    VtValue const& value,
    RTCGeometry geometry,
    HdEmbreeRTCBufferAllocator* allocator,
    char const* interpolation,
    int topologyId)
    : _embreeBufferId(allocator->Allocate())
    , _buffer(name, value)
    , _geometry(geometry)
    , _allocator(allocator)
{
    if (_embreeBufferId == -1) {
        TF_WARN("Embree subdivision primvar buffer limit exceeded for %s",
                name.GetText());
        return;
    }

    rtcSetGeometryVertexAttributeCount(_geometry, _allocator->NumBuffers());
    if (!_UploadSubdivAttribute(
            _geometry, _embreeBufferId, _buffer, interpolation)) {
        _allocator->Free(_embreeBufferId);
        _embreeBufferId = -1;
        return;
    }

    if (topologyId >= 0) {
        // Varying uses the shared linear topology; each face-varying primvar
        // uses its own topology because independently authored seams differ.
        rtcSetGeometryVertexAttributeTopology(
            _geometry,
            static_cast<unsigned int>(_embreeBufferId),
            static_cast<unsigned int>(topologyId));
    }
}

HdEmbreeSubdivSampler::~HdEmbreeSubdivSampler()
{
    if (_embreeBufferId != -1) {
        _allocator->Free(_embreeBufferId);
    }
}

bool
HdEmbreeSubdivSampler::Sample(
    unsigned int element, float u, float v,
    void* value, HdTupleType dataType) const
{
    return _SampleSubdivAttribute(
        _geometry, _embreeBufferId, _buffer.GetTupleType(),
        element, u, v, value, nullptr, nullptr, dataType);
}

bool
HdEmbreeSubdivSampler::SampleWithDerivatives(
    unsigned int element, float u, float v,
    void* value, void* dPdu, void* dPdv,
    HdTupleType dataType) const
{
    return _SampleSubdivAttribute(
        _geometry, _embreeBufferId, _buffer.GetTupleType(),
        element, u, v, value, dPdu, dPdv, dataType);
}

HdEmbreeSubdivVertexSampler::HdEmbreeSubdivVertexSampler(
    TfToken const& name,
    VtValue const& value,
    RTCGeometry geometry,
    HdEmbreeRTCBufferAllocator* allocator)
    : HdEmbreeSubdivSampler(
        name, value, geometry, allocator, "vertex")
{
}

HdEmbreeSubdivVaryingSampler::HdEmbreeSubdivVaryingSampler(
    TfToken const& name,
    VtValue const& value,
    RTCGeometry geometry,
    HdEmbreeRTCBufferAllocator* allocator)
    : HdEmbreeSubdivSampler(
        name, value, geometry, allocator, "varying", 1)
{
}

HdEmbreeSubdivFaceVaryingSampler::HdEmbreeSubdivFaceVaryingSampler(
    TfToken const& name,
    VtValue const& value,
    RTCGeometry geometry,
    unsigned int topologyId,
    HdEmbreeRTCBufferAllocator* allocator)
    : HdEmbreeSubdivSampler(
        name, value, geometry, allocator, "face-varying", topologyId)
{
}

PXR_NAMESPACE_CLOSE_SCOPE
