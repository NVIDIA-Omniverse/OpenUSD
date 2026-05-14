//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/imaging/plugin/hdEmbree/light.h"

#include "light.h"
#include "pxr/imaging/plugin/hdEmbree/debugCodes.h"
#include "pxr/imaging/plugin/hdEmbree/renderParam.h"
#include "pxr/imaging/plugin/hdEmbree/renderer.h"

#include "pxr/base/gf/color.h"
#include "pxr/base/gf/colorSpace.h"
#include "pxr/imaging/hd/sceneDelegate.h"
#include "pxr/imaging/hio/image.h"

#include <embree4/rtcore_buffer.h>
#include <embree4/rtcore_scene.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <vector>

namespace {

PXR_NAMESPACE_USING_DIRECTIVE

constexpr float _pi = static_cast<float>(M_PI);

const GfColorSpace _xyzColorSpace(GfColorSpaceNames->LinearCIEXYZD65);
const TfToken _colorSpaceMetadataKey("oiio:ColorSpace");
const TfToken _alternateColorSpaceMetadataKey("ColorSpace");

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

void
_SyncLightTexture(const SdfPath& id, HdEmbree_LightData& light,
                  HdSceneDelegate *sceneDelegate)
{
    std::string path;
    if (VtValue textureValue = sceneDelegate->GetLightParamValue(
            id, HdLightTokens->textureFile);
        textureValue.IsHolding<SdfAssetPath>()) {
        SdfAssetPath texturePath =
            textureValue.UncheckedGet<SdfAssetPath>();
        path = texturePath.GetResolvedPath();
        if (path.empty()) {
            path = texturePath.GetAssetPath();
        }
    }
    light.texture = _LoadLightTexture(path);
}

} // anonymous namespace

PXR_NAMESPACE_OPEN_SCOPE

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
HdEmbree_Light::Sync(HdSceneDelegate *sceneDelegate,
                         HdRenderParam *renderParam, HdDirtyBits *dirtyBits)
{
    HD_TRACE_FUNCTION();
    HF_MALLOC_TAG_FUNCTION();

    HdEmbreeRenderParam *embreeRenderParam =
        static_cast<HdEmbreeRenderParam*>(renderParam);

    // calling this bumps the scene version and causes a re-render
    embreeRenderParam->AcquireSceneForEdit();

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
        _lightData.intensity = sceneDelegate->GetLightParamValue(
            id, HdLightTokens->intensity).GetWithDefault(1.0f);
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
            } else if constexpr (std::is_same_v<T, HdEmbree_Distant>) {
                typedLight = HdEmbree_Distant{
                    float(GfDegreesToRadians(
                        sceneDelegate->GetLightParamValue(id, HdLightTokens->angle)
                            .GetWithDefault(0.53f) / 2.0f)),
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
    }

    HdEmbreeRenderer *renderer = embreeRenderParam->GetRenderer();
    renderer->AddLight(id, this);

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

    // Remove from renderer's light map
    HdEmbreeRenderer *renderer = embreeParam->GetRenderer();
    renderer->RemoveLight(GetId(), this);
}

PXR_NAMESPACE_CLOSE_SCOPE
