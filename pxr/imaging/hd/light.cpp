//
// Copyright 2016 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/imaging/hd/light.h"

#include "pxr/imaging/hd/changeTracker.h"
#include "pxr/imaging/hd/instancer.h"
#include "pxr/imaging/hd/lightIesProfile.h"
#include "pxr/imaging/hd/material.h"
#include "pxr/imaging/hd/sceneDelegate.h"
#include "pxr/imaging/hd/tokens.h"

#include "pxr/base/gf/color.h"
#include "pxr/base/gf/colorSpace.h"
#include "pxr/base/gf/math.h"
#include "pxr/base/gf/vec3d.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/tf/stringUtils.h"
#include "pxr/base/vt/value.h"
#include "pxr/imaging/hio/image.h"
#include "pxr/usd/sdf/assetPath.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

TF_DEFINE_PUBLIC_TOKENS(HdLightTokens, HD_LIGHT_TOKENS);

namespace {

constexpr double _Pi = 3.14159265358979323846;
constexpr double _MinLightSize = 1.0e-4;

const GfColorSpace _linearRec709ColorSpace(GfColorSpaceNames->LinearRec709);
const GfColorSpace _xyzColorSpace(GfColorSpaceNames->CIEXYZ);
const GfColorSpace _linearXyzD65ColorSpace(GfColorSpaceNames->LinearCIEXYZD65);
const TfToken _colorSpaceMetadataKey("oiio:ColorSpace", TfToken::Immortal);
const TfToken _alternateColorSpaceMetadataKey("ColorSpace", TfToken::Immortal);

struct _PhysicalLightParams
{
    TfToken lightType;
    GfMatrix4d lightToWorld = GfMatrix4d(1.0);
    double metersPerUnit = 1.0;
    bool normalize = false;

    bool hasPhotometricPower = false;
    float photometricPower = 0.0f;

    bool hasPhotometricIlluminance = false;
    float photometricIlluminance = 0.0f;

    bool hasPhotometricIlluminanceDistance = false;
    float photometricIlluminanceDistance = 0.0f;

    float width = 1.0f;
    float height = 1.0f;
    float radius = 0.5f;
    float length = 1.0f;
    float angle = 0.53f;
};

bool
_IsFinite(double v)
{
    return std::isfinite(v);
}

float
_LinearRec709Luminance(GfVec3f const& color)
{
    return color[0] * 0.2126f + color[1] * 0.7152f + color[2] * 0.0722f;
}

float
_GetLuminance(GfColor const& color)
{
    GfColor xyzColor(color, _xyzColorSpace);
    return xyzColor.GetRGB()[1];
}

GfVec3f
_Rec709LuminanceComponents()
{
    static const GfVec3f components(
        _GetLuminance(GfColor(GfVec3f::XAxis(), _linearRec709ColorSpace)),
        _GetLuminance(GfColor(GfVec3f::YAxis(), _linearRec709ColorSpace)),
        _GetLuminance(GfColor(GfVec3f::ZAxis(), _linearRec709ColorSpace)));
    return components;
}

GfVec3f
_BlackbodyTemperatureAsRgb(float kelvinColorTemp)
{
    GfColor tempColor(_linearRec709ColorSpace);
    tempColor.SetFromPlanckianLocus(kelvinColorTemp, 1.0f);

    const GfVec3f tempColorRGB = tempColor.GetRGB();
    const float rec709Luminance = GfDot(
        tempColorRGB, _Rec709LuminanceComponents());
    return rec709Luminance > 0.0f
        ? tempColorRGB / rec709Luminance
        : GfVec3f(1.0f);
}

VtValue
_GetLightParamValue(
    HdSceneDelegate* sceneDelegate,
    SdfPath const& id,
    TfToken const& paramName)
{
    return sceneDelegate
        ? sceneDelegate->GetLightParamValue(id, paramName)
        : VtValue();
}

bool
_GetFloat(VtValue const& value, float* out)
{
    if (value.IsHolding<float>()) {
        *out = value.UncheckedGet<float>();
        return true;
    }
    if (value.IsHolding<double>()) {
        *out = static_cast<float>(value.UncheckedGet<double>());
        return true;
    }
    if (value.IsHolding<int>()) {
        *out = static_cast<float>(value.UncheckedGet<int>());
        return true;
    }
    return false;
}

float
_GetFloatParam(
    HdSceneDelegate* sceneDelegate,
    SdfPath const& id,
    TfToken const& paramName,
    float fallback)
{
    float value = fallback;
    _GetFloat(_GetLightParamValue(sceneDelegate, id, paramName), &value);
    return value;
}

bool
_GetBoolParam(
    HdSceneDelegate* sceneDelegate,
    SdfPath const& id,
    TfToken const& paramName,
    bool fallback)
{
    const VtValue value = _GetLightParamValue(sceneDelegate, id, paramName);
    return value.IsHolding<bool>() ? value.UncheckedGet<bool>() : fallback;
}

GfVec3f
_GetVec3fParam(
    HdSceneDelegate* sceneDelegate,
    SdfPath const& id,
    TfToken const& paramName,
    GfVec3f const& fallback)
{
    const VtValue value = _GetLightParamValue(sceneDelegate, id, paramName);
    if (value.IsHolding<GfVec3f>()) {
        return value.UncheckedGet<GfVec3f>();
    }
    if (value.IsHolding<GfVec3d>()) {
        return GfVec3f(value.UncheckedGet<GfVec3d>());
    }
    return fallback;
}

double
_GetMetersPerUnit(HdSceneDelegate* sceneDelegate, SdfPath const& id)
{
    const VtValue value = _GetLightParamValue(
        sceneDelegate, id, HdLightTokens->metersPerUnit);
    double metersPerUnit = 1.0;
    if (value.IsHolding<double>()) {
        metersPerUnit = value.UncheckedGet<double>();
    } else if (value.IsHolding<float>()) {
        metersPerUnit = value.UncheckedGet<float>();
    }
    return metersPerUnit > 0.0 && _IsFinite(metersPerUnit)
        ? metersPerUnit
        : 1.0;
}

std::string
_GetAssetPathFromValue(VtValue const& value)
{
    if (value.IsHolding<SdfAssetPath>()) {
        const SdfAssetPath& assetPath = value.UncheckedGet<SdfAssetPath>();
        const std::string& resolvedPath = assetPath.GetResolvedPath();
        return !resolvedPath.empty() ? resolvedPath : assetPath.GetAssetPath();
    }
    if (value.IsHolding<std::string>()) {
        return value.UncheckedGet<std::string>();
    }
    return std::string();
}

std::string
_GetTexturePathFromMaterialNode(HdMaterialNode2 const& node)
{
    auto it = node.parameters.find(HdLightTokens->textureFile);
    if (it == node.parameters.end()) {
        return std::string();
    }
    return _GetAssetPathFromValue(it->second);
}

std::string
_GetTexturePathFromMaterialNetwork(HdMaterialNetwork2 const& network)
{
    const auto terminalIt = network.terminals.find(
        HdMaterialTerminalTokens->light);
    if (terminalIt != network.terminals.end()) {
        const auto nodeIt = network.nodes.find(terminalIt->second.upstreamNode);
        if (nodeIt != network.nodes.end()) {
            if (std::string path =
                    _GetTexturePathFromMaterialNode(nodeIt->second);
                !path.empty()) {
                return path;
            }
        }
    }

    for (const auto& nodeEntry : network.nodes) {
        if (std::string path =
                _GetTexturePathFromMaterialNode(nodeEntry.second);
            !path.empty()) {
            return path;
        }
    }

    return std::string();
}

std::string
_GetTexturePathFromMaterialResource(
    HdSceneDelegate* sceneDelegate,
    SdfPath const& id)
{
    if (!sceneDelegate) {
        return std::string();
    }

    VtValue materialResource;
    try {
        materialResource = sceneDelegate->GetMaterialResource(id);
    } catch (...) {
        return std::string();
    }

    if (materialResource.IsHolding<HdMaterialNetwork2>()) {
        return _GetTexturePathFromMaterialNetwork(
            materialResource.UncheckedGet<HdMaterialNetwork2>());
    }
    if (materialResource.IsHolding<HdMaterialNetworkMap>()) {
        return _GetTexturePathFromMaterialNetwork(
            HdConvertToHdMaterialNetwork2(
                materialResource.UncheckedGet<HdMaterialNetworkMap>()));
    }

    return std::string();
}

std::string
_GetDomeTexturePath(HdSceneDelegate* sceneDelegate, SdfPath const& id)
{
    std::string path = _GetAssetPathFromValue(_GetLightParamValue(
        sceneDelegate, id, HdLightTokens->textureFile));
    if (path.empty()) {
        path = _GetTexturePathFromMaterialResource(sceneDelegate, id);
    }
    return path;
}

GfMatrix4d
_GetLightToWorld(HdSceneDelegate* sceneDelegate, SdfPath const& id)
{
    return sceneDelegate ? sceneDelegate->GetTransform(id) : GfMatrix4d(1.0);
}

_PhysicalLightParams
_GetPhysicalLightParams(
    HdSceneDelegate* sceneDelegate,
    SdfPath const& id,
    TfToken const& lightType)
{
    _PhysicalLightParams params;
    params.lightType = lightType;
    params.lightToWorld = _GetLightToWorld(sceneDelegate, id);
    params.metersPerUnit = _GetMetersPerUnit(sceneDelegate, id);
    params.normalize = _GetBoolParam(
        sceneDelegate, id, HdLightTokens->normalize, false);

    VtValue value = _GetLightParamValue(
        sceneDelegate, id, HdLightTokens->photometricPower);
    params.hasPhotometricPower = _GetFloat(value, &params.photometricPower);

    value = _GetLightParamValue(
        sceneDelegate, id, HdLightTokens->photometricIlluminance);
    params.hasPhotometricIlluminance = _GetFloat(
        value, &params.photometricIlluminance);

    value = _GetLightParamValue(
        sceneDelegate, id, HdLightTokens->photometricIlluminanceDistance);
    params.hasPhotometricIlluminanceDistance = _GetFloat(
        value, &params.photometricIlluminanceDistance);

    params.width = _GetFloatParam(
        sceneDelegate, id, HdLightTokens->width, params.width);
    params.height = _GetFloatParam(
        sceneDelegate, id, HdLightTokens->height, params.height);
    params.radius = _GetFloatParam(
        sceneDelegate, id, HdLightTokens->radius, params.radius);
    params.length = _GetFloatParam(
        sceneDelegate, id, HdLightTokens->length, params.length);
    params.angle = _GetFloatParam(
        sceneDelegate, id, HdLightTokens->angle, params.angle);

    return params;
}

double
_MetersPerUnit(_PhysicalLightParams const& params)
{
    return params.metersPerUnit > 0.0 && _IsFinite(params.metersPerUnit)
        ? params.metersPerUnit
        : 1.0;
}

GfVec3d
_TransformDir(GfMatrix4d const& xf, GfVec3d const& dir)
{
    return xf.TransformDir(dir);
}

double
_AreaRect(_PhysicalLightParams const& params)
{
    const GfVec3d u = _TransformDir(params.lightToWorld,
        GfVec3d(params.width, 0.0, 0.0));
    const GfVec3d v = _TransformDir(params.lightToWorld,
        GfVec3d(0.0, params.height, 0.0));
    return GfCross(u, v).GetLength();
}

double
_AreaDisk(_PhysicalLightParams const& params)
{
    const double r = std::max<double>(_MinLightSize, params.radius);
    const double a = _TransformDir(params.lightToWorld, GfVec3d(r, 0.0, 0.0))
        .GetLength();
    const double b = _TransformDir(params.lightToWorld, GfVec3d(0.0, r, 0.0))
        .GetLength();
    return _Pi * a * b;
}

double
_AreaSphere(_PhysicalLightParams const& params)
{
    const double r = std::max<double>(_MinLightSize, params.radius);
    const double a = _TransformDir(params.lightToWorld, GfVec3d(r, 0.0, 0.0))
        .GetLength();
    const double b = _TransformDir(params.lightToWorld, GfVec3d(0.0, r, 0.0))
        .GetLength();
    const double c = _TransformDir(params.lightToWorld, GfVec3d(0.0, 0.0, r))
        .GetLength();
    const double ab = std::pow(a * b, 1.6);
    const double ac = std::pow(a * c, 1.6);
    const double bc = std::pow(b * c, 1.6);
    return std::pow((ab + ac + bc) / 3.0, 1.0 / 1.6) * 4.0 * _Pi;
}

double
_AreaCylinder(_PhysicalLightParams const& params)
{
    const double r = std::max<double>(_MinLightSize, params.radius);
    const double len = std::max<double>(_MinLightSize, params.length);
    const double c = _TransformDir(params.lightToWorld, GfVec3d(len, 0.0, 0.0))
        .GetLength();
    const double a = _TransformDir(params.lightToWorld, GfVec3d(0.0, r, 0.0))
        .GetLength();
    const double b = _TransformDir(params.lightToWorld, GfVec3d(0.0, 0.0, r))
        .GetLength();
    const double perimeter = _Pi *
        (3.0 * (a + b) - std::sqrt((3.0 * a + b) * (a + 3.0 * b)));
    return perimeter * c;
}

double
_DistantHalfAngle(_PhysicalLightParams const& params)
{
    const double angle = _IsFinite(params.angle)
        ? std::clamp<double>(params.angle, 0.0, 360.0)
        : 0.0;
    return std::clamp(0.5 * GfDegreesToRadians(angle), 0.0, _Pi);
}

double
_DistantMeasure(_PhysicalLightParams const& params)
{
    const double theta = _DistantHalfAngle(params);
    if (theta <= 0.0) {
        return 1.0;
    }

    const double sinTheta = std::sin(theta);
    const double sin2 = sinTheta * sinTheta;
    if (params.hasPhotometricIlluminance &&
        params.photometricIlluminance > 0.0f &&
        std::cos(theta) < 0.0) {
        return _Pi;
    }
    return _Pi * ((theta <= 0.5 * _Pi) ? sin2 : (2.0 - sin2));
}

double
_DistantNormalizeMeasure(_PhysicalLightParams const& params)
{
    const double theta = _DistantHalfAngle(params);
    if (theta <= 0.0) {
        return 1.0;
    }

    const double sinTheta = std::sin(theta);
    const double sin2 = sinTheta * sinTheta;
    return _Pi * ((theta <= 0.5 * _Pi) ? sin2 : (2.0 - sin2));
}

double
_LightMeasure(_PhysicalLightParams const& params)
{
    if (params.lightType == HdSprimTypeTokens->rectLight) {
        return _AreaRect(params);
    }
    if (params.lightType == HdSprimTypeTokens->diskLight) {
        return _AreaDisk(params);
    }
    if (params.lightType == HdSprimTypeTokens->sphereLight) {
        return _AreaSphere(params);
    }
    if (params.lightType == HdSprimTypeTokens->cylinderLight) {
        return _AreaCylinder(params);
    }
    if (params.lightType == HdSprimTypeTokens->distantLight) {
        return _DistantMeasure(params);
    }
    return 1.0;
}

bool
_IsAreaLight(TfToken const& lightType)
{
    return lightType == HdSprimTypeTokens->rectLight ||
           lightType == HdSprimTypeTokens->diskLight ||
           lightType == HdSprimTypeTokens->sphereLight ||
           lightType == HdSprimTypeTokens->cylinderLight;
}

double
_AreaGeometricNormalizer(_PhysicalLightParams const& params)
{
    const double mpu2 = _MetersPerUnit(params) * _MetersPerUnit(params);
    return params.normalize ? mpu2 : _LightMeasure(params) * mpu2;
}

double
_DistantGeometricNormalizer(_PhysicalLightParams const& params)
{
    const double physicalMeasure = _DistantMeasure(params);
    if (!params.normalize) {
        return physicalMeasure;
    }

    // A renderer applies the standard UsdLux distant-light normalization
    // after this scale. Compensate here when the physical illuminance measure
    // differs, which occurs for cones extending behind the receiving plane.
    const double normalizeMeasure = _DistantNormalizeMeasure(params);
    return normalizeMeasure > 0.0
        ? physicalMeasure / normalizeMeasure
        : 0.0;
}

double
_ProjectDiskSolidAngle(_PhysicalLightParams const& params)
{
    const double mpu = _MetersPerUnit(params);
    const double rx = _TransformDir(params.lightToWorld,
        GfVec3d(params.radius, 0.0, 0.0)).GetLength() * mpu;
    const double ry = _TransformDir(params.lightToWorld,
        GfVec3d(0.0, params.radius, 0.0)).GetLength() * mpu;
    const double r = std::max({rx, ry, _MinLightSize * mpu});
    const double d = std::max(0.0f, params.photometricIlluminanceDistance);
    return _Pi * ((r * r) / (d * d + r * r));
}

double
_ProjectSphereSolidAngle(_PhysicalLightParams const& params)
{
    const double mpu = _MetersPerUnit(params);
    const double rx = _TransformDir(params.lightToWorld,
        GfVec3d(params.radius, 0.0, 0.0)).GetLength() * mpu;
    const double ry = _TransformDir(params.lightToWorld,
        GfVec3d(0.0, params.radius, 0.0)).GetLength() * mpu;
    const double rz = _TransformDir(params.lightToWorld,
        GfVec3d(0.0, 0.0, params.radius)).GetLength() * mpu;
    const double r = std::max({rx, ry, rz, _MinLightSize * mpu});
    const double d = std::max<double>(params.photometricIlluminanceDistance, r);
    return _Pi * ((r * r) / (d * d));
}

double
_ProjectRectSolidAngle(_PhysicalLightParams const& params)
{
    const double mpu = _MetersPerUnit(params);
    const double halfX = std::max(_MinLightSize,
        _TransformDir(params.lightToWorld, GfVec3d(params.width * 0.5, 0.0, 0.0))
            .GetLength()) * mpu;
    const double halfY = std::max(_MinLightSize,
        _TransformDir(params.lightToWorld, GfVec3d(0.0, params.height * 0.5, 0.0))
            .GetLength()) * mpu;
    const double d = std::max(0.0f, params.photometricIlluminanceDistance);

    const double term1Denom = std::sqrt(d * d + halfX * halfX);
    const double term2Denom = std::sqrt(d * d + halfY * halfY);
    const double term1 = (2.0 * halfX / term1Denom) *
        std::atan(halfY / term1Denom);
    const double term2 = (2.0 * halfY / term2Denom) *
        std::atan(halfX / term2Denom);
    return term1 + term2;
}

double
_ProjectCylinderSolidAngle(_PhysicalLightParams const& params)
{
    const double mpu = _MetersPerUnit(params);
    const double radius = std::max(_MinLightSize,
        _TransformDir(params.lightToWorld, GfVec3d(0.0, params.radius, 0.0))
            .GetLength()) * mpu;
    const double length = std::max(_MinLightSize,
        _TransformDir(params.lightToWorld, GfVec3d(params.length, 0.0, 0.0))
            .GetLength()) * mpu;
    const double d = std::max(0.0f, params.photometricIlluminanceDistance);

    constexpr int xSamples = 64;
    constexpr int phiSamples = 64;
    const double dx = length / double(xSamples);
    const double dPhi = 2.0 * _Pi / double(phiSamples);
    double result = 0.0;

    for (int ix = 0; ix < xSamples; ++ix) {
        const double x = -0.5 * length + (double(ix) + 0.5) * dx;
        for (int ip = 0; ip < phiSamples; ++ip) {
            const double phi = (double(ip) + 0.5) * dPhi;
            const double sinPhi = std::sin(phi);
            const double cosPhi = std::cos(phi);
            const double sy = radius * sinPhi;
            const double sz = radius * cosPhi;
            const double vx = x;
            const double vy = sy;
            const double vz = sz - d;
            const double r2 = vx * vx + vy * vy + vz * vz;
            const double r = std::sqrt(r2);
            if (r <= 1e-12) {
                continue;
            }
            const double cosThetaPatch = (-vz) / r;
            if (cosThetaPatch <= 0.0) {
                continue;
            }
            const double cosThetaSample = std::max(
                0.0, sinPhi * (-vy / r) + cosPhi * (-vz / r));
            const double dA = radius * dPhi * dx;
            result += dA * (cosThetaSample / r2) * cosThetaPatch;
        }
    }
    return result;
}

double
_ProjectedSolidAngle(_PhysicalLightParams const& params)
{
    if (params.lightType == HdSprimTypeTokens->rectLight) {
        return _ProjectRectSolidAngle(params);
    }
    if (params.lightType == HdSprimTypeTokens->diskLight) {
        return _ProjectDiskSolidAngle(params);
    }
    if (params.lightType == HdSprimTypeTokens->sphereLight) {
        return _ProjectSphereSolidAngle(params);
    }
    if (params.lightType == HdSprimTypeTokens->cylinderLight) {
        return _ProjectCylinderSolidAngle(params);
    }
    return 0.0;
}

std::string
_NormalizeColorSpaceName(std::string const& name)
{
    std::string normalized;
    normalized.reserve(name.size());
    for (const char c : name) {
        if (c == '-' || c == ' ' || c == ':') {
            normalized.push_back('_');
        } else {
            normalized.push_back(
                static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    return normalized;
}

bool
_ResolveColorSpaceName(std::string const& sourceColorSpace, TfToken* outColorSpaceName)
{
    if (!outColorSpaceName) {
        return false;
    }

    const std::string normalized = _NormalizeColorSpaceName(sourceColorSpace);
    if (normalized.empty() || normalized == "raw" || normalized == "identity" ||
        normalized == "data" || normalized == "unknown") {
        return false;
    }
    if (normalized == "srgb" || normalized == "srgb_texture") {
        *outColorSpaceName = GfColorSpaceNames->SRGBRec709;
        return true;
    }
    if (normalized == "lin_rec709") {
        *outColorSpaceName = GfColorSpaceNames->LinearRec709;
        return true;
    }
    if (normalized == "gamma22" || normalized == "g22_rec709") {
        *outColorSpaceName = GfColorSpaceNames->G22Rec709;
        return true;
    }
    if (normalized == "gamma18" || normalized == "g18_rec709") {
        *outColorSpaceName = GfColorSpaceNames->G18Rec709;
        return true;
    }
    if (normalized == "acescg") {
        *outColorSpaceName = GfColorSpaceNames->LinearAP1;
        return true;
    }
    if (normalized == "g22_ap1") {
        *outColorSpaceName = GfColorSpaceNames->G22AP1;
        return true;
    }
    if (normalized == "adobergb") {
        *outColorSpaceName = GfColorSpaceNames->G22AdobeRGB;
        return true;
    }
    if (normalized == "lin_adobergb") {
        *outColorSpaceName = GfColorSpaceNames->LinearAdobeRGB;
        return true;
    }
    if (normalized == "srgb_displayp3") {
        *outColorSpaceName = GfColorSpaceNames->SRGBP3D65;
        return true;
    }
    if (normalized == "lin_displayp3") {
        *outColorSpaceName = GfColorSpaceNames->LinearP3D65;
        return true;
    }

    const TfToken directToken(normalized);
    if (GfColorSpace::IsValid(directToken)) {
        *outColorSpaceName = directToken;
        return true;
    }

    return false;
}

TfToken
_GetImageColorSpaceName(HioImageSharedPtr const& image)
{
    if (!image) {
        return GfColorSpaceNames->LinearRec709;
    }

    std::string metadataColorSpace;
    if ((image->GetMetadata(_colorSpaceMetadataKey, &metadataColorSpace) ||
         image->GetMetadata(_alternateColorSpaceMetadataKey, &metadataColorSpace)) &&
        !metadataColorSpace.empty()) {
        TfToken resolvedColorSpaceName;
        if (_ResolveColorSpaceName(metadataColorSpace, &resolvedColorSpaceName)) {
            return resolvedColorSpaceName;
        }
    }

    if (image->IsColorSpaceSRGB()) {
        return GfColorSpaceNames->SRGBRec709;
    }

    return GfColorSpaceNames->LinearRec709;
}

float
_GetTextureLuminance(GfVec3f const& rgb, TfToken const& colorSpaceName)
{
    if (colorSpaceName.IsEmpty() ||
        colorSpaceName == GfColorSpaceNames->LinearRec709) {
        return _LinearRec709Luminance(rgb);
    }

    const GfColorSpace sourceColorSpace(colorSpaceName);
    const GfColor xyz = _linearXyzD65ColorSpace.Convert(sourceColorSpace, rgb);
    return xyz.GetRGB()[1];
}

float
_SanitizeWeight(float value)
{
    if (!std::isfinite(value) || value < 0.0f) {
        return 0.0f;
    }
    return value;
}

bool
_ReadDomeTextureIlluminance(std::string const& path, float* outIlluminance)
{
    if (!outIlluminance || path.empty()) {
        return false;
    }

    HioImageSharedPtr image = HioImage::OpenForReading(path);
    if (!image) {
        return false;
    }

    const int width = image->GetWidth();
    const int height = image->GetHeight();
    if (width <= 0 || height <= 0) {
        return false;
    }

    std::vector<GfVec3f> pixels(static_cast<size_t>(width) * height);

    HioImage::StorageSpec storage;
    storage.width = width;
    storage.height = height;
    storage.depth = 1;
    storage.format = HioFormatFloat32Vec3;
    storage.data = pixels.data();

    if (!image->Read(storage)) {
        TF_WARN("Could not read image %s", path.c_str());
        return false;
    }

    const TfToken colorSpaceName = _GetImageColorSpaceName(image);
    const double dPhi = 2.0 * _Pi / static_cast<double>(width);
    double illuminance = 0.0;

    for (int y = 0; y < height; ++y) {
        const double theta0 =
            _Pi * (static_cast<double>(y) / static_cast<double>(height));
        const double theta1 =
            _Pi * ((static_cast<double>(y) + 1.0) / static_cast<double>(height));
        const double thetaCos0 = std::min(theta0, 0.5 * _Pi);
        const double thetaCos1 = std::min(theta1, 0.5 * _Pi);
        const double rowCosineMeasure = thetaCos1 > thetaCos0
            ? 0.5 * (std::sin(thetaCos1) * std::sin(thetaCos1) -
                     std::sin(thetaCos0) * std::sin(thetaCos0)) * dPhi
            : 0.0;
        if (rowCosineMeasure <= 0.0) {
            continue;
        }

        for (int x = 0; x < width; ++x) {
            const size_t idx = static_cast<size_t>(y) * width + x;
            const float luminance = _SanitizeWeight(
                _GetTextureLuminance(pixels[idx], colorSpaceName));
            illuminance += static_cast<double>(luminance) * rowCosineMeasure;
        }
    }

    *outIlluminance = static_cast<float>(illuminance);
    return true;
}

} // anonymous namespace

HdLight::HdLight(SdfPath const &id)
 : HdSprim(id)
 , _instancerId()
{
}

HdLight::~HdLight() = default;

void
HdLight::_UpdateInstancer(
    HdSceneDelegate* delegate,
    HdDirtyBits* dirtyBits)
{
    if (HdChangeTracker::IsInstancerDirty(*dirtyBits, GetId())) {
        const SdfPath& instancerId = delegate->GetInstancerId(GetId());
        if (instancerId == _instancerId) {
            return;
        }
        HdChangeTracker& tracker = delegate->GetRenderIndex().GetChangeTracker();
        if (!_instancerId.IsEmpty()) {
            tracker.RemoveInstancerSprimDependency(_instancerId, GetId());
        }
        if (!instancerId.IsEmpty()) {
            tracker.AddInstancerSprimDependency(instancerId, GetId());
        }
        _instancerId = instancerId;
    }
}

/* static */
float
HdLight::EmissionLuminanceFactor(
    HdSceneDelegate* sceneDelegate,
    SdfPath const& id,
    TfToken const& lightType)
{
    const bool isDistant = lightType == HdSprimTypeTokens->distantLight;
    GfVec3f emission = _GetVec3fParam(
        sceneDelegate, id, HdLightTokens->color, GfVec3f(1.0f));
    emission *= _GetFloatParam(
        sceneDelegate, id, HdLightTokens->intensity,
        isDistant ? 50000.0f : 1.0f);
    emission *= _GetFloatParam(
        sceneDelegate, id, HdLightTokens->diffuse, 1.0f);
    emission *= std::pow(2.0f, _GetFloatParam(
        sceneDelegate, id, HdLightTokens->exposure, 0.0f));

    if (_GetBoolParam(
            sceneDelegate, id, HdLightTokens->enableColorTemperature, false)) {
        emission = GfCompMult(emission, _BlackbodyTemperatureAsRgb(
            _GetFloatParam(
                sceneDelegate, id, HdLightTokens->colorTemperature, 6500.0f)));
    }

    const float luminance = GfDot(emission, _Rec709LuminanceComponents());
    return luminance > 0.0f ? 1.0f / luminance : 0.0f;
}

/* static */
float
HdLight::AreaLightPowerFactor(
    HdSceneDelegate* sceneDelegate,
    SdfPath const& id,
    TfToken const& lightType)
{
    const _PhysicalLightParams params = _GetPhysicalLightParams(
        sceneDelegate, id, lightType);
    const double geometricNormalizer = _AreaGeometricNormalizer(params);
    const double denominator = _Pi * geometricNormalizer;
    return denominator > 0.0 ? static_cast<float>(1.0 / denominator) : 0.0f;
}

/* static */
float
HdLight::AreaLightIlluminanceFactor(
    HdSceneDelegate* sceneDelegate,
    SdfPath const& id,
    TfToken const& lightType)
{
    const _PhysicalLightParams params = _GetPhysicalLightParams(
        sceneDelegate, id, lightType);
    const double projectedSolidAngle = _ProjectedSolidAngle(params);
    if (projectedSolidAngle <= 0.0) {
        return 0.0f;
    }

    double factor = 1.0 / projectedSolidAngle;
    if (params.normalize) {
        factor *= _LightMeasure(params);
    }
    return static_cast<float>(factor);
}

/* static */
float
HdLight::DistantLightIlluminanceFactor(
    HdSceneDelegate* sceneDelegate,
    SdfPath const& id)
{
    const _PhysicalLightParams params = _GetPhysicalLightParams(
        sceneDelegate, id, HdSprimTypeTokens->distantLight);
    const double geometricNormalizer = _DistantGeometricNormalizer(params);
    return geometricNormalizer > 0.0
        ? static_cast<float>(1.0 / geometricNormalizer)
        : 0.0f;
}

/* static */
float
HdLight::DomeTextureIlluminanceFactor(
    HdSceneDelegate* sceneDelegate,
    SdfPath const& id)
{
    const std::string texturePath = _GetDomeTexturePath(sceneDelegate, id);

    float textureIlluminance = 0.0f;
    if (!texturePath.empty() &&
        _ReadDomeTextureIlluminance(texturePath, &textureIlluminance) &&
        textureIlluminance > 0.0f) {
        return 1.0f / textureIlluminance;
    }

    return static_cast<float>(1.0 / _Pi);
}

/* static */
float
HdLight::IesPowerFactor(HdSceneDelegate* sceneDelegate, SdfPath const& id)
{
    const std::string iesPath = _GetAssetPathFromValue(_GetLightParamValue(
        sceneDelegate, id, HdLightTokens->shapingIesFile));
    if (iesPath.empty()) {
        return 1.0f;
    }

    std::ifstream in(iesPath);
    if (!in.is_open()) {
        TF_WARN("could not open ies file %s", iesPath.c_str());
        return 1.0f;
    }

    std::stringstream buffer;
    buffer << in.rdbuf();

    HdLightIesProfile profile;
    if (!profile.Load(buffer.str()) || !profile.IsValid()) {
        TF_WARN("could not load ies file %s", iesPath.c_str());
        return 1.0f;
    }

    const float profilePower = profile.GetPower();
    if (profilePower <= 0.0f || !std::isfinite(profilePower)) {
        return 1.0f;
    }

    const bool iesNormalize = _GetBoolParam(
        sceneDelegate, id, HdLightTokens->shapingIesNormalize, false);
    return iesNormalize ? 1.0f : (1.0f / profilePower);
}

/* static */
float
HdLight::ComputePhysicalScalingFactor(
    HdSceneDelegate* sceneDelegate,
    SdfPath const& id,
    TfToken const& lightType)
{
    const _PhysicalLightParams params = _GetPhysicalLightParams(
        sceneDelegate, id, lightType);

    const bool hasPhotometricInput = params.hasPhotometricPower ||
        params.hasPhotometricIlluminance ||
        params.hasPhotometricIlluminanceDistance;
    if (!hasPhotometricInput) {
        return 1.0f;
    }

    if (_IsAreaLight(lightType)) {
        const bool powerIsZero = params.hasPhotometricPower &&
            params.photometricPower == 0.0f;
        const bool illuminanceIsZero = params.hasPhotometricIlluminance &&
            params.photometricIlluminance == 0.0f;
        if (powerIsZero && illuminanceIsZero) {
            return 0.0f;
        }

        if (params.hasPhotometricIlluminance &&
            params.hasPhotometricIlluminanceDistance &&
            params.photometricIlluminance > 0.0f &&
            params.photometricIlluminanceDistance > 0.0f) {
            float scale = params.photometricIlluminance;
            scale *= EmissionLuminanceFactor(sceneDelegate, id, lightType);
            scale *= AreaLightIlluminanceFactor(sceneDelegate, id, lightType);
            return scale;
        }

        if (params.hasPhotometricPower && params.photometricPower > 0.0f) {
            float scale = params.photometricPower;
            scale *= EmissionLuminanceFactor(sceneDelegate, id, lightType);
            scale *= AreaLightPowerFactor(sceneDelegate, id, lightType);
            scale *= IesPowerFactor(sceneDelegate, id);
            return scale;
        }

        return (powerIsZero || illuminanceIsZero) ? 0.0f : 1.0f;
    }

    if (lightType == HdSprimTypeTokens->distantLight) {
        if (!params.hasPhotometricIlluminance) {
            return 1.0f;
        }
        if (params.photometricIlluminance == 0.0f) {
            return 0.0f;
        }
        if (params.photometricIlluminance > 0.0f) {
            float scale = params.photometricIlluminance;
            scale *= EmissionLuminanceFactor(sceneDelegate, id, lightType);
            scale *= DistantLightIlluminanceFactor(sceneDelegate, id);
            return scale;
        }
        return 1.0f;
    }

    if (lightType == HdSprimTypeTokens->domeLight) {
        if (!params.hasPhotometricIlluminance) {
            return 1.0f;
        }
        if (params.photometricIlluminance == 0.0f) {
            return 0.0f;
        }
        if (params.photometricIlluminance > 0.0f) {
            float scale = params.photometricIlluminance;
            scale *= EmissionLuminanceFactor(sceneDelegate, id, lightType);
            scale *= DomeTextureIlluminanceFactor(sceneDelegate, id);
            return scale;
        }
        return 1.0f;
    }

    return 1.0f;
}

/* static */
std::string
HdLight::StringifyDirtyBits(HdDirtyBits dirtyBits) {
    if (dirtyBits == DirtyBits::Clean) {
        return std::string("Clean");
    }
    std::stringstream ss;
    if (dirtyBits & DirtyTransform) {
        ss << "Transform ";
    }
    if (dirtyBits & DirtyParams) {
        ss << "Params ";
    }
    if (dirtyBits & DirtyShadowParams) {
        ss << "ShadowParams ";
    }
    if (dirtyBits & DirtyCollection) {
        ss << "Collection ";
    }
    if (dirtyBits & DirtyResource) {
        ss << "Resource ";
    }
    if (dirtyBits & DirtyInstancer) {
        ss << "Instancer ";
    }
    return TfStringTrimRight(ss.str());
}

PXR_NAMESPACE_CLOSE_SCOPE
