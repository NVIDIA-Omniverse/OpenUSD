//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "textureNodes.h"
#include "../nodeRegistry.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <type_traits>

namespace mxcpp {

namespace {

static constexpr float _kFloatEps = 1e-6f;

static const SlotName _kFile("file");
static const SlotName _kFileColorSpace("colorSpace:file");
static const SlotName _kLayer("layer");
static const SlotName _kTexcoord("texcoord");
static const SlotName _kOut("out");
static const SlotName _kDefaultVal("default");
static const SlotName _kUAddressMode("uaddressmode");
static const SlotName _kVAddressMode("vaddressmode");
static const SlotName _kFilterType("filtertype");
static const SlotName _kFrameRange("framerange");
static const SlotName _kFrameOffset("frameoffset");
static const SlotName _kFrameEndAction("frameendaction");
static const SlotName _kUvtiling("uvtiling");
static const SlotName _kUvoffset("uvoffset");
static const SlotName _kRealWorldImageSize("realworldimagesize");
static const SlotName _kRealWorldTileSize("realworldtilesize");

static Vec2f
_EvaluateVec2Input(const ParamMap& inputs,
                   const SlotName& slot,
                   const ShadingContext& ctx,
                   const Vec2f& defaultValue)
{
    Value value;
    if (inputs.Evaluate(slot, ctx, &value) &&
        ValueHolds<Vec2f>(value)) {
        return ValueGet<Vec2f>(value);
    }
    return Get<Vec2f>(inputs, slot, defaultValue);
}

static ShadingContext
_OffsetContextDx(const ShadingContext& ctx)
{
    ShadingContext shifted = ctx;
    shifted.position += ctx.dPositiondx;
    shifted.texcoord += Vec2f(ctx.dudx, ctx.dvdx);
    return shifted;
}

static ShadingContext
_OffsetContextDy(const ShadingContext& ctx)
{
    ShadingContext shifted = ctx;
    shifted.position += ctx.dPositiondy;
    shifted.texcoord += Vec2f(ctx.dudy, ctx.dvdy);
    return shifted;
}

static std::string
_NormalizeToken(const std::string& value, const char* fallback)
{
    return NormalizeSpaceName(value, std::string(fallback));
}

static std::string
_NormalizeColorSpace(const std::string& value)
{
    std::string normalized = value;
    std::transform(
        normalized.begin(), normalized.end(), normalized.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return normalized;
}

static TextureAddressMode
_GetAddressMode(const ParamMap& inputs,
                const SlotName& slot,
                const char* fallback)
{
    const std::string mode =
        _NormalizeToken(Get<std::string>(inputs, slot, std::string(fallback)),
                        fallback);
    if (mode == "constant") {
        return TextureAddressMode::Constant;
    }
    if (mode == "clamp") {
        return TextureAddressMode::Clamp;
    }
    if (mode == "mirror") {
        return TextureAddressMode::Mirror;
    }
    return TextureAddressMode::Periodic;
}

static TextureFilterType
_GetFilterType(const ParamMap& inputs)
{
    const std::string filterType =
        _NormalizeToken(
            Get<std::string>(inputs, _kFilterType, std::string("linear")),
            "linear");
    if (filterType == "closest") {
        return TextureFilterType::Closest;
    }
    if (filterType == "cubic") {
        return TextureFilterType::Cubic;
    }
    return TextureFilterType::Linear;
}

template<typename T>
struct _TextureValueTraits;

template<>
struct _TextureValueTraits<float>
{
    static constexpr int kChannelCount = 1;
    static constexpr float kFillValue = 0.0f;

    static Vec4f ToVec4(float value)
    {
        return Vec4f(value, 0.0f, 0.0f, 0.0f);
    }

    static float FromVec4(const Vec4f& value)
    {
        return value[0];
    }
};

template<>
struct _TextureValueTraits<Vec2f>
{
    static constexpr int kChannelCount = 2;
    static constexpr float kFillValue = 0.0f;

    static Vec4f ToVec4(const Vec2f& value)
    {
        return Vec4f(value[0], value[1], 0.0f, 0.0f);
    }

    static Vec2f FromVec4(const Vec4f& value)
    {
        return Vec2f(value[0], value[1]);
    }
};

template<>
struct _TextureValueTraits<Vec3f>
{
    static constexpr int kChannelCount = 3;
    static constexpr float kFillValue = 0.0f;

    static Vec4f ToVec4(const Vec3f& value)
    {
        return Vec4f(value[0], value[1], value[2], 0.0f);
    }

    static Vec3f FromVec4(const Vec4f& value)
    {
        return Vec3f(value[0], value[1], value[2]);
    }
};

template<>
struct _TextureValueTraits<Vec4f>
{
    static constexpr int kChannelCount = 4;

    static Vec4f ToVec4(const Vec4f& value)
    {
        return value;
    }

    static Vec4f FromVec4(const Vec4f& value)
    {
        return value;
    }
};

static Vec2f
_ApplyTiledTransform(const ParamMap& inputs, const Vec2f& texcoord)
{
    const Vec2f uvtiling = Get<Vec2f>(inputs, _kUvtiling, Vec2f(1.0f));
    const Vec2f uvoffset = Get<Vec2f>(inputs, _kUvoffset, Vec2f(0.0f));
    const Vec2f imageSize =
        Get<Vec2f>(inputs, _kRealWorldImageSize, Vec2f(1.0f));
    const Vec2f tileSize =
        Get<Vec2f>(inputs, _kRealWorldTileSize, Vec2f(1.0f));

    const Vec2f tiled = CompMult(texcoord, uvtiling) - uvoffset;
    const Vec2f safeImageSize(
        (std::abs(imageSize[0]) > _kFloatEps) ? imageSize[0] : 1.0f,
        (std::abs(imageSize[1]) > _kFloatEps) ? imageSize[1] : 1.0f);

    return Vec2f(
        tiled[0] / safeImageSize[0] * tileSize[0],
        tiled[1] / safeImageSize[1] * tileSize[1]);
}

template<bool IsTiled>
static Vec2f
_ComputeSampleCoord(const ParamMap& inputs, const Vec2f& texcoord)
{
    if constexpr (IsTiled) {
        return _ApplyTiledTransform(inputs, texcoord);
    } else {
        return texcoord;
    }
}

template<bool IsTiled>
static void
_ComputeTextureFootprint(const ParamMap& inputs,
                         const ShadingContext& ctx,
                         Vec2f* outSt,
                         Vec2f* outDstdx,
                         Vec2f* outDstdy)
{
    const Vec2f baseTexcoord =
        _EvaluateVec2Input(inputs, _kTexcoord, ctx, ctx.texcoord);
    const Vec2f st = _ComputeSampleCoord<IsTiled>(inputs, baseTexcoord);

    const ShadingContext shiftedDx = _OffsetContextDx(ctx);
    const Vec2f texcoordDx =
        _EvaluateVec2Input(inputs, _kTexcoord, shiftedDx, shiftedDx.texcoord);
    const Vec2f stDx = _ComputeSampleCoord<IsTiled>(inputs, texcoordDx);

    const ShadingContext shiftedDy = _OffsetContextDy(ctx);
    const Vec2f texcoordDy =
        _EvaluateVec2Input(inputs, _kTexcoord, shiftedDy, shiftedDy.texcoord);
    const Vec2f stDy = _ComputeSampleCoord<IsTiled>(inputs, texcoordDy);

    if (outSt) {
        *outSt = st;
    }
    if (outDstdx) {
        *outDstdx = stDx - st;
    }
    if (outDstdy) {
        *outDstdy = stDy - st;
    }
}

static bool
_UsesConstantDefaultOutside(const TextureAddressMode mode, const float value)
{
    return mode == TextureAddressMode::Constant &&
           (value < 0.0f || value > 1.0f);
}

template<typename T, TextureDataRole DataRole, bool IsTiled>
static void
_EvalTextureNode(const ParamMap& inputs,
                 const ShadingContext& ctx,
                 NodeOutputMap* outputs)
{
    const T defaultValue = Get<T>(inputs, _kDefaultVal, Zero<T>());
    const Vec4f defaultColor = _TextureValueTraits<T>::ToVec4(defaultValue);
    const std::string filePath = Get<std::string>(inputs, _kFile, std::string());

    if (!ctx.textureSystem || filePath.empty()) {
        (*outputs)[_kOut] = Value(defaultValue);
        return;
    }

    Vec2f st(0.0f);
    Vec2f dstdx(0.0f);
    Vec2f dstdy(0.0f);
    _ComputeTextureFootprint<IsTiled>(inputs, ctx, &st, &dstdx, &dstdy);

    const TextureAddressMode uAddressMode =
        _GetAddressMode(inputs, _kUAddressMode, "periodic");
    const TextureAddressMode vAddressMode =
        _GetAddressMode(inputs, _kVAddressMode, "periodic");
    if (_UsesConstantDefaultOutside(uAddressMode, st[0]) ||
        _UsesConstantDefaultOutside(vAddressMode, st[1])) {
        (*outputs)[_kOut] = Value(defaultValue);
        return;
    }

    Texture2DRequest request;
    request.filePath = filePath;
    request.layerName = Get<std::string>(inputs, _kLayer, std::string());
    request.st = st;
    request.dstdx = dstdx;
    request.dstdy = dstdy;
    request.uAddressMode = uAddressMode;
    request.vAddressMode = vAddressMode;
    request.filterType = _GetFilterType(inputs);
    request.frameRange = Get<std::string>(inputs, _kFrameRange, std::string());
    request.frameOffset = Get<int>(inputs, _kFrameOffset, 0);
    request.frameEndAction =
        _GetAddressMode(inputs, _kFrameEndAction, "constant");
    request.frame = ctx.frame;
    request.dataRole = DataRole;
    request.sourceColorSpace = _NormalizeColorSpace(
        Get<std::string>(inputs, _kFileColorSpace, std::string()));
    request.channelCount = _TextureValueTraits<T>::kChannelCount;
    request.defaultValue = defaultColor;

    if constexpr (std::is_same_v<T, Vec4f>) {
        request.channelFillValue =
            (DataRole == TextureDataRole::Color) ? 1.0f : 0.0f;
    } else {
        request.channelFillValue = _TextureValueTraits<T>::kFillValue;
    }

    const Texture2DResult sampled = ctx.textureSystem->Sample2D(request);
    (*outputs)[_kOut] = Value(_TextureValueTraits<T>::FromVec4(sampled.value));
}

}  // namespace

// ---- Registration --------------------------------------------------------

void
RegisterTextureNodes(NodeRegistry& reg)
{
    reg.Register(
        "ND_image_float",
        &_EvalTextureNode<float, TextureDataRole::NonColor, false>);
    reg.Register(
        "ND_image_color3",
        &_EvalTextureNode<Vec3f, TextureDataRole::Color, false>);
    reg.Register(
        "ND_image_color4",
        &_EvalTextureNode<Vec4f, TextureDataRole::Color, false>);
    reg.Register(
        "ND_image_vector2",
        &_EvalTextureNode<Vec2f, TextureDataRole::NonColor, false>);
    reg.Register(
        "ND_image_vector3",
        &_EvalTextureNode<Vec3f, TextureDataRole::NonColor, false>);
    reg.Register(
        "ND_image_vector4",
        &_EvalTextureNode<Vec4f, TextureDataRole::NonColor, false>);

    reg.Register(
        "ND_tiledimage_float",
        &_EvalTextureNode<float, TextureDataRole::NonColor, true>);
    reg.Register(
        "ND_tiledimage_color3",
        &_EvalTextureNode<Vec3f, TextureDataRole::Color, true>);
    reg.Register(
        "ND_tiledimage_color4",
        &_EvalTextureNode<Vec4f, TextureDataRole::Color, true>);
    reg.Register(
        "ND_tiledimage_vector2",
        &_EvalTextureNode<Vec2f, TextureDataRole::NonColor, true>);
    reg.Register(
        "ND_tiledimage_vector3",
        &_EvalTextureNode<Vec3f, TextureDataRole::NonColor, true>);
    reg.Register(
        "ND_tiledimage_vector4",
        &_EvalTextureNode<Vec4f, TextureDataRole::NonColor, true>);
}

}  // namespace mxcpp
