//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_COLOR_MANAGEMENT_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_COLOR_MANAGEMENT_H

#include "pxr/base/gf/vec3f.h"
#include "pxr/base/tf/token.h"
#include "pxr/pxr.h"

#include <string>

PXR_NAMESPACE_OPEN_SCOPE

/// Renderer working spaces supported by hdEmbree.
enum class HdEmbreeRenderColorSpace
{
    LinearRec709,
    LinearAP1,
    Data
};

/// Result of resolving an authored source color-space name.
enum class HdEmbreeColorSpaceResolution
{
    NoTransform,
    Transform,
    Unsupported
};

/// Parse a standard UsdRenderSettings renderingColorSpace token.
///
/// Returns false for unsupported tokens and leaves \p result unchanged.
bool HdEmbreeParseRenderColorSpace(
    TfToken const& token,
    HdEmbreeRenderColorSpace* result);

/// Return the standard token corresponding to \p colorSpace.
TfToken const& HdEmbreeGetRenderColorSpaceToken(
    HdEmbreeRenderColorSpace colorSpace);

/// Return true when renderer-managed color transforms must be bypassed.
bool HdEmbreeBypassesColorTransforms(
    HdEmbreeRenderColorSpace colorSpace);

/// Return the linear RGB space used for numerical color algorithms.
///
/// Data has no primaries, so it deliberately uses Linear Rec.709 as the
/// deterministic, backward-compatible fallback for algorithms that require
/// an RGB basis.
TfToken const& HdEmbreeGetWorkingColorSpaceToken(
    HdEmbreeRenderColorSpace colorSpace);

/// Return RGB coefficients that compute CIE Y in the selected working space.
///
/// Data uses the Linear Rec.709 fallback described above.
GfVec3f HdEmbreeGetLuminanceCoefficients(
    HdEmbreeRenderColorSpace colorSpace);

/// Resolve a canonical Gf color-space name without alias interpretation.
HdEmbreeColorSpaceResolution HdEmbreeResolveColorSpace(
    std::string const& sourceColorSpace,
    TfToken* resolvedColorSpace);

/// Convert one linear or encoded RGB triplet into the renderer working space.
///
/// Returns false only when the source color-space name is unsupported. Data
/// designations and a matching source/destination are successful no-ops.
bool HdEmbreeConvertToRenderColorSpace(
    std::string const& sourceColorSpace,
    HdEmbreeRenderColorSpace renderColorSpace,
    GfVec3f* rgb);

PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_IMAGING_PLUGIN_HD_EMBREE_COLOR_MANAGEMENT_H
