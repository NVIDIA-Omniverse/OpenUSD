//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include <renderer/materials/oiioTextureSystem.h>

#include "pxr/base/arch/fileSystem.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/pxr.h"

#if defined(PXR_OIIO_PLUGIN_ENABLED)
#include <OpenImageIO/imageio.h>
#endif

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

void Test_Register(const char* name, std::function<bool()> fn);
bool Test_IsClose(float a, float b, float eps = 1e-5f);

#define _REG(name) Test_Register("TextureSystem." #name, &name)

#if defined(PXR_OIIO_PLUGIN_ENABLED)

namespace {

class _TemporaryImage
{
public:
    explicit _TemporaryImage(const char* suffix)
        : _directory(ArchMakeTmpSubdir(
              ArchGetTmpDir(), "testMaterialXCppTextureSystem"))
        , _path(_directory.empty()
              ? std::string()
              : _directory + "image" + suffix)
    {
    }

    ~_TemporaryImage()
    {
        if (!_path.empty()) {
            ArchUnlinkFile(_path.c_str());
        }
        if (!_directory.empty()) {
            ArchRmDir(_directory.c_str());
        }
    }

    _TemporaryImage(const _TemporaryImage&) = delete;
    _TemporaryImage& operator=(const _TemporaryImage&) = delete;

    const std::string& GetPath() const
    {
        return _path;
    }

private:
    std::string _directory;
    std::string _path;
};

bool
_WriteConstantImage(const std::string& path,
                    OIIO::TypeDesc format,
                    int channelCount,
                    const std::string& sourceColorSpace)
{
    OIIO::ImageSpec spec(1, 1, channelCount, format);
    if (!sourceColorSpace.empty()) {
        spec.attribute("oiio:ColorSpace", sourceColorSpace);
    }

    OIIO::ImageOutput::unique_ptr output = OIIO::ImageOutput::create(path);
    if (!output) {
        std::printf("    failed to create '%s': %s\n",
                    path.c_str(), OIIO::geterror().c_str());
        return false;
    }
    if (!output->open(path, spec)) {
        std::printf("    failed to open '%s': %s\n",
                    path.c_str(), output->geterror().c_str());
        return false;
    }

    std::vector<float> pixel(static_cast<size_t>(channelCount), 0.5f);
    if (channelCount == 4) {
        pixel[3] = 1.0f;
    }
    if (!output->write_image(OIIO::TypeDesc::FLOAT, pixel.data())) {
        std::printf("    failed to write '%s': %s\n",
                    path.c_str(), output->geterror().c_str());
        output->close();
        return false;
    }
    if (!output->close()) {
        std::printf("    failed to close '%s': %s\n",
                    path.c_str(), output->geterror().c_str());
        return false;
    }
    return true;
}

float
_GetStoredValue(OIIO::TypeDesc format)
{
    if (format == OIIO::TypeDesc::UINT8) {
        return 128.0f / 255.0f;
    }
    if (format == OIIO::TypeDesc::UINT16) {
        return 32768.0f / 65535.0f;
    }
    return 0.5f;
}

bool
_RunAutomaticColorSpaceCase(ty::OiioTextureSystem* textureSystem,
                            const char* label,
                            const char* suffix,
                            OIIO::TypeDesc format,
                            int channelCount,
                            const char* metadataColorSpace,
                            bool enableAutomaticColorSpace,
                            bool expectSrgb)
{
    _TemporaryImage image(suffix);
    if (image.GetPath().empty()) {
        std::printf("    %s: failed to create temporary directory\n", label);
        return false;
    }
    if (!_WriteConstantImage(
            image.GetPath(), format, channelCount, metadataColorSpace)) {
        return false;
    }

    mxcpp::Texture2DRequest request;
    request.filePath = image.GetPath();
    request.st = mxcpp::Vec2f(0.5f);
    request.filterType = mxcpp::TextureFilterType::Closest;
    request.dataRole = mxcpp::TextureDataRole::Color;
    request.inferSrgbFromFile = enableAutomaticColorSpace;
    request.channelCount = 4;
    request.channelFillValue = 1.0f;
    request.defaultValue = mxcpp::Vec4f(0.0f, 0.0f, 0.0f, 1.0f);

    const mxcpp::Texture2DResult result = textureSystem->Sample2D(request);
    if (result.status != mxcpp::TextureSampleStatus::Ok) {
        std::printf("    %s: texture sampling failed\n", label);
        return false;
    }

    GfVec3f expected(_GetStoredValue(format));
    if (expectSrgb &&
        !ty::ConvertToRenderColorSpace(
            "srgb_rec709_scene",
            ty::RenderColorSpace::LinearRec709,
            &expected)) {
        std::printf("    %s: expected sRGB conversion failed\n", label);
        return false;
    }

    constexpr float tolerance = 2.0e-3f;
    for (int channel = 0; channel < 3; ++channel) {
        if (!Test_IsClose(
                result.value[channel], expected[channel], tolerance)) {
            std::printf(
                "    %s: channel %d was %g, expected %g\n",
                label,
                channel,
                result.value[channel],
                expected[channel]);
            return false;
        }
    }
    return Test_IsClose(result.value[3], 1.0f, tolerance);
}

bool
TestUsdUvTextureAutomaticColorSpaceUsesOiioImageSpec()
{
    ty::OiioTextureSystem textureSystem;

    return _RunAutomaticColorSpaceCase(
               &textureSystem,
               "three-channel uint8 fallback",
               ".tga",
               OIIO::TypeDesc::UINT8,
               3,
               "",
               true,
               true) &&
           _RunAutomaticColorSpaceCase(
               &textureSystem,
               "four-channel uint8 fallback",
               ".tga",
               OIIO::TypeDesc::UINT8,
               4,
               "",
               true,
               true) &&
           _RunAutomaticColorSpaceCase(
               &textureSystem,
               "single-channel uint8 fallback",
               ".tga",
               OIIO::TypeDesc::UINT8,
               1,
               "",
               true,
               false) &&
           _RunAutomaticColorSpaceCase(
               &textureSystem,
               "three-channel uint16 fallback",
               ".tif",
               OIIO::TypeDesc::UINT16,
               3,
               "",
               true,
               false) &&
           _RunAutomaticColorSpaceCase(
               &textureSystem,
               "sRGB metadata overrides uint16 fallback",
               ".tif",
               OIIO::TypeDesc::UINT16,
               3,
               "sRGB",
               true,
               true) &&
           _RunAutomaticColorSpaceCase(
               &textureSystem,
               "automatic inspection is opt-in",
               ".tga",
               OIIO::TypeDesc::UINT8,
               3,
               "",
               false,
               false);
}

}  // namespace

#endif

void
Test_RegisterTextureSystemTests()
{
#if defined(PXR_OIIO_PLUGIN_ENABLED)
    _REG(TestUsdUvTextureAutomaticColorSpaceUsesOiioImageSpec);
#endif
}

#undef _REG
