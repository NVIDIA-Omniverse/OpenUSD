//
// hdEmbree OpenImageIO-backed texture system for MaterialXCpp.
//
#include "pxr/imaging/plugin/hdEmbree/oiioTextureSystem.h"

#include "pxr/base/gf/colorSpace.h"
#include "pxr/base/tf/diagnostic.h"
#include "pxr/base/tf/token.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <mutex>
#include <unordered_set>

#if defined(PXR_OIIO_PLUGIN_ENABLED)
#include <OpenImageIO/texture.h>
#include <OpenImageIO/ustring.h>
#endif

PXR_NAMESPACE_OPEN_SCOPE

struct HdEmbreeOiioTextureSystem::_Impl
{
#if defined(PXR_OIIO_PLUGIN_ENABLED)
    OIIO::TextureSystem* textureSystem = nullptr;
#endif
    mutable std::mutex warningMutex;
    mutable std::unordered_set<std::string> warnedFiles;
};

namespace {

mxcpp::Texture2DResult
_MakeDefaultResult(const mxcpp::Texture2DRequest& request,
                   const mxcpp::TextureSampleStatus status)
{
    mxcpp::Texture2DResult result;
    result.value = request.defaultValue;
    result.status = status;
    return result;
}

bool
_ShouldWarnOnce(HdEmbreeOiioTextureSystem::_Impl* const impl,
                const std::string& key)
{
    if (impl == nullptr || key.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(impl->warningMutex);
    return impl->warnedFiles.insert(key).second;
}

std::string
_NormalizeColorSpaceName(const std::string& name)
{
    std::string normalized = name;
    std::transform(
        normalized.begin(), normalized.end(), normalized.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return normalized;
}

bool
_ShouldApplyColorTransform(const mxcpp::Texture2DRequest& request)
{
    return request.dataRole == mxcpp::TextureDataRole::Color &&
           request.channelCount >= 3 &&
           !request.sourceColorSpace.empty();
}

enum class _ColorSpaceResolution {
    NoTransform,
    Transform,
    Unsupported
};

_ColorSpaceResolution
_ResolveColorSpace(
    const std::string& sourceColorSpace,
    TfToken* outColorSpaceName)
{
    if (!outColorSpaceName) {
        return _ColorSpaceResolution::Unsupported;
    }

    const std::string normalized = _NormalizeColorSpaceName(sourceColorSpace);
    if (normalized.empty() ||
        normalized == "none" ||
        normalized == "raw" ||
        normalized == "data" ||
        normalized == "auto" ||
        normalized == "identity") {
        return _ColorSpaceResolution::NoTransform;
    }

    if (normalized == "srgb_texture" || normalized == "srgb") {
        *outColorSpaceName = GfColorSpaceNames->SRGBRec709;
        return _ColorSpaceResolution::Transform;
    }
    if (normalized == "lin_rec709" || normalized == "lin_srgb") {
        *outColorSpaceName = GfColorSpaceNames->LinearRec709;
        return _ColorSpaceResolution::Transform;
    }
    if (normalized == "g22_rec709") {
        *outColorSpaceName = GfColorSpaceNames->G22Rec709;
        return _ColorSpaceResolution::Transform;
    }
    if (normalized == "g18_rec709") {
        *outColorSpaceName = GfColorSpaceNames->G18Rec709;
        return _ColorSpaceResolution::Transform;
    }
    if (normalized == "acescg" || normalized == "lin_ap1") {
        *outColorSpaceName = GfColorSpaceNames->LinearAP1;
        return _ColorSpaceResolution::Transform;
    }
    if (normalized == "g22_ap1") {
        *outColorSpaceName = GfColorSpaceNames->G22AP1;
        return _ColorSpaceResolution::Transform;
    }
    if (normalized == "adobergb") {
        *outColorSpaceName = GfColorSpaceNames->G22AdobeRGB;
        return _ColorSpaceResolution::Transform;
    }
    if (normalized == "lin_adobergb") {
        *outColorSpaceName = GfColorSpaceNames->LinearAdobeRGB;
        return _ColorSpaceResolution::Transform;
    }
    if (normalized == "srgb_displayp3") {
        *outColorSpaceName = GfColorSpaceNames->SRGBP3D65;
        return _ColorSpaceResolution::Transform;
    }
    if (normalized == "lin_displayp3") {
        *outColorSpaceName = GfColorSpaceNames->LinearP3D65;
        return _ColorSpaceResolution::Transform;
    }

    const TfToken directToken(normalized);
    if (GfColorSpace::IsValid(directToken)) {
        *outColorSpaceName = directToken;
        return _ColorSpaceResolution::Transform;
    }

    return _ColorSpaceResolution::Unsupported;
}

void
_ApplyColorTransform(
    const mxcpp::Texture2DRequest& request,
    float* sampled,
    HdEmbreeOiioTextureSystem::_Impl* impl)
{
    if (!_ShouldApplyColorTransform(request) || !sampled) {
        return;
    }

    TfToken sourceColorSpaceName;
    const _ColorSpaceResolution resolution =
        _ResolveColorSpace(request.sourceColorSpace, &sourceColorSpaceName);
    if (resolution == _ColorSpaceResolution::NoTransform) {
        return;
    }
    if (resolution == _ColorSpaceResolution::Unsupported) {
        const std::string warningKey =
            request.filePath + "|" + request.sourceColorSpace;
        if (_ShouldWarnOnce(impl, warningKey)) {
            TF_WARN(
                "Unsupported MaterialX texture color space '%s' for '%s'. "
                "Leaving sampled values unchanged.",
                request.sourceColorSpace.c_str(),
                request.filePath.c_str());
        }
        return;
    }

    if (sourceColorSpaceName == GfColorSpaceNames->LinearRec709) {
        return;
    }

    std::array<float, 3> rgb = {sampled[0], sampled[1], sampled[2]};
    const GfColorSpace srcColorSpace(sourceColorSpaceName);
    const GfColorSpace dstColorSpace(GfColorSpaceNames->LinearRec709);
    srcColorSpace.ConvertRGBSpan(
        dstColorSpace, TfSpan<float>(rgb.data(), rgb.size()));
    sampled[0] = rgb[0];
    sampled[1] = rgb[1];
    sampled[2] = rgb[2];
}

#if defined(PXR_OIIO_PLUGIN_ENABLED)

OIIO::TextureOpt::Wrap
_ToOiioWrap(const mxcpp::TextureAddressMode mode)
{
    switch (mode) {
    case mxcpp::TextureAddressMode::Constant:
        return OIIO::TextureOpt::WrapBlack;
    case mxcpp::TextureAddressMode::Clamp:
        return OIIO::TextureOpt::WrapClamp;
    case mxcpp::TextureAddressMode::Periodic:
        return OIIO::TextureOpt::WrapPeriodic;
    case mxcpp::TextureAddressMode::Mirror:
        return OIIO::TextureOpt::WrapMirror;
    case mxcpp::TextureAddressMode::UseMetadata:
        return OIIO::TextureOpt::WrapDefault;
    }

    return OIIO::TextureOpt::WrapDefault;
}

OIIO::TextureOpt::InterpMode
_ToOiioInterp(const mxcpp::TextureFilterType filterType)
{
    switch (filterType) {
    case mxcpp::TextureFilterType::Closest:
        return OIIO::TextureOpt::InterpClosest;
    case mxcpp::TextureFilterType::Linear:
        return OIIO::TextureOpt::InterpBilinear;
    case mxcpp::TextureFilterType::Cubic:
        return OIIO::TextureOpt::InterpBicubic;
    }

    return OIIO::TextureOpt::InterpSmartBicubic;
}

#endif

}  // namespace

HdEmbreeOiioTextureSystem::HdEmbreeOiioTextureSystem()
    : _impl(std::make_unique<_Impl>())
{
#if defined(PXR_OIIO_PLUGIN_ENABLED)
    _impl->textureSystem = OIIO::TextureSystem::create(/* shared = */ true);
    if (_impl->textureSystem) {
        _impl->textureSystem->attribute("automip", 1);
        _impl->textureSystem->attribute("autotile", 64);
        _impl->textureSystem->attribute("accept_untiled", 1);
        _impl->textureSystem->attribute("accept_unmipped", 1);
        _impl->textureSystem->attribute("gray_to_rgb", 1);
    }
#endif
}

HdEmbreeOiioTextureSystem::~HdEmbreeOiioTextureSystem()
{
#if defined(PXR_OIIO_PLUGIN_ENABLED)
    if (_impl && _impl->textureSystem) {
        OIIO::TextureSystem::destroy(_impl->textureSystem);
        _impl->textureSystem = nullptr;
    }
#endif
}

mxcpp::Texture2DResult
HdEmbreeOiioTextureSystem::Sample2D(
    const mxcpp::Texture2DRequest& request) const
{
    if (request.filePath.empty()) {
        return _MakeDefaultResult(request, mxcpp::TextureSampleStatus::Missing);
    }

#if !defined(PXR_OIIO_PLUGIN_ENABLED)
    if (_ShouldWarnOnce(_impl.get(), request.filePath)) {
        TF_WARN(
            "OpenImageIO-backed texture sampling requested for '%s', but "
            "hdEmbree was built without PXR_OIIO_PLUGIN_ENABLED. Returning "
            "the authored default value.",
            request.filePath.c_str());
    }
    return _MakeDefaultResult(request, mxcpp::TextureSampleStatus::Error);
#else
    if (!_impl || !_impl->textureSystem) {
        if (_ShouldWarnOnce(_impl.get(), request.filePath)) {
            TF_WARN(
                "OpenImageIO texture system is unavailable for '%s'. "
                "Returning the authored default value.",
                request.filePath.c_str());
        }
        return _MakeDefaultResult(request, mxcpp::TextureSampleStatus::Error);
    }

    OIIO::TextureOpt options;
    options.swrap = _ToOiioWrap(request.uAddressMode);
    options.twrap = _ToOiioWrap(request.vAddressMode);
    options.interpmode = _ToOiioInterp(request.filterType);
    options.mipmode = OIIO::TextureOpt::MipModeDefault;
    options.time = request.frame;
    options.fill = request.channelFillValue;

    const float missingColor[4] = {
        request.defaultValue[0],
        request.defaultValue[1],
        request.defaultValue[2],
        request.defaultValue[3],
    };
    options.missingcolor = missingColor;

    if (!request.layerName.empty()) {
        options.subimagename = OIIO::ustring(request.layerName);
    }

    float sampled[4] = {
        request.defaultValue[0],
        request.defaultValue[1],
        request.defaultValue[2],
        request.defaultValue[3],
    };

    // MaterialX graph coordinates use a lower-left origin, while the OIIO
    // image-space T axis increases from top to bottom.  Convert values and
    // derivatives together at the texture backend boundary.
    const float s = request.st[0];
    const float t = 1.0f - request.st[1];
    const float dsdx = request.dstdx[0];
    const float dtdx = -request.dstdx[1];
    const float dsdy = request.dstdy[0];
    const float dtdy = -request.dstdy[1];

    OIIO::TextureSystem::Perthread* const threadInfo =
        _impl->textureSystem->get_perthread_info();
    OIIO::TextureSystem::TextureHandle* const handle =
        _impl->textureSystem->get_texture_handle(
            OIIO::ustring(request.filePath), threadInfo);

    const bool ok =
        handle && _impl->textureSystem->good(handle) &&
        _impl->textureSystem->texture(
            handle,
            threadInfo,
            options,
            s,
            t,
            dsdx,
            dtdx,
            dsdy,
            dtdy,
            request.channelCount,
            sampled);

    if (!ok) {
        const std::string error = _impl->textureSystem->geterror();
        if (_ShouldWarnOnce(_impl.get(), request.filePath)) {
            if (error.empty()) {
                TF_WARN(
                    "Failed to sample texture '%s'. Returning the authored "
                    "default value.",
                    request.filePath.c_str());
            } else {
                TF_WARN(
                    "Failed to sample texture '%s': %s. Returning the "
                    "authored default value.",
                    request.filePath.c_str(),
                    error.c_str());
            }
        }
        return _MakeDefaultResult(request, mxcpp::TextureSampleStatus::Error);
    }

    _ApplyColorTransform(request, sampled, _impl.get());

    mxcpp::Texture2DResult result;
    result.value = mxcpp::Vec4f(sampled[0], sampled[1], sampled[2], sampled[3]);
    result.status = mxcpp::TextureSampleStatus::Ok;
    return result;
#endif
}

PXR_NAMESPACE_CLOSE_SCOPE
