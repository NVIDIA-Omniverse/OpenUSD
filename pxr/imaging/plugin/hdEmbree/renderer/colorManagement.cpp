//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "colorManagement.h"

#include "pxr/base/gf/colorSpace.h"
#include "pxr/base/gf/matrix3f.h"
#include "pxr/base/tf/span.h"

#include <array>

PXR_NAMESPACE_OPEN_SCOPE

bool
ty::ParseRenderColorSpace(
    TfToken const& token,
    ty::RenderColorSpace* result)
{
    if (!result) {
        return false;
    }
    if (token == GfColorSpaceNames->LinearRec709) {
        *result = ty::RenderColorSpace::LinearRec709;
        return true;
    }
    if (token == GfColorSpaceNames->LinearAP1) {
        *result = ty::RenderColorSpace::LinearAP1;
        return true;
    }
    if (token == GfColorSpaceNames->Data) {
        *result = ty::RenderColorSpace::Data;
        return true;
    }
    return false;
}

TfToken const&
ty::GetRenderColorSpaceToken(ty::RenderColorSpace colorSpace)
{
    switch (colorSpace) {
    case ty::RenderColorSpace::LinearRec709:
        return GfColorSpaceNames->LinearRec709;
    case ty::RenderColorSpace::LinearAP1:
        return GfColorSpaceNames->LinearAP1;
    case ty::RenderColorSpace::Data:
        return GfColorSpaceNames->Data;
    }
    return GfColorSpaceNames->LinearRec709;
}

bool
ty::BypassesColorTransforms(ty::RenderColorSpace colorSpace)
{
    return colorSpace == ty::RenderColorSpace::Data;
}

TfToken const&
ty::GetWorkingColorSpaceToken(ty::RenderColorSpace colorSpace)
{
    return colorSpace == ty::RenderColorSpace::LinearAP1
        ? GfColorSpaceNames->LinearAP1
        : GfColorSpaceNames->LinearRec709;
}

GfVec3f
ty::GetLuminanceCoefficients(ty::RenderColorSpace colorSpace)
{
    const GfMatrix3f rgbToXyz =
        GfColorSpace(ty::GetWorkingColorSpaceToken(colorSpace))
            .GetRGBToXYZ();
    return GfVec3f(rgbToXyz[1][0], rgbToXyz[1][1], rgbToXyz[1][2]);
}

ty::ColorSpaceResolution
ty::ResolveColorSpace(
    std::string const& sourceColorSpace,
    TfToken* resolvedColorSpace)
{
    if (!resolvedColorSpace) {
        return ty::ColorSpaceResolution::Unsupported;
    }

    const TfToken sourceColorSpaceToken(sourceColorSpace);
    if (sourceColorSpaceToken == GfColorSpaceNames->Data ||
        sourceColorSpaceToken == GfColorSpaceNames->Raw ||
        sourceColorSpaceToken == GfColorSpaceNames->Identity ||
        sourceColorSpaceToken == GfColorSpaceNames->Unknown) {
        return ty::ColorSpaceResolution::NoTransform;
    }

    if (!GfColorSpace::IsValid(sourceColorSpaceToken)) {
        return ty::ColorSpaceResolution::Unsupported;
    }

    *resolvedColorSpace = sourceColorSpaceToken;
    return ty::ColorSpaceResolution::Transform;
}

bool
ty::ConvertToRenderColorSpace(
    std::string const& sourceColorSpace,
    ty::RenderColorSpace renderColorSpace,
    GfVec3f* rgb)
{
    if (!rgb) {
        return false;
    }
    if (ty::BypassesColorTransforms(renderColorSpace)) {
        return true;
    }

    TfToken sourceColorSpaceToken;
    const ty::ColorSpaceResolution resolution =
        ty::ResolveColorSpace(
            sourceColorSpace, &sourceColorSpaceToken);
    if (resolution == ty::ColorSpaceResolution::NoTransform) {
        return true;
    }
    if (resolution == ty::ColorSpaceResolution::Unsupported) {
        return false;
    }

    const TfToken& destinationColorSpaceToken =
        ty::GetWorkingColorSpaceToken(renderColorSpace);
    if (sourceColorSpaceToken == destinationColorSpaceToken) {
        return true;
    }

    std::array<float, 3> values = {(*rgb)[0], (*rgb)[1], (*rgb)[2]};
    const GfColorSpace source(sourceColorSpaceToken);
    const GfColorSpace destination(destinationColorSpaceToken);
    source.ConvertRGBSpan(
        destination, TfSpan<float>(values.data(), values.size()));
    *rgb = GfVec3f(values[0], values[1], values[2]);
    return true;
}

PXR_NAMESPACE_CLOSE_SCOPE
