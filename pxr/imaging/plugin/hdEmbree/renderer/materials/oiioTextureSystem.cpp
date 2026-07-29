//
// hdEmbree OpenImageIO-backed texture system for MaterialXCpp.
//
#include "oiioTextureSystem.h"

#include <renderer/renderSettings.h>

#include "pxr/base/tf/diagnostic.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <exception>
#include <mutex>
#include <string>
#include <unordered_set>

#if defined(PXR_OIIO_PLUGIN_ENABLED)
#include <OpenImageIO/texture.h>
#include <OpenImageIO/ustring.h>
#include <tbb/concurrent_unordered_map.h>
#endif

PXR_NAMESPACE_OPEN_SCOPE

struct HdEmbreeOiioTextureSystem::_Impl
{
#if defined(PXR_OIIO_PLUGIN_ENABLED)
    OIIO::TextureSystem* textureSystem = nullptr;
    OIIO::TextureSystem* pngTextureSystem = nullptr;

    // Resolving an OIIO texture handle requires constructing a ustring (a
    // globally locked table insert plus a hash of the whole path) and calling
    // get_texture_handle (another locked map lookup).  Both are stable for the
    // lifetime of the texture system, but they were previously repeated on
    // every single texture tap across every render thread, so their shared
    // locks serialized the workers and dominated textured-render cost.  Cache
    // the resolved handle per file path in a lock-free concurrent map so the
    // hot path performs one wait-free lookup instead.  A given file path
    // always selects the same underlying texture system (PNG inputs go to
    // pngTextureSystem, everything else to textureSystem), so a single
    // path-keyed map stays consistent across both.
    struct _CachedHandle
    {
        OIIO::ustring filename;
        OIIO::TextureSystem::TextureHandle* handle = nullptr;
        bool isUdim = false;
    };
    mutable tbb::concurrent_unordered_map<std::string, _CachedHandle>
        handleCache;

    const _CachedHandle& ResolveHandle(
        OIIO::TextureSystem* const system,
        OIIO::TextureSystem::Perthread* const threadInfo,
        const std::string& filePath) const
    {
        const auto it = handleCache.find(filePath);
        if (it != handleCache.end()) {
            return it->second;
        }

        _CachedHandle entry;
        entry.filename = OIIO::ustring(filePath);
        entry.handle = system->get_texture_handle(entry.filename, threadInfo);
        entry.isUdim = entry.handle && system->is_udim(entry.handle);

        // A concurrent insert of the same key from another thread simply
        // resolves to the same stable handle; the losing entry is discarded.
        // Node references into the map stay valid because entries are never
        // erased.
        return handleCache.insert({filePath, entry}).first->second;
    }
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

bool
_ShouldApplyColorTransform(const mxcpp::Texture2DRequest& request)
{
    return request.dataRole == mxcpp::TextureDataRole::Color &&
           request.channelCount >= 3 &&
           !request.sourceColorSpace.empty();
}

void
_ApplyColorTransform(
    const mxcpp::Texture2DRequest& request,
    float* sampled,
    HdEmbreeOiioTextureSystem::_Impl* impl,
    HdEmbreeRenderColorSpace renderColorSpace)
{
    if (!_ShouldApplyColorTransform(request) || !sampled) {
        return;
    }

    GfVec3f rgb(sampled[0], sampled[1], sampled[2]);
    if (!HdEmbreeConvertToRenderColorSpace(
            request.sourceColorSpace, renderColorSpace, &rgb)) {
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
    case mxcpp::TextureFilterType::SmartBicubic:
        return OIIO::TextureOpt::InterpSmartBicubic;
    }

    return OIIO::TextureOpt::InterpSmartBicubic;
}

float
_LocalUdimCoord(const float coord)
{
    return coord - std::floor(coord);
}

void
_ConfigureTextureSystem(
    OIIO::TextureSystem* const textureSystem,
    const bool preserveUnassociatedAlpha)
{
    if (!textureSystem) {
        return;
    }

    textureSystem->attribute("automip", 1);
    // Untiled inputs (JPEG, PNG, ...) are cached in autotile-sized blocks.
    // The OIIO default of 64 produces ~1024 tiles for a 2048^2 image, so
    // path-traced taps that scatter across the surface cross tile
    // boundaries constantly and each crossing costs a locked tile-cache
    // lookup -- that dominated textured-render cost (measured ~35% of a
    // single-sphere frame).  A larger block keeps far fewer, coarser tiles
    // resident, which is the right trade for a path tracer that ends up
    // touching most of every visible texture.  The value only affects
    // caching granularity, not filtering, so output is bit-identical.
    // Pre-generated tiled, mipped .tx inputs avoid this path entirely and
    // remain the fastest option for texture-heavy scenes.
    textureSystem->attribute("autotile", 512);
    // Never close texture files that are still referenced.  ALab-scale UDIM
    // sets reference hundreds of tiles; with OIIO's default cap of 100 open
    // files, the cache constantly reopens EXRs, which is a locked path.  A
    // value of 0 means "unlimited" -- the real ceiling is the OS descriptor
    // limit (ulimit -n), which is the intended place to govern it.
    textureSystem->attribute("max_open_files", 0);
    textureSystem->attribute("accept_untiled", 1);
    textureSystem->attribute("accept_unmipped", 1);
    textureSystem->attribute("gray_to_rgb", 1);
    textureSystem->attribute("unassociatedalpha",
                             preserveUnassociatedAlpha ? 1 : 0);
}

bool
_HasPngExtension(const std::string& filePath)
{
    if (filePath.empty()) {
        return false;
    }

    size_t pathEnd = filePath.find_first_of("?#");
    if (pathEnd == std::string::npos) {
        pathEnd = filePath.size();
    }
    if (pathEnd < 4) {
        return false;
    }

    const size_t dot = filePath.find_last_of('.', pathEnd - 1);
    const size_t slash = filePath.find_last_of("/\\", pathEnd - 1);
    if (dot == std::string::npos ||
        (slash != std::string::npos && dot < slash) ||
        pathEnd - dot != 4) {
        return false;
    }

    return std::tolower(static_cast<unsigned char>(filePath[dot + 1])) == 'p' &&
           std::tolower(static_cast<unsigned char>(filePath[dot + 2])) == 'n' &&
           std::tolower(static_cast<unsigned char>(filePath[dot + 3])) == 'g';
}

OIIO::TextureSystem*
_SelectTextureSystem(
    HdEmbreeOiioTextureSystem::_Impl* const impl,
    const std::string& filePath)
{
    if (impl && impl->pngTextureSystem && _HasPngExtension(filePath)) {
        return impl->pngTextureSystem;
    }
    return impl ? impl->textureSystem : nullptr;
}

#endif

}  // namespace

HdEmbreeOiioTextureSystem::HdEmbreeOiioTextureSystem()
    : _impl(std::make_unique<_Impl>())
{
#if defined(PXR_OIIO_PLUGIN_ENABLED)
    _impl->textureSystem = OIIO::TextureSystem::create(/* shared = */ true);
    _ConfigureTextureSystem(
        _impl->textureSystem, /* preserveUnassociatedAlpha = */ false);

    // OIIO exposes unassociated-alpha handling at the TextureSystem/ImageCache
    // level rather than per texture lookup.  Use a separate non-shared system
    // so only PNG inputs bypass OIIO's default automatic premultiplication.
    _impl->pngTextureSystem = OIIO::TextureSystem::create(/* shared = */ false);
    _ConfigureTextureSystem(
        _impl->pngTextureSystem, /* preserveUnassociatedAlpha = */ true);

    // Size the tile cache before the first render-setting sync.
    SetCacheSizeMB(HdEmbreeRenderSettings{}.textureCacheSizeMB);
#endif
}

void
HdEmbreeOiioTextureSystem::SetCacheSizeMB(int sizeMB)
{
#if defined(PXR_OIIO_PLUGIN_ENABLED)
    const float mb = static_cast<float>(std::max(1, sizeMB));
    if (_impl->textureSystem) {
        _impl->textureSystem->attribute("max_memory_MB", mb);
    }
    if (_impl->pngTextureSystem) {
        _impl->pngTextureSystem->attribute("max_memory_MB", mb);
    }
#else
    (void)sizeMB;
#endif
}

void
HdEmbreeOiioTextureSystem::SetRenderColorSpace(
    HdEmbreeRenderColorSpace colorSpace)
{
    _renderColorSpace = colorSpace;
}

HdEmbreeOiioTextureSystem::~HdEmbreeOiioTextureSystem()
{
#if defined(PXR_OIIO_PLUGIN_ENABLED)
    if (_impl && _impl->pngTextureSystem) {
        OIIO::TextureSystem::destroy(_impl->pngTextureSystem);
        _impl->pngTextureSystem = nullptr;
    }
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
    OIIO::TextureSystem* const textureSystem =
        _SelectTextureSystem(_impl.get(), request.filePath);
    if (!textureSystem) {
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
    options.sblur = std::max(0.0f, request.blur[0]);
    options.tblur = std::max(0.0f, request.blur[1]);

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

    // OIIO handle resolution, sampling, and color management can report
    // recoverable failures as exceptions. Keep the exception region around
    // those backend calls only, where the filename is still available.
    try {
    OIIO::TextureSystem::Perthread* const threadInfo =
        textureSystem->get_perthread_info();
    const _Impl::_CachedHandle& cached =
        _impl->ResolveHandle(textureSystem, threadInfo, request.filePath);
    OIIO::TextureSystem::TextureHandle* handle = cached.handle;

    // MaterialX graph coordinates use a lower-left origin, while the OIIO
    // image-space T axis increases from top to bottom.  Convert values and
    // derivatives together at the texture backend boundary.
    float s = request.st[0];
    float t = 1.0f - request.st[1];
    const float dsdx = request.dstdx[0];
    const float dtdx = -request.dstdx[1];
    const float dsdy = request.dstdy[0];
    const float dtdy = -request.dstdy[1];

    if (handle && cached.isUdim) {
        // UDIM tile numbers are defined in MaterialX UV space:
        // 1001 + floor(u) + 10 * floor(v).  Resolve the concrete tile before
        // flipping T for image-space sampling, otherwise the first row works
        // by accident and higher rows are resolved from the flipped axis.
        handle = textureSystem->resolve_udim(
            handle, threadInfo, request.st[0], request.st[1]);
        s = _LocalUdimCoord(request.st[0]);
        t = 1.0f - _LocalUdimCoord(request.st[1]);
    }

    const bool ok =
        handle && textureSystem->good(handle) &&
        textureSystem->texture(
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
        const std::string error = textureSystem->geterror();
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

    _ApplyColorTransform(
        request, sampled, _impl.get(), _renderColorSpace);

    mxcpp::Texture2DResult result;
    result.value = mxcpp::Vec4f(sampled[0], sampled[1], sampled[2], sampled[3]);
    result.status = mxcpp::TextureSampleStatus::Ok;
    return result;
    } catch (const std::exception& error) {
        if (_ShouldWarnOnce(_impl.get(), request.filePath)) {
            TF_WARN(
                "Exception sampling texture '%s': %s. Returning the authored "
                "default value.",
                request.filePath.c_str(), error.what());
        }
    } catch (...) {
        if (_ShouldWarnOnce(_impl.get(), request.filePath)) {
            TF_WARN(
                "Unknown exception sampling texture '%s'. Returning the "
                "authored default value.",
                request.filePath.c_str());
        }
    }
    return _MakeDefaultResult(request, mxcpp::TextureSampleStatus::Error);
#endif
}

PXR_NAMESPACE_CLOSE_SCOPE
