//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
// Hero-wavelength conversions and the HdEmbreeRenderer members that use them.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_HERO_WAVELENGTH_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_HERO_WAVELENGTH_H

#include "renderer.h"
#include "rendererMath.h"

#include <renderer/materials/MaterialXCpp/spectral.h>

PXR_NAMESPACE_OPEN_SCOPE
namespace ty {

inline float
RgbToSpectralValue(
    const GfVec3f& rgb,
    const _HeroWavelengthState& hero,
    HdEmbreeRenderColorSpace renderColorSpace)
{
    const mxcpp::Spectral::RgbColorSpace spectralColorSpace =
        renderColorSpace == HdEmbreeRenderColorSpace::LinearAP1
            ? mxcpp::Spectral::RgbColorSpace::LinearAP1
            : mxcpp::Spectral::RgbColorSpace::LinearRec709;
    return mxcpp::Spectral::RgbToSpectralValue(
        ToMx(rgb), hero.wavelengthNm, spectralColorSpace);
}

inline GfVec3f
SpectralValueToRgb(
    float value,
    const _HeroWavelengthState& hero,
    HdEmbreeRenderColorSpace renderColorSpace)
{
    const mxcpp::Spectral::RgbColorSpace spectralColorSpace =
        renderColorSpace == HdEmbreeRenderColorSpace::LinearAP1
            ? mxcpp::Spectral::RgbColorSpace::LinearAP1
            : mxcpp::Spectral::RgbColorSpace::LinearRec709;
    return ToGf(mxcpp::Spectral::SpectralValueToRgb(
        value,
        hero.wavelengthNm,
        hero.pdf,
        spectralColorSpace));
}

inline GfVec3f
SpectralScalarToRgb(
    float value,
    const _HeroWavelengthState& hero,
    HdEmbreeRenderColorSpace renderColorSpace)
{
    return hero.active
        ? SpectralValueToRgb(value, hero, renderColorSpace)
                       : GfVec3f(value);
}

} // namespace ty

inline void
HdEmbreeRenderer::_ApplyPathWeight(
    GfVec3f const& weight, _PathState* state) const
{
    if (!state) {
        return;
    }
    if (state->hero.active) {
        const _HeroWavelengthState hero{
            true, state->hero.wavelengthNm, state->hero.pdf};
        state->throughputSpectral *=
            ty::RgbToSpectralValue(weight, hero, _renderColorSpace);
    } else {
        state->throughputRgb = GfCompMult(state->throughputRgb, weight);
    }
}

inline GfVec3f
HdEmbreeRenderer::_GetPathThroughputRgb(_PathState const& state) const
{
    const _HeroWavelengthState hero{
        state.hero.active,
        state.hero.wavelengthNm,
        state.hero.pdf};
    return state.hero.active
               ? ty::SpectralScalarToRgb(
                     state.throughputSpectral, hero, _renderColorSpace)
               : state.throughputRgb;
}

PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_IMAGING_PLUGIN_HD_EMBREE_HERO_WAVELENGTH_H
