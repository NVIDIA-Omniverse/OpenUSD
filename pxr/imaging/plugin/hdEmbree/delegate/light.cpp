//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/imaging/plugin/hdEmbree/delegate/light.h"

#include "light.h"
#include "pxr/imaging/plugin/hdEmbree/renderer/debugCodes.h"
#include "pxr/imaging/plugin/hdEmbree/delegate/renderParam.h"
#include "pxr/imaging/plugin/hdEmbree/renderer/renderer.h"

#include "pxr/base/gf/color.h"
#include "pxr/base/gf/colorSpace.h"
#include "pxr/base/gf/math.h"
#include "pxr/imaging/hd/material.h"
#include "pxr/imaging/hd/sceneDelegate.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/hio/image.h"

#include <embree4/rtcore_buffer.h>
#include <embree4/rtcore_scene.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <sstream>
#include <vector>

namespace {

PXR_NAMESPACE_USING_DIRECTIVE

constexpr float _pi = static_cast<float>(M_PI);

const GfColorSpace _xyzColorSpace(GfColorSpaceNames->LinearCIEXYZD65);
const TfToken _colorSpaceMetadataKey("oiio:ColorSpace");
const TfToken _alternateColorSpaceMetadataKey("ColorSpace");
const TfToken _visibleInPrimaryRayToken(
    "visibleInPrimaryRay", TfToken::Immortal);

float
_Smoothstep(float t, float edge0, float edge1)
{
    const float length = edge1 - edge0;
    if (length == 0.0f) {
        return (t <= edge0) ? 0.0f : 1.0f;
    }

    t = GfClamp((t - edge0) / length, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float
_Theta(GfVec3f const& v)
{
    return std::acos(GfClamp(v[2], -1.0f, 1.0f));
}

float
_Phi(GfVec3f const& v)
{
    const float p = std::atan2(v[1], v[0]);
    return p < 0.0f ? (p + 2.0f * _pi) : p;
}

bool
_HasAuthoredDirectionalShaping(HdEmbree_Shaping const& shaping)
{
    return shaping.focus > 0.0f ||
           shaping.coneAngle < 180.0f ||
           shaping.coneSoftness != 0.0f ||
           shaping.ies.iesFile.valid();
}

// Map an IES profile vertical angle (radians) to the eval-space theta where
// it lands after the angleScale remap in PxrIESFile::eval.
float
_IesKnotToEvalTheta(float angle, float angleScale)
{
    if (angleScale > 0.0f) {
        return angle * angleScale;
    }
    if (angleScale < 0.0f) {
        return _pi - angleScale * (angle - _pi);
    }
    return angle;
}

std::string
_NormalizeColorSpaceName(const std::string& name)
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
_ResolveColorSpaceName(const std::string& sourceColorSpace, TfToken* outColorSpaceName)
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
_GetImageColorSpaceName(const HioImageSharedPtr& image)
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
_GetLuminance(const GfVec3f& rgb, const TfToken& colorSpaceName)
{
    if (colorSpaceName.IsEmpty() ||
        colorSpaceName == GfColorSpaceNames->LinearRec709) {
        return 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2];
    }

    const GfColorSpace sourceColorSpace(colorSpaceName);
    const GfColor xyz = _xyzColorSpace.Convert(sourceColorSpace, rgb);
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

void
_ClearSamplingDistribution(HdEmbree_LightTexture* texture)
{
    if (!texture) {
        return;
    }
    texture->texelWeights.clear();
    texture->conditionalCdf.clear();
    texture->marginalCdf.clear();
    texture->weightSum = 0.0f;
}

HdEmbree_LightTexture
_LoadLightTexture(std::string const& path)
{
    if (path.empty()) {
        return HdEmbree_LightTexture();
    }

    HioImageSharedPtr img = HioImage::OpenForReading(path);
    if (!img) {
        return HdEmbree_LightTexture();
    }

    int width = img->GetWidth();
    int height = img->GetHeight();

    std::vector<GfVec3f> pixels(width * height);

    HioImage::StorageSpec storage;
    storage.width = width;
    storage.height = height;
    storage.depth = 1;
    storage.format = HioFormatFloat32Vec3;
    storage.data = &pixels.front();

    if (img->Read(storage)) {
        HdEmbree_LightTexture texture;
        texture.pixels = std::move(pixels);
        texture.width = width;
        texture.height = height;
        texture.colorSpaceName = _GetImageColorSpaceName(img);
        HdEmbreeBuildDomeLightSamplingDistribution(&texture);
        return texture;
    }
    TF_WARN("Could not read image %s", path.c_str());
    return { std::vector<GfVec3f>(), 0, 0 };
}

bool
_IsFinitePoint(GfVec3f const& p)
{
    return std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]);
}

bool
_IsPositiveFinite(float v)
{
    return std::isfinite(v) && v > 0.0f;
}

void
_AppendPoint(
    HdEmbree_LightData const& light,
    GfVec3f const& localPoint,
    std::vector<GfVec3f>* points)
{
    points->push_back(light.xformLightToWorld.Transform(localPoint));
}

void
_AppendRectVisibleGeometry(
    HdEmbree_LightData const& light,
    HdEmbree_Rect const& rect,
    std::vector<GfVec3f>* points,
    std::vector<GfVec3i>* triangles)
{
    if (!_IsPositiveFinite(rect.width) || !_IsPositiveFinite(rect.height)) {
        return;
    }

    const int base = static_cast<int>(points->size());
    const float halfWidth = 0.5f * rect.width;
    const float halfHeight = 0.5f * rect.height;
    _AppendPoint(light, GfVec3f(-halfWidth, -halfHeight, 0.0f), points);
    _AppendPoint(light, GfVec3f( halfWidth, -halfHeight, 0.0f), points);
    _AppendPoint(light, GfVec3f( halfWidth,  halfHeight, 0.0f), points);
    _AppendPoint(light, GfVec3f(-halfWidth,  halfHeight, 0.0f), points);

    // RectLight emits toward local -Z. Keep triangle winding consistent with
    // that normal, though Embree still intersects both sides for visibility.
    triangles->push_back(GfVec3i(base + 0, base + 2, base + 1));
    triangles->push_back(GfVec3i(base + 0, base + 3, base + 2));
}

void
_AppendDiskVisibleGeometry(
    HdEmbree_LightData const& light,
    HdEmbree_Disk const& disk,
    std::vector<GfVec3f>* points,
    std::vector<GfVec3i>* triangles)
{
    if (!_IsPositiveFinite(disk.radius)) {
        return;
    }

    constexpr int segments = 48;
    const int base = static_cast<int>(points->size());
    _AppendPoint(light, GfVec3f(0.0f), points);
    for (int i = 0; i < segments; ++i) {
        const float phi = 2.0f * _pi * static_cast<float>(i) /
            static_cast<float>(segments);
        _AppendPoint(
            light,
            GfVec3f(
                disk.radius * std::cos(phi),
                disk.radius * std::sin(phi),
                0.0f),
            points);
    }
    for (int i = 0; i < segments; ++i) {
        const int current = base + 1 + i;
        const int next = base + 1 + ((i + 1) % segments);
        triangles->push_back(GfVec3i(base, next, current));
    }
}

void
_AppendSphereVisibleGeometry(
    HdEmbree_LightData const& light,
    HdEmbree_Sphere const& sphere,
    std::vector<GfVec3f>* points,
    std::vector<GfVec3i>* triangles)
{
    if (!_IsPositiveFinite(sphere.radius)) {
        return;
    }

    constexpr int segments = 32;
    constexpr int rings = 16;
    const int base = static_cast<int>(points->size());
    _AppendPoint(light, GfVec3f(0.0f, 0.0f, sphere.radius), points);
    for (int ring = 1; ring < rings; ++ring) {
        const float theta = _pi * static_cast<float>(ring) /
            static_cast<float>(rings);
        const float sinTheta = std::sin(theta);
        const float cosTheta = std::cos(theta);
        for (int i = 0; i < segments; ++i) {
            const float phi = 2.0f * _pi * static_cast<float>(i) /
                static_cast<float>(segments);
            _AppendPoint(
                light,
                GfVec3f(
                    sphere.radius * sinTheta * std::cos(phi),
                    sphere.radius * sinTheta * std::sin(phi),
                    sphere.radius * cosTheta),
                points);
        }
    }
    const int bottom = static_cast<int>(points->size());
    _AppendPoint(light, GfVec3f(0.0f, 0.0f, -sphere.radius), points);

    for (int i = 0; i < segments; ++i) {
        const int next = (i + 1) % segments;
        triangles->push_back(GfVec3i(base, base + 1 + i, base + 1 + next));
    }
    for (int ring = 0; ring < rings - 2; ++ring) {
        const int row = base + 1 + ring * segments;
        const int nextRow = row + segments;
        for (int i = 0; i < segments; ++i) {
            const int next = (i + 1) % segments;
            triangles->push_back(GfVec3i(row + i, nextRow + i, nextRow + next));
            triangles->push_back(GfVec3i(row + i, nextRow + next, row + next));
        }
    }
    const int lastRow = base + 1 + (rings - 2) * segments;
    for (int i = 0; i < segments; ++i) {
        const int next = (i + 1) % segments;
        triangles->push_back(GfVec3i(bottom, lastRow + next, lastRow + i));
    }
}

void
_AppendCylinderVisibleGeometry(
    HdEmbree_LightData const& light,
    HdEmbree_Cylinder const& cylinder,
    std::vector<GfVec3f>* points,
    std::vector<GfVec3i>* triangles)
{
    if (!_IsPositiveFinite(cylinder.radius) ||
        !_IsPositiveFinite(cylinder.length)) {
        return;
    }

    constexpr int segments = 48;
    const int base = static_cast<int>(points->size());
    const float halfLength = 0.5f * cylinder.length;
    for (int i = 0; i < segments; ++i) {
        const float phi = 2.0f * _pi * static_cast<float>(i) /
            static_cast<float>(segments);
        const float y = cylinder.radius * std::cos(phi);
        const float z = cylinder.radius * std::sin(phi);
        _AppendPoint(light, GfVec3f(-halfLength, y, z), points);
        _AppendPoint(light, GfVec3f( halfLength, y, z), points);
    }
    for (int i = 0; i < segments; ++i) {
        const int next = (i + 1) % segments;
        const int a = base + 2 * i;
        const int b = base + 2 * i + 1;
        const int c = base + 2 * next + 1;
        const int d = base + 2 * next;
        triangles->push_back(GfVec3i(a, b, c));
        triangles->push_back(GfVec3i(a, c, d));
    }
}

bool
_BuildVisibleLightGeometry(
    HdEmbree_LightData const& light,
    std::vector<GfVec3f>* points,
    std::vector<GfVec3i>* triangles)
{
    points->clear();
    triangles->clear();

    std::visit([&](auto const& typedLight) {
        using T = std::decay_t<decltype(typedLight)>;
        if constexpr (std::is_same_v<T, HdEmbree_Rect>) {
            _AppendRectVisibleGeometry(light, typedLight, points, triangles);
        } else if constexpr (std::is_same_v<T, HdEmbree_Disk>) {
            _AppendDiskVisibleGeometry(light, typedLight, points, triangles);
        } else if constexpr (std::is_same_v<T, HdEmbree_Sphere>) {
            _AppendSphereVisibleGeometry(light, typedLight, points, triangles);
        } else if constexpr (std::is_same_v<T, HdEmbree_Cylinder>) {
            _AppendCylinderVisibleGeometry(light, typedLight, points, triangles);
        }
    }, light.lightVariant);

    if (points->empty() || triangles->empty()) {
        points->clear();
        triangles->clear();
        return false;
    }
    for (GfVec3f const& point : *points) {
        if (!_IsFinitePoint(point)) {
            points->clear();
            triangles->clear();
            return false;
        }
    }
    return true;
}

std::string
_ResolveAssetPath(const SdfAssetPath& assetPath)
{
    std::string path = assetPath.GetResolvedPath();
    if (path.empty()) {
        path = assetPath.GetAssetPath();
    }
    return path;
}

std::string
_GetTexturePathFromValue(const VtValue& value)
{
    if (value.IsHolding<SdfAssetPath>()) {
        return _ResolveAssetPath(value.UncheckedGet<SdfAssetPath>());
    }
    return std::string();
}

std::string
_GetTexturePathFromMaterialNode(const HdMaterialNode2& node)
{
    auto it = node.parameters.find(HdLightTokens->textureFile);
    if (it == node.parameters.end()) {
        return std::string();
    }
    return _GetTexturePathFromValue(it->second);
}

std::string
_GetTexturePathFromMaterialNetwork(const HdMaterialNetwork2& network)
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
_GetTexturePathFromMaterialResource(const SdfPath& id,
                                    HdSceneDelegate *sceneDelegate)
{
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

void
_SyncLightTexture(const SdfPath& id, HdEmbree_LightData& light,
                  HdSceneDelegate *sceneDelegate)
{
    std::string path = _GetTexturePathFromValue(
        sceneDelegate->GetLightParamValue(id, HdLightTokens->textureFile));
    if (path.empty()) {
        path = _GetTexturePathFromMaterialResource(id, sceneDelegate);
    }
    light.texture = _LoadLightTexture(path);
}

} // anonymous namespace

PXR_NAMESPACE_OPEN_SCOPE

GfVec3f
HdEmbreeEvaluateDirectionalShaping(
    HdEmbree_Shaping const& shaping,
    GfVec3f const& localDirection)
{
    if (localDirection.GetLengthSq() <= 0.0f ||
        !std::isfinite(localDirection[0]) ||
        !std::isfinite(localDirection[1]) ||
        !std::isfinite(localDirection[2])) {
        return GfVec3f(0.0f);
    }

    const GfVec3f omegaInLocal = localDirection.GetNormalized();
    const float cosThetaOffZ = GfClamp(omegaInLocal[2], -1.0f, 1.0f);
    GfVec3f shapingWeight(1.0f);

    if (shaping.focus > 0.0f) {
        const float ff = std::pow(
            std::abs(cosThetaOffZ), shaping.focus);
        const GfVec3f focusTint = GfLerp(
            ff, shaping.focusTint, GfVec3f(1.0f));
        shapingWeight = GfCompMult(shapingWeight, focusTint);
    }

    const float thetaCone = GfDegreesToRadians(shaping.coneAngle);
    const float thetaSoft =
        GfLerp(shaping.coneSoftness, thetaCone, 0.0f);
    const float thetaOffZ = std::acos(cosThetaOffZ);
    shapingWeight *= 1.0f - _Smoothstep(thetaOffZ, thetaSoft, thetaCone);

    HdEmbree_IES const& ies = shaping.ies;
    if (ies.iesFile.valid()) {
        const float norm = ies.normalize ? ies.iesFile.power() : 1.0f;
        const float iesWeight =
            (norm > 0.0f)
                ? ies.iesFile.eval(_Theta(omegaInLocal), _Phi(omegaInLocal),
                                   ies.angleScale) /
                      norm
                : 0.0f;
        shapingWeight *= iesWeight;
    }

    return shapingWeight;
}

float
HdEmbreeDirectionalShapingImportance(
    HdEmbree_Shaping const& shaping,
    GfVec3f const& localDirection)
{
    const GfVec3f shapingWeight =
        HdEmbreeEvaluateDirectionalShaping(shaping, localDirection);
    // This is a proposal weight only.  Max-component importance avoids
    // embedding working-space-specific luminance coefficients in light sync.
    return std::max(
        0.0f,
        std::max(
            shapingWeight[0],
            std::max(shapingWeight[1], shapingWeight[2])));
}

void
HdEmbreeBuildDirectionalShapingDistribution(HdEmbree_Shaping* shaping)
{
    if (!shaping) {
        return;
    }

    HdEmbree_DirectionalShapingDistribution& distribution =
        shaping->directionalDistribution;
    distribution = HdEmbree_DirectionalShapingDistribution();

    if (!_HasAuthoredDirectionalShaping(*shaping)) {
        return;
    }

    constexpr int numPhi = HdEmbree_DirectionalShapingDistribution::NumPhi;
    constexpr int numBaseTheta =
        HdEmbree_DirectionalShapingDistribution::NumBaseTheta;

    // Theta row boundaries: a uniform base partition augmented with the
    // exact angles where each shaping feature has structure, so features
    // narrower than a base row (small cone angles, narrow IES beams) span
    // whole rows and cannot vanish when the cell weights are integrated.
    std::vector<float> thetaBounds;
    thetaBounds.reserve(static_cast<size_t>(numBaseTheta) + 3);
    for (int v = 0; v <= numBaseTheta; ++v) {
        thetaBounds.push_back(
            _pi * static_cast<float>(v) / static_cast<float>(numBaseTheta));
    }
    if (shaping->coneAngle < 180.0f) {
        const float thetaConeUnclamped = GfDegreesToRadians(shaping->coneAngle);
        const float thetaCone = GfClamp(thetaConeUnclamped, 0.0f, _pi);
        const float thetaSoft =
            GfLerp(shaping->coneSoftness, thetaCone, 0.0f);
        thetaBounds.push_back(thetaCone);
        thetaBounds.push_back(thetaSoft);
    }
    if (shaping->ies.iesFile.valid()) {
        for (const float angle : shaping->ies.iesFile.verticalAngles()) {
            thetaBounds.push_back(GfClamp(
                _IesKnotToEvalTheta(angle, shaping->ies.angleScale),
                0.0f, _pi));
        }
    }
    std::sort(thetaBounds.begin(), thetaBounds.end());

    // Merge near-coincident boundaries to keep rows non-degenerate.
    constexpr float thetaMergeEps = 1.0e-4f;
    std::vector<float> rowTheta;
    rowTheta.reserve(thetaBounds.size());
    for (const float theta : thetaBounds) {
        if (rowTheta.empty() || theta - rowTheta.back() > thetaMergeEps) {
            rowTheta.push_back(theta);
        }
    }
    rowTheta.front() = 0.0f;
    rowTheta.back() = _pi;

    const int numRows = static_cast<int>(rowTheta.size()) - 1;
    const int numCells = numRows * numPhi;

    distribution.rowCosThetaBounds.resize(static_cast<size_t>(numRows + 1));
    for (int v = 0; v <= numRows; ++v) {
        distribution.rowCosThetaBounds[static_cast<size_t>(v)] =
            std::cos(rowTheta[static_cast<size_t>(v)]);
    }
    distribution.rowCosThetaBounds.front() = 1.0f;
    distribution.rowCosThetaBounds.back() = -1.0f;

    std::vector<float> cellWeights(
        static_cast<size_t>(numCells), 0.0f);
    distribution.cellPdfSolidAngle.assign(static_cast<size_t>(numCells), 0.0f);
    distribution.cdf.assign(static_cast<size_t>(numCells + 1), 0.0f);

    // Integrate the importance over each cell with a midpoint rule rather
    // than evaluating a single point, so partially covered cells keep a
    // representative weight.
    constexpr int numSubZ = 4;
    constexpr int numSubPhi = 4;

    GfVec3f principal(0.0f);
    float weightSum = 0.0f;
    float peakWeight = 0.0f;

    for (int v = 0; v < numRows; ++v) {
        const float z0 = distribution.rowCosThetaBounds[static_cast<size_t>(v)];
        const float z1 =
            distribution.rowCosThetaBounds[static_cast<size_t>(v + 1)];
        const float cellSolidAngle =
            2.0f * _pi * (z0 - z1) / static_cast<float>(numPhi);
        if (cellSolidAngle <= 0.0f) {
            continue;
        }
        const float zCenter = 0.5f * (z0 + z1);
        const float rCenter =
            std::sqrt(std::max(0.0f, 1.0f - zCenter * zCenter));

        for (int h = 0; h < numPhi; ++h) {
            float importanceSum = 0.0f;
            for (int sz = 0; sz < numSubZ; ++sz) {
                const float z = GfLerp(
                    (static_cast<float>(sz) + 0.5f) /
                        static_cast<float>(numSubZ),
                    z0, z1);
                const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
                for (int sp = 0; sp < numSubPhi; ++sp) {
                    const float phi = 2.0f * _pi *
                        (static_cast<float>(h) +
                         (static_cast<float>(sp) + 0.5f) /
                             static_cast<float>(numSubPhi)) /
                        static_cast<float>(numPhi);
                    const float importance =
                        HdEmbreeDirectionalShapingImportance(
                            *shaping,
                            GfVec3f(r * std::cos(phi),
                                    r * std::sin(phi),
                                    z));
                    importanceSum += importance;
                    peakWeight = std::max(peakWeight, importance);
                }
            }
            const float weight = importanceSum * cellSolidAngle /
                static_cast<float>(numSubZ * numSubPhi);
            const int idx = v * numPhi + h;
            cellWeights[static_cast<size_t>(idx)] = weight;
            weightSum += weight;

            const float phiCenter = 2.0f * _pi *
                (static_cast<float>(h) + 0.5f) / static_cast<float>(numPhi);
            principal += GfVec3f(rCenter * std::cos(phiCenter),
                                 rCenter * std::sin(phiCenter),
                                 zCenter) * weight;
        }
    }

    if (weightSum <= 0.0f || !std::isfinite(weightSum)) {
        TF_DEBUG(HDEMBREE_LIGHT_CREATE).Msg(
            "Directional shaping importance integrated to zero; "
            "falling back to area sampling\n");
        distribution = HdEmbree_DirectionalShapingDistribution();
        return;
    }

    float cumulative = 0.0f;
    distribution.cdf[0] = 0.0f;
    for (int v = 0; v < numRows; ++v) {
        const float z0 = distribution.rowCosThetaBounds[static_cast<size_t>(v)];
        const float z1 =
            distribution.rowCosThetaBounds[static_cast<size_t>(v + 1)];
        const float cellSolidAngle =
            2.0f * _pi * (z0 - z1) / static_cast<float>(numPhi);
        for (int h = 0; h < numPhi; ++h) {
            const int idx = v * numPhi + h;
            cumulative += cellWeights[static_cast<size_t>(idx)] / weightSum;
            distribution.cdf[static_cast<size_t>(idx + 1)] = cumulative;
            distribution.cellPdfSolidAngle[static_cast<size_t>(idx)] =
                (cellSolidAngle > 0.0f)
                    ? cellWeights[static_cast<size_t>(idx)] /
                          (weightSum * cellSolidAngle)
                    : 0.0f;
        }
    }
    distribution.cdf.back() = 1.0f;
    distribution.weightSum = weightSum;
    distribution.totalSolidAngleWeightedIntensity = weightSum;
    distribution.averageWeight = weightSum / (4.0f * _pi);
    distribution.peakWeight = peakWeight;
    if (principal.GetLengthSq() > 0.0f && std::isfinite(principal[0]) &&
        std::isfinite(principal[1]) && std::isfinite(principal[2])) {
        distribution.principalDirection = principal.GetNormalized();
    }
}

HdEmbree_DirectionalShapingSample
HdEmbreeSampleDirectionalShaping(
    HdEmbree_Shaping const& shaping,
    float u1,
    float u2)
{
    HdEmbree_DirectionalShapingSample result;
    HdEmbree_DirectionalShapingDistribution const& distribution =
        shaping.directionalDistribution;
    if (!distribution.IsValid()) {
        return result;
    }

    constexpr int numPhi = HdEmbree_DirectionalShapingDistribution::NumPhi;
    const int numCells = distribution.NumRows() * numPhi;

    const float sample = GfClamp(
        u1, 0.0f, std::nextafter(1.0f, 0.0f));
    const auto cdfBegin = distribution.cdf.begin();
    const auto cdfIt = std::upper_bound(
        cdfBegin + 1, distribution.cdf.end(), sample);
    const int idx = std::clamp(
        static_cast<int>(cdfIt - (cdfBegin + 1)),
        0,
        numCells - 1);

    const float cdf0 = distribution.cdf[static_cast<size_t>(idx)];
    const float cdf1 = distribution.cdf[static_cast<size_t>(idx + 1)];
    const float cellU = (cdf1 > cdf0)
        ? ((sample - cdf0) / (cdf1 - cdf0))
        : 0.0f;

    const int v = idx / numPhi;
    const int h = idx - v * numPhi;
    const float z0 = distribution.rowCosThetaBounds[static_cast<size_t>(v)];
    const float z1 =
        distribution.rowCosThetaBounds[static_cast<size_t>(v + 1)];
    float z = GfLerp(cellU, z0, z1);
    // Keep the sample strictly inside the row (bounds are descending, so
    // z1 < z <= z0 nominally): rounding in the lerp or renormalization in
    // the PDF lookup can otherwise re-bin a boundary sample into the
    // neighboring row, decorrelating the sampled and evaluated PDFs. Three
    // ulps cover the at-most-one-ulp renormalization drift.
    float zLo = z1;
    float zHi = z0;
    for (int i = 0; i < 3; ++i) {
        zLo = std::nextafter(zLo, z0);
        zHi = std::nextafter(zHi, z1);
    }
    if (zLo <= zHi) {
        z = GfClamp(z, zLo, zHi);
    }
    const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
    const float phi = 2.0f * _pi *
        (static_cast<float>(h) +
         GfClamp(u2, 0.0f, std::nextafter(1.0f, 0.0f))) /
        static_cast<float>(numPhi);

    result.localDirection = GfVec3f(
        r * std::cos(phi),
        r * std::sin(phi),
        z);
    // Report the PDF through the same lookup MIS uses instead of reading
    // cellPdfSolidAngle[idx] directly: float rounding can land the sampled z
    // exactly on a row boundary, where the lookup resolves to the neighboring
    // row. Sharing one source of truth keeps sample and evaluation consistent
    // (a sample that rounds into a zero-weight row is simply discarded).
    result.pdfSolidAngle =
        HdEmbreeDirectionalShapingPdf(shaping, result.localDirection);
    result.importance =
        HdEmbreeDirectionalShapingImportance(shaping, result.localDirection);
    result.valid =
        result.pdfSolidAngle > 0.0f && std::isfinite(result.pdfSolidAngle);
    return result;
}

float
HdEmbreeDirectionalShapingPdf(
    HdEmbree_Shaping const& shaping,
    GfVec3f const& localDirection)
{
    HdEmbree_DirectionalShapingDistribution const& distribution =
        shaping.directionalDistribution;
    if (!distribution.IsValid() ||
        localDirection.GetLengthSq() <= 0.0f ||
        !std::isfinite(localDirection[0]) ||
        !std::isfinite(localDirection[1]) ||
        !std::isfinite(localDirection[2])) {
        return 0.0f;
    }

    constexpr int numPhi = HdEmbree_DirectionalShapingDistribution::NumPhi;
    const int numRows = distribution.NumRows();
    const GfVec3f omegaInLocal = localDirection.GetNormalized();
    const float z = GfClamp(omegaInLocal[2], -1.0f, 1.0f);
    const float phi = _Phi(omegaInLocal);
    // Row boundaries are descending in cos(theta); row v covers
    // (bounds[v + 1], bounds[v]].
    const auto& bounds = distribution.rowCosThetaBounds;
    const auto rowIt = std::upper_bound(
        bounds.begin(), bounds.end(), z, std::greater<float>());
    const int v = std::clamp(
        static_cast<int>(rowIt - bounds.begin()) - 1,
        0,
        numRows - 1);
    const int h = std::clamp(
        static_cast<int>(
            phi * static_cast<float>(numPhi) / (2.0f * _pi)),
        0,
        numPhi - 1);
    const int idx = v * numPhi + h;
    return distribution.cellPdfSolidAngle[static_cast<size_t>(idx)];
}

void
HdEmbreeBuildDomeLightSamplingDistribution(HdEmbree_LightTexture* texture)
{
    if (!texture) {
        return;
    }

    _ClearSamplingDistribution(texture);

    const int width = texture->width;
    const int height = texture->height;
    const size_t expectedTexelCount =
        static_cast<size_t>(width) * static_cast<size_t>(height);
    const TfToken colorSpaceName =
        texture->colorSpaceName.IsEmpty()
            ? GfColorSpaceNames->LinearRec709
            : texture->colorSpaceName;
    if (width <= 0 || height <= 0 ||
        texture->pixels.size() < expectedTexelCount) {
        return;
    }

    texture->texelWeights.resize(expectedTexelCount, 0.0f);
    texture->conditionalCdf.resize(
        static_cast<size_t>(height) * static_cast<size_t>(width + 1), 0.0f);
    texture->marginalCdf.resize(static_cast<size_t>(height + 1), 0.0f);

    float totalWeight = 0.0f;
    texture->marginalCdf[0] = 0.0f;

    for (int y = 0; y < height; ++y) {
        const float theta0 =
            _pi * (static_cast<float>(y) / static_cast<float>(height));
        const float theta1 =
            _pi * ((static_cast<float>(y) + 1.0f) / static_cast<float>(height));
        const float rowMeasure = std::max(
            0.0f, std::cos(theta0) - std::cos(theta1));

        float rowWeight = 0.0f;
        float* const rowCdf = texture->conditionalCdf.data() +
            static_cast<size_t>(y) * static_cast<size_t>(width + 1);
        rowCdf[0] = 0.0f;

        for (int x = 0; x < width; ++x) {
            const size_t idx =
                static_cast<size_t>(y) * static_cast<size_t>(width) + x;
            const float luminance =
                _SanitizeWeight(_GetLuminance(texture->pixels[idx], colorSpaceName));
            const float weight = luminance * rowMeasure;
            texture->texelWeights[idx] = weight;
            rowWeight += weight;
            rowCdf[x + 1] = rowWeight;
        }

        if (rowWeight <= 0.0f) {
            rowWeight = 0.0f;
            for (int x = 0; x < width; ++x) {
                const size_t idx =
                    static_cast<size_t>(y) * static_cast<size_t>(width) + x;
                texture->texelWeights[idx] = rowMeasure;
                rowWeight += rowMeasure;
                rowCdf[x + 1] = rowWeight;
            }
        }

        if (rowWeight > 0.0f) {
            const float invRowWeight = 1.0f / rowWeight;
            for (int x = 1; x <= width; ++x) {
                rowCdf[x] *= invRowWeight;
            }
            rowCdf[width] = 1.0f;
        }

        totalWeight += rowWeight;
        texture->marginalCdf[y + 1] = totalWeight;
    }

    if (totalWeight <= 0.0f) {
        _ClearSamplingDistribution(texture);
        return;
    }

    const float invTotalWeight = 1.0f / totalWeight;
    for (size_t i = 1; i < texture->marginalCdf.size(); ++i) {
        texture->marginalCdf[i] *= invTotalWeight;
    }
    texture->marginalCdf.back() = 1.0f;
    texture->weightSum = totalWeight;
}

HdEmbree_Light::HdEmbree_Light(SdfPath const& id, TfToken const& lightType)
    : HdLight(id) {
    if (id.IsEmpty()) {
        return;
    }

    TF_DEBUG(HDEMBREE_LIGHT_CREATE).Msg(
            "Creating light %s: %s\n", id.GetText(), lightType.GetText());

    // Set the variant to the right type - Sync will fill rest of data
    if (lightType == HdSprimTypeTokens->cylinderLight) {
        _lightData.lightVariant = HdEmbree_Cylinder();
    } else if (lightType == HdSprimTypeTokens->diskLight) {
        _lightData.lightVariant = HdEmbree_Disk();
    } else if (lightType == HdSprimTypeTokens->distantLight) {
        _lightData.lightVariant = HdEmbree_Distant();
    } else if (lightType == HdSprimTypeTokens->domeLight) {
        _lightData.lightVariant = HdEmbree_Dome();
    } else if (lightType == HdSprimTypeTokens->rectLight) {
        // Get shape parameters
        _lightData.lightVariant = HdEmbree_Rect();
    } else if (lightType == HdSprimTypeTokens->sphereLight) {
        _lightData.lightVariant = HdEmbree_Sphere();
    } else {
        TF_WARN("HdEmbree - Unrecognized light type: %s", lightType.GetText());
        _lightData.lightVariant = HdEmbree_UnknownLight();
    }
}

HdEmbree_Light::~HdEmbree_Light() = default;


void
HdEmbree_Light::_ReleaseVisibleGeometry(
    RTCScene scene, HdEmbreeRenderer* renderer)
{
    if (!_rtcVisibleGeometry) {
        _rtcVisibleGeometryId = RTC_INVALID_GEOMETRY_ID;
        _rtcVisiblePoints.clear();
        _rtcVisibleTriangles.clear();
        return;
    }

    if (renderer) {
        renderer->RemoveLightGeometry(_rtcVisibleGeometryId, &_lightData);
    }
    if (scene && _rtcVisibleGeometryId != RTC_INVALID_GEOMETRY_ID) {
        rtcDetachGeometry(scene, _rtcVisibleGeometryId);
    }
    rtcReleaseGeometry(_rtcVisibleGeometry);
    _rtcVisibleGeometry = nullptr;
    _rtcVisibleGeometryId = RTC_INVALID_GEOMETRY_ID;
    _rtcVisiblePoints.clear();
    _rtcVisibleTriangles.clear();
}

void
HdEmbree_Light::_UpdateVisibleGeometry(
    RTCScene scene, RTCDevice device, HdEmbreeRenderer* renderer)
{
    if (!scene || !device || !renderer || !_lightData.visible ||
        !_lightData.visibleInPrimaryRay || !IsFiniteLight()) {
        _ReleaseVisibleGeometry(scene, renderer);
        return;
    }

    std::vector<GfVec3f> points;
    std::vector<GfVec3i> triangles;
    if (!_BuildVisibleLightGeometry(_lightData, &points, &triangles)) {
        _ReleaseVisibleGeometry(scene, renderer);
        return;
    }

    const bool needsNewGeometry = !_rtcVisibleGeometry;
    if (needsNewGeometry) {
        _rtcVisibleGeometry = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_TRIANGLE);
        rtcSetGeometryBuildQuality(_rtcVisibleGeometry, RTC_BUILD_QUALITY_REFIT);
        rtcSetGeometryTimeStepCount(_rtcVisibleGeometry, 1);
        rtcSetGeometryMask(_rtcVisibleGeometry, HdEmbree_RayMask::All);
        _rtcVisibleGeometryId = rtcAttachGeometry(scene, _rtcVisibleGeometry);
        renderer->AddLightGeometry(_rtcVisibleGeometryId, &_lightData);
    }

    _rtcVisiblePoints = std::move(points);
    _rtcVisibleTriangles = std::move(triangles);

    rtcSetSharedGeometryBuffer(
        _rtcVisibleGeometry,
        RTC_BUFFER_TYPE_VERTEX,
        0,
        RTC_FORMAT_FLOAT3,
        _rtcVisiblePoints.data(),
        0,
        sizeof(GfVec3f),
        _rtcVisiblePoints.size());
    rtcSetSharedGeometryBuffer(
        _rtcVisibleGeometry,
        RTC_BUFFER_TYPE_INDEX,
        0,
        RTC_FORMAT_UINT3,
        _rtcVisibleTriangles.data(),
        0,
        sizeof(GfVec3i),
        _rtcVisibleTriangles.size());

    rtcEnableGeometry(_rtcVisibleGeometry);
    rtcCommitGeometry(_rtcVisibleGeometry);
}

void
HdEmbree_Light::Sync(HdSceneDelegate *sceneDelegate,
                         HdRenderParam *renderParam, HdDirtyBits *dirtyBits)
{
    HD_TRACE_FUNCTION();
    HF_MALLOC_TAG_FUNCTION();

    HdEmbreeRenderParam *embreeRenderParam =
        static_cast<HdEmbreeRenderParam*>(renderParam);

    // Calling this bumps the scene version and causes a re-render.
    RTCScene scene = embreeRenderParam->AcquireSceneForEdit();
    RTCDevice device = embreeRenderParam->GetEmbreeDevice();

    SdfPath const& id = GetId();
    const HdDirtyBits bits = *dirtyBits;

    if (bits & HdLight::DirtyTransform) {
        // We'll only consider the first time sample for now.
        HdTimeSampleArray<GfMatrix4d, 1> xformSamples;
        sceneDelegate->SampleTransform(id, &xformSamples);
        _lightData.xformLightToWorld = GfMatrix4f(xformSamples.values[0]);
        _lightData.xformWorldToLight =
            _lightData.xformLightToWorld.GetInverse();
        _lightData.normalXformLightToWorld =
            _lightData.xformWorldToLight.ExtractRotationMatrix().GetTranspose();
    }

    if (bits & (HdLight::DirtyParams | HdLight::DirtyResource)) {
        // Store luminance parameters
        const bool isDistant =
            std::holds_alternative<HdEmbree_Distant>(
                _lightData.lightVariant);
        _lightData.intensity = sceneDelegate->GetLightParamValue(
            id, HdLightTokens->intensity).GetWithDefault(
                isDistant ? 50000.0f : 1.0f);
        _lightData.diffuse = sceneDelegate->GetLightParamValue(
            id, HdLightTokens->diffuse).GetWithDefault(1.0f);
        _lightData.exposure = sceneDelegate->GetLightParamValue(
            id, HdLightTokens->exposure).GetWithDefault(0.0f);
        _lightData.color = sceneDelegate->GetLightParamValue(
            id, HdLightTokens->color).GetWithDefault(GfVec3f{1.0f, 1.0f, 1.0f});
        _lightData.normalize = sceneDelegate->GetLightParamValue(
            id, HdLightTokens->normalize).GetWithDefault(false);
        _lightData.colorTemperature = sceneDelegate->GetLightParamValue(
            id, HdLightTokens->colorTemperature).GetWithDefault(6500.0f);
        _lightData.enableColorTemperature = sceneDelegate->GetLightParamValue(
            id, HdLightTokens->enableColorTemperature).GetWithDefault(false);

        // Get visibility
        _lightData.visible = sceneDelegate->GetVisible(id);
        _lightData.visibleInPrimaryRay = sceneDelegate->GetLightParamValue(
            id, _visibleInPrimaryRayToken).GetWithDefault(false);
        // Switch on the _lightData type and pull the relevant attributes from
        // the scene delegate.
        std::visit([this, &id, &sceneDelegate](auto& typedLight) {
            using T = std::decay_t<decltype(typedLight)>;
            if constexpr (std::is_same_v<T, HdEmbree_Cylinder>) {
                typedLight = HdEmbree_Cylinder{
                    sceneDelegate->GetLightParamValue(id, HdLightTokens->radius)
                        .GetWithDefault(0.5f),
                    sceneDelegate->GetLightParamValue(id, HdLightTokens->length)
                        .GetWithDefault(1.0f),
                };
            } else if constexpr (std::is_same_v<T, HdEmbree_Disk>) {
                typedLight = HdEmbree_Disk{
                    sceneDelegate->GetLightParamValue(id, HdLightTokens->radius)
                        .GetWithDefault(0.5f),
                };
            } else if constexpr (std::is_same_v<T, HdEmbree_Distant>) {
                typedLight = HdEmbree_Distant{
                    sceneDelegate->GetLightParamValue(id, HdLightTokens->angle)
                        .GetWithDefault(0.53f),
                };
            } else if constexpr (std::is_same_v<T, HdEmbree_Dome>) {
                typedLight = HdEmbree_Dome{};
                _SyncLightTexture(id, _lightData, sceneDelegate);
            } else if constexpr (std::is_same_v<T, HdEmbree_Rect>) {
                typedLight = HdEmbree_Rect{
                    sceneDelegate->GetLightParamValue(id, HdLightTokens->width)
                        .Get<float>(),
                    sceneDelegate->GetLightParamValue(id, HdLightTokens->height)
                        .Get<float>(),
                };
                _SyncLightTexture(id, _lightData, sceneDelegate);
            } else if constexpr (std::is_same_v<T, HdEmbree_Sphere>) {
                typedLight = HdEmbree_Sphere{
                    sceneDelegate->GetLightParamValue(id, HdLightTokens->radius)
                        .GetWithDefault(0.5f),
                };
            } else if constexpr (std::is_same_v<T, HdEmbree_UnknownLight>) {
                // Do nothing...
            } else {
                // We should never get to this branch, as all possible variants
                // should be handled above, but as of summer 2025 gcc isn't
                // clever enough to prune the static assert if the if-statement
                // is exhaustive. As a workaround, we static assert on sizeof(T),
                // so that the assert only fires for concrete types we fail to
                // handle.
                static_assert(sizeof(T) == 0,
                        "non-exhaustive _LightVariant visitor");
            }
        }, _lightData.lightVariant);

        if (const auto value = sceneDelegate->GetLightParamValue(
                id, HdLightTokens->shapingFocus);
            value.IsHolding<float>()) {
            _lightData.shaping.focus = value.UncheckedGet<float>();
        }

        if (const auto value = sceneDelegate->GetLightParamValue(
                id, HdLightTokens->shapingFocusTint);
            value.IsHolding<GfVec3f>()) {
            _lightData.shaping.focusTint = value.UncheckedGet<GfVec3f>();
        }

        if (const auto value = sceneDelegate->GetLightParamValue(
                id, HdLightTokens->shapingConeAngle);
            value.IsHolding<float>()) {
            _lightData.shaping.coneAngle = value.UncheckedGet<float>();
        }

        if (const auto value = sceneDelegate->GetLightParamValue(
                id, HdLightTokens->shapingConeSoftness);
            value.IsHolding<float>()) {
            _lightData.shaping.coneSoftness = value.UncheckedGet<float>();
        }

        if (const auto value = sceneDelegate->GetLightParamValue(
                id, HdLightTokens->shapingIesFile);
            value.IsHolding<SdfAssetPath>()) {
            SdfAssetPath iesAssetPath = value.UncheckedGet<SdfAssetPath>();
            std::string iesPath = iesAssetPath.GetResolvedPath();
            if (iesPath.empty()) {
                iesPath = iesAssetPath.GetAssetPath();
            }

            if (!iesPath.empty()) {
                std::ifstream in(iesPath);
                if (!in.is_open()) {
                    TF_WARN("could not open ies file %s", iesPath.c_str());
                } else {
                    std::stringstream buffer;
                    buffer << in.rdbuf();

                    if (!_lightData.shaping.ies.iesFile.load(buffer.str())) {
                        TF_WARN("could not load ies file %s", iesPath.c_str());
                    }
                }
            }
        }

        if (const auto value = sceneDelegate->GetLightParamValue(
                id, HdLightTokens->shapingIesNormalize);
            value.IsHolding<bool>()) {
            _lightData.shaping.ies.normalize = value.UncheckedGet<bool>();
        }

        if (const auto value = sceneDelegate->GetLightParamValue(
                id, HdLightTokens->shapingIesAngleScale);
            value.IsHolding<float>()) {
            _lightData.shaping.ies.angleScale = value.UncheckedGet<float>();
        }

        HdEmbreeBuildDirectionalShapingDistribution(&_lightData.shaping);
    }

    if (bits & (HdLight::DirtyParams |
                HdLight::DirtyResource |
                HdLight::DirtyCollection)) {
        _lightData.lightLink = sceneDelegate->GetLightParamValue(
            id, HdTokens->lightLink).GetWithDefault(TfToken());
        _lightData.shadowLink = sceneDelegate->GetLightParamValue(
            id, HdTokens->shadowLink).GetWithDefault(TfToken());
    }

    HdEmbreeRenderer *renderer = embreeRenderParam->GetRenderer();
    _UpdateVisibleGeometry(scene, device, renderer);
    renderer->AddLight(id, &_lightData);

    *dirtyBits &= ~HdLight::AllDirty;
}

HdDirtyBits
HdEmbree_Light::GetInitialDirtyBitsMask() const
{
    return HdLight::AllDirty;
}

void
HdEmbree_Light::Finalize(HdRenderParam *renderParam)
{
    auto* embreeParam = static_cast<HdEmbreeRenderParam*>(renderParam);

    RTCScene scene = embreeParam->AcquireSceneForEdit();
    HdEmbreeRenderer *renderer = embreeParam->GetRenderer();
    _ReleaseVisibleGeometry(scene, renderer);

    // Remove from renderer's light map.
    renderer->RemoveLight(GetId(), &_lightData);
}

PXR_NAMESPACE_CLOSE_SCOPE
