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
    if (token == GfColorSpaceNames->Data) {
        *result = HdEmbreeRenderColorSpace::Data;
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
    case HdEmbreeRenderColorSpace::Data:
        return GfColorSpaceNames->Data;
    }
    return GfColorSpaceNames->LinearRec709;
}

bool
HdEmbreeBypassesColorTransforms(HdEmbreeRenderColorSpace colorSpace)
{
    return colorSpace == HdEmbreeRenderColorSpace::Data;
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

    const TfToken sourceColorSpaceToken(sourceColorSpace);
    if (sourceColorSpaceToken == GfColorSpaceNames->Data ||
        sourceColorSpaceToken == GfColorSpaceNames->Raw ||
        sourceColorSpaceToken == GfColorSpaceNames->Identity ||
        sourceColorSpaceToken == GfColorSpaceNames->Unknown) {
        return HdEmbreeColorSpaceResolution::NoTransform;
    }

    if (!GfColorSpace::IsValid(sourceColorSpaceToken)) {
        return HdEmbreeColorSpaceResolution::Unsupported;
    }

    *resolvedColorSpace = sourceColorSpaceToken;
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
