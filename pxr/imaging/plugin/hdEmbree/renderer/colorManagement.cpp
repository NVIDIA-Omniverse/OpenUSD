//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/imaging/plugin/hdEmbree/renderer/colorManagement.h"

#include "pxr/base/gf/colorSpace.h"
#include "pxr/base/gf/matrix3f.h"
#include "pxr/base/tf/span.h"

#include <algorithm>
#include <array>
#include <cctype>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

std::string
_NormalizeColorSpaceName(std::string const& name)
{
    std::string normalized = name;
    std::transform(
        normalized.begin(), normalized.end(), normalized.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return normalized;
}

} // anonymous namespace

bool
HdEmbreeParseRenderColorSpace(
    TfToken const& token,
    HdEmbreeRenderColorSpace* result)
{
    if (!result) {
        return false;
    }
    if (token == GfColorSpaceNames->LinearRec709) {
        *result = HdEmbreeRenderColorSpace::LinearRec709;
        return true;
    }
    if (token == GfColorSpaceNames->LinearAP1) {
        *result = HdEmbreeRenderColorSpace::LinearAP1;
        return true;
    }
    if (token == GfColorSpaceNames->Raw) {
        *result = HdEmbreeRenderColorSpace::Raw;
        return true;
    }
    return false;
}

TfToken const&
HdEmbreeGetRenderColorSpaceToken(HdEmbreeRenderColorSpace colorSpace)
{
    switch (colorSpace) {
    case HdEmbreeRenderColorSpace::LinearRec709:
        return GfColorSpaceNames->LinearRec709;
    case HdEmbreeRenderColorSpace::LinearAP1:
        return GfColorSpaceNames->LinearAP1;
    case HdEmbreeRenderColorSpace::Raw:
        return GfColorSpaceNames->Raw;
    }
    return GfColorSpaceNames->LinearRec709;
}

bool
HdEmbreeBypassesColorTransforms(HdEmbreeRenderColorSpace colorSpace)
{
    return colorSpace == HdEmbreeRenderColorSpace::Raw;
}

TfToken const&
HdEmbreeGetWorkingColorSpaceToken(HdEmbreeRenderColorSpace colorSpace)
{
    return colorSpace == HdEmbreeRenderColorSpace::LinearAP1
        ? GfColorSpaceNames->LinearAP1
        : GfColorSpaceNames->LinearRec709;
}

GfVec3f
HdEmbreeGetLuminanceCoefficients(HdEmbreeRenderColorSpace colorSpace)
{
    const GfMatrix3f rgbToXyz =
        GfColorSpace(HdEmbreeGetWorkingColorSpaceToken(colorSpace))
            .GetRGBToXYZ();
    return GfVec3f(rgbToXyz[1][0], rgbToXyz[1][1], rgbToXyz[1][2]);
}

HdEmbreeColorSpaceResolution
HdEmbreeResolveColorSpace(
    std::string const& sourceColorSpace,
    TfToken* resolvedColorSpace)
{
    if (!resolvedColorSpace) {
        return HdEmbreeColorSpaceResolution::Unsupported;
    }

    const std::string normalized =
        _NormalizeColorSpaceName(sourceColorSpace);
    if (normalized.empty() ||
        normalized == "none" ||
        normalized == "raw" ||
        normalized == "data" ||
        normalized == "auto" ||
        normalized == "identity") {
        return HdEmbreeColorSpaceResolution::NoTransform;
    }

    if (normalized == "srgb_texture" || normalized == "srgb") {
        *resolvedColorSpace = GfColorSpaceNames->SRGBRec709;
    } else if (normalized == "lin_rec709" || normalized == "lin_srgb") {
        *resolvedColorSpace = GfColorSpaceNames->LinearRec709;
    } else if (normalized == "g22_rec709") {
        *resolvedColorSpace = GfColorSpaceNames->G22Rec709;
    } else if (normalized == "g18_rec709") {
        *resolvedColorSpace = GfColorSpaceNames->G18Rec709;
    } else if (normalized == "acescg" || normalized == "lin_ap1") {
        *resolvedColorSpace = GfColorSpaceNames->LinearAP1;
    } else if (normalized == "g22_ap1") {
        *resolvedColorSpace = GfColorSpaceNames->G22AP1;
    } else if (normalized == "adobergb") {
        *resolvedColorSpace = GfColorSpaceNames->G22AdobeRGB;
    } else if (normalized == "lin_adobergb") {
        *resolvedColorSpace = GfColorSpaceNames->LinearAdobeRGB;
    } else if (normalized == "srgb_displayp3") {
        *resolvedColorSpace = GfColorSpaceNames->SRGBP3D65;
    } else if (normalized == "lin_displayp3") {
        *resolvedColorSpace = GfColorSpaceNames->LinearP3D65;
    } else {
        const TfToken directToken(normalized);
        if (!GfColorSpace::IsValid(directToken)) {
            return HdEmbreeColorSpaceResolution::Unsupported;
        }
        *resolvedColorSpace = directToken;
    }

    return HdEmbreeColorSpaceResolution::Transform;
}

bool
HdEmbreeConvertToRenderColorSpace(
    std::string const& sourceColorSpace,
    HdEmbreeRenderColorSpace renderColorSpace,
    GfVec3f* rgb)
{
    if (!rgb) {
        return false;
    }
    if (HdEmbreeBypassesColorTransforms(renderColorSpace)) {
        return true;
    }

    TfToken sourceColorSpaceToken;
    const HdEmbreeColorSpaceResolution resolution =
        HdEmbreeResolveColorSpace(
            sourceColorSpace, &sourceColorSpaceToken);
    if (resolution == HdEmbreeColorSpaceResolution::NoTransform) {
        return true;
    }
    if (resolution == HdEmbreeColorSpaceResolution::Unsupported) {
        return false;
    }

    const TfToken& destinationColorSpaceToken =
        HdEmbreeGetWorkingColorSpaceToken(renderColorSpace);
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
