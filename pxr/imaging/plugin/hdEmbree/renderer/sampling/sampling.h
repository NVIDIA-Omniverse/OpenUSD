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
#include "pxr/base/tf/token.h"
#include "pxr/pxr.h"

#include <oqmc/oqmc.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <variant>

PXR_NAMESPACE_OPEN_SCOPE
namespace ty {

// ---------------------------------------------------------------------------
// Sampler sequence selection
// ---------------------------------------------------------------------------

enum class SamplerSequence : uint8_t
{
    OpenQMCSobol,
    OpenQMCSobolBN,
    OpenQMCPMJ,
    OpenQMCPMJBN,
    OpenQMCLattice,
    OpenQMCLatticeBN
};

inline TfToken
GetSamplerSequenceToken(SamplerSequence sequence)
{
    switch (sequence) {
    case SamplerSequence::OpenQMCSobol:
        return TfToken("openqmc_sobol");
    case SamplerSequence::OpenQMCSobolBN:
        return TfToken("openqmc_sobolbn");
    case SamplerSequence::OpenQMCPMJ:
        return TfToken("openqmc_pmj");
    case SamplerSequence::OpenQMCPMJBN:
        return TfToken("openqmc_pmjbn");
    case SamplerSequence::OpenQMCLattice:
        return TfToken("openqmc_lattice");
    case SamplerSequence::OpenQMCLatticeBN:
        return TfToken("openqmc_latticebn");
    }
    return TfToken("openqmc_sobolbn");
}

inline SamplerSequence
GetDefaultSamplerSequence()
{
    return SamplerSequence::OpenQMCSobolBN;
}

inline SamplerSequence
GetSamplerSequenceFromToken(TfToken const& token)
{
    if (token == TfToken("openqmc_sobol")) {
        return SamplerSequence::OpenQMCSobol;
    }
    if (token == TfToken("openqmc_sobolbn")) {
        return SamplerSequence::OpenQMCSobolBN;
    }
    if (token == TfToken("openqmc_pmj")) {
        return SamplerSequence::OpenQMCPMJ;
    }
    if (token == TfToken("openqmc_pmjbn")) {
        return SamplerSequence::OpenQMCPMJBN;
    }
    if (token == TfToken("openqmc_lattice")) {
        return SamplerSequence::OpenQMCLattice;
    }
    if (token == TfToken("openqmc_latticebn")) {
        return SamplerSequence::OpenQMCLatticeBN;
    }
    return GetDefaultSamplerSequence();
}

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

using OpenQmcVariant = std::variant<
    std::monostate,
    oqmc::SobolSampler,
    oqmc::SobolBnSampler,
    oqmc::PmjSampler,
    oqmc::PmjBnSampler,
    oqmc::LatticeSampler,
    oqmc::LatticeBnSampler>;

struct SampleDomain
{
    SamplerSequence sequence =
        GetDefaultSamplerSequence();
    OpenQmcVariant openQmcDomain;

    SampleDomain() = default;

    SampleDomain(SamplerSequence samplerSequence,
                         OpenQmcVariant openQmcSampler)
        : sequence(samplerSequence)
        , openQmcDomain(openQmcSampler)
    {
    }

    SampleDomain Fork(SampleDomainKey key) const
    {
        const int domainKey =
            static_cast<int>(SampleDomainKeyValue(key));
        // C++17 requires auto for the std::visit visitor parameter.
        return std::visit(
            [this, domainKey](auto const& sampler)
                -> SampleDomain {
                using SamplerT = std::decay_t<decltype(sampler)>;
                if constexpr (std::is_same_v<SamplerT, std::monostate>) {
                    return SampleDomain();
                } else {
                    return SampleDomain(
                        sequence, sampler.newDomain(domainKey));
                }
            },
            openQmcDomain);
    }

    SampleDomain Split(SampleDomainKey key,
                               int size,
                               int index) const
    {
        const int safeSize = std::max(size, 1);
        const int safeIndex = std::max(index, 0);
        const int domainKey =
            static_cast<int>(SampleDomainKeyValue(key));
        // C++17 requires auto for the std::visit visitor parameter.
        return std::visit(
            [this, domainKey, safeSize, safeIndex](auto const& sampler)
                -> SampleDomain {
                using SamplerT = std::decay_t<decltype(sampler)>;
                if constexpr (std::is_same_v<SamplerT, std::monostate>) {
                    return SampleDomain();
                } else {
                    return SampleDomain(
                        sequence,
                        sampler.newDomainSplit(
                            domainKey, safeSize, safeIndex));
                }
            },
            openQmcDomain);
    }

    SampleDomain Distrib(SampleDomainKey key,
                                 int index) const
    {
        const int safeIndex = std::max(index, 0);
        const int domainKey =
            static_cast<int>(SampleDomainKeyValue(key));
        // C++17 requires auto for the std::visit visitor parameter.
        return std::visit(
            [this, domainKey, safeIndex](auto const& sampler)
                -> SampleDomain {
                using SamplerT = std::decay_t<decltype(sampler)>;
                if constexpr (std::is_same_v<SamplerT, std::monostate>) {
                    return SampleDomain();
                } else {
                    return SampleDomain(
                        sequence,
                        sampler.newDomainDistrib(domainKey, safeIndex));
                }
            },
            openQmcDomain);
    }

    SampleDomain Chain(SampleDomainKey key,
                               int index) const
    {
        const int safeIndex = std::max(index, 0);
        const int domainKey =
            static_cast<int>(SampleDomainKeyValue(key));
        // C++17 requires auto for the std::visit visitor parameter.
        return std::visit(
            [this, domainKey, safeIndex](auto const& sampler)
                -> SampleDomain {
                using SamplerT = std::decay_t<decltype(sampler)>;
                if constexpr (std::is_same_v<SamplerT, std::monostate>) {
                    return SampleDomain();
                } else {
                    return SampleDomain(
                        sequence,
                        sampler.newDomainChain(domainKey, safeIndex));
                }
            },
            openQmcDomain);
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
        // C++17 requires auto for the std::visit visitor parameter.
        std::visit(
            [&sample](auto const& sampler) {
                using SamplerT = std::decay_t<decltype(sampler)>;
                if constexpr (!std::is_same_v<SamplerT, std::monostate>) {
                    sampler.template drawSample<Size>(sample.data());
                }
            },
            openQmcDomain);
        return sample;
    }
};

struct Sampler
{
    SamplerSequence sequence =
        GetDefaultSamplerSequence();
    OpenQmcVariant openQmcRoot;

    Sampler(uint32_t frameSeed,
                    uint32_t pixelX,
                    uint32_t pixelY,
                    uint32_t sampleIdx,
                    SamplerSequence samplerSequence)
        : sequence(samplerSequence)
    {
        const int x = static_cast<int>(pixelX);
        const int y = static_cast<int>(pixelY);
        const int frame = static_cast<int>(frameSeed & 0x7fffffffu);
        const int index = static_cast<int>(sampleIdx);

        switch (sequence) {
        case SamplerSequence::OpenQMCSobol:
            openQmcRoot = oqmc::SobolSampler(
                x, y, frame, index, _GetOpenQMCCache<oqmc::SobolSampler>());
            break;
        case SamplerSequence::OpenQMCSobolBN:
            openQmcRoot = oqmc::SobolBnSampler(
                x, y, frame, index, _GetOpenQMCCache<oqmc::SobolBnSampler>());
            break;
        case SamplerSequence::OpenQMCPMJ:
            openQmcRoot = oqmc::PmjSampler(
                x, y, frame, index, _GetOpenQMCCache<oqmc::PmjSampler>());
            break;
        case SamplerSequence::OpenQMCPMJBN:
            openQmcRoot = oqmc::PmjBnSampler(
                x, y, frame, index, _GetOpenQMCCache<oqmc::PmjBnSampler>());
            break;
        case SamplerSequence::OpenQMCLattice:
            openQmcRoot = oqmc::LatticeSampler(
                x, y, frame, index, _GetOpenQMCCache<oqmc::LatticeSampler>());
            break;
        case SamplerSequence::OpenQMCLatticeBN:
            openQmcRoot = oqmc::LatticeBnSampler(
                x, y, frame, index, _GetOpenQMCCache<oqmc::LatticeBnSampler>());
            break;
        }
    }

    SampleDomain RootDomain() const
    {
        return SampleDomain(sequence, openQmcRoot);
    }

private:
    template <typename SamplerT>
    static char*
    _GetOpenQMCCache()
    {
        struct _Cache {
            std::array<char, SamplerT::cacheSize> bytes{};

            _Cache() { SamplerT::initialiseCache(bytes.data()); }
        };

        static _Cache cache;
        return cache.bytes.data();
    }
};

} // namespace ty
PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_IMAGING_PLUGIN_HD_EMBREE_SAMPLING_H
