//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_SAMPLING_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_SAMPLING_H

#include "pxr/base/gf/vec2f.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/gf/vec4f.h"
#include "pxr/pxr.h"

#include <oqmc/oqmc.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

PXR_NAMESPACE_OPEN_SCOPE
namespace ty {

// The renderer-wide OpenQMC sequence is selected here and nowhere else.
using OpenQmcSampler = oqmc::SobolBnSampler;

inline uint32_t
ResolveFrameSeed(int configuredSeed, float sceneFrame)
{
    if (configuredSeed != -1) {
        return static_cast<uint32_t>(configuredSeed);
    }
    static_assert(sizeof(sceneFrame) == sizeof(uint32_t));
    uint32_t frameSeed;
    std::memcpy(&frameSeed, &sceneFrame, sizeof(frameSeed));
    return frameSeed;
}

// ---------------------------------------------------------------------------
// Domain-aware sampler API
// ---------------------------------------------------------------------------

enum class SampleDomainKey : uint32_t
{
    Pixel = 0x0001u,
    CameraJitter = 0x0010u,
    CameraLens = 0x0011u,
    AmbientOcclusion = 0x001fu,
    AmbientOcclusionSample = 0x0020u,
    AmbientOcclusionShuffle = 0x0021u,
    PathBounce = 0x0100u,
    Presence = 0x0110u,
    Wavelength = 0x0111u,
    BsdfSample = 0x0120u,
    DirectLighting = 0x012fu,
    DirectLightSelect = 0x0130u,
    DirectLightSample = 0x0131u,
    RussianRoulette = 0x0140u,
    MediumChannel = 0x0200u,
    MediumFreeFlight = 0x0201u,
    MediumDirectLighting = 0x020fu,
    MediumDirectLightSelect = 0x0210u,
    MediumDirectLightSample = 0x0211u,
    MediumRussianRoulette = 0x0220u,
    MediumPhase = 0x0230u,
    SssEntry = 0x0300u,
    SssEntryDirection = 0x0301u,
    SssBounce = 0x0310u,
    SssChannel = 0x0320u,
    SssGuideChoice = 0x0321u,
    SssBackwardChoice = 0x0322u,
    SssPhaseDirection = 0x0323u,
    SssFreeFlight = 0x0324u
};

inline uint32_t
SampleDomainKeyValue(SampleDomainKey key)
{
    return static_cast<uint32_t>(key);
}

struct SampleDomain
{
    explicit SampleDomain(OpenQmcSampler const& openQmcSampler)
        : openQmcDomain(openQmcSampler)
    {
    }

    SampleDomain Fork(SampleDomainKey key) const
    {
        const int domainKey =
            static_cast<int>(SampleDomainKeyValue(key));
        return SampleDomain(openQmcDomain.newDomain(domainKey));
    }

    SampleDomain Split(SampleDomainKey key,
                               int size,
                               int index) const
    {
        const int safeSize = std::max(size, 1);
        const int safeIndex = std::max(index, 0);
        const int domainKey =
            static_cast<int>(SampleDomainKeyValue(key));
        return SampleDomain(
            openQmcDomain.newDomainSplit(
                domainKey, safeSize, safeIndex));
    }

    SampleDomain Distrib(SampleDomainKey key,
                                 int index) const
    {
        const int safeIndex = std::max(index, 0);
        const int domainKey =
            static_cast<int>(SampleDomainKeyValue(key));
        return SampleDomain(
            openQmcDomain.newDomainDistrib(domainKey, safeIndex));
    }

    SampleDomain Chain(SampleDomainKey key,
                               int index) const
    {
        const int safeIndex = std::max(index, 0);
        const int domainKey =
            static_cast<int>(SampleDomainKeyValue(key));
        return SampleDomain(
            openQmcDomain.newDomainChain(domainKey, safeIndex));
    }

    float Draw1D() const
    {
        return _Draw<1>()[0];
    }

    GfVec2f Draw2D() const
    {
        const std::array<float, 2> sample = _Draw<2>();
        return GfVec2f(sample[0], sample[1]);
    }

    GfVec3f Draw3D() const
    {
        const std::array<float, 3> sample = _Draw<3>();
        return GfVec3f(sample[0], sample[1], sample[2]);
    }

    GfVec4f Draw4D() const
    {
        const std::array<float, 4> sample = _Draw<4>();
        return GfVec4f(sample[0], sample[1], sample[2], sample[3]);
    }

private:
    template <int Size>
    std::array<float, Size> _Draw() const
    {
        static_assert(Size >= 1, "Draw size must be at least one.");
        static_assert(Size <= 4, "Draw size must be at most four.");

        std::array<float, Size> sample{};
        openQmcDomain.drawSample<Size>(sample.data());
        return sample;
    }

    OpenQmcSampler openQmcDomain;
};

struct Sampler
{
    Sampler(uint32_t frameSeed,
            uint32_t pixelX,
            uint32_t pixelY,
            uint32_t sampleIdx)
        : openQmcRoot(
            static_cast<int>(pixelX),
            static_cast<int>(pixelY),
            static_cast<int>(frameSeed & 0x7fffffffu),
            static_cast<int>(sampleIdx),
            _GetOpenQMCCache())
    {
    }

    SampleDomain RootDomain() const
    {
        return SampleDomain(openQmcRoot);
    }

private:
    static char*
    _GetOpenQMCCache()
    {
        struct _Cache {
            std::array<char, OpenQmcSampler::cacheSize> bytes{};

            _Cache() { OpenQmcSampler::initialiseCache(bytes.data()); }
        };

        static _Cache cache;
        return cache.bytes.data();
    }

    OpenQmcSampler openQmcRoot;
};

} // namespace ty
PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_IMAGING_PLUGIN_HD_EMBREE_SAMPLING_H
