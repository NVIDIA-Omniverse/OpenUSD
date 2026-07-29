//
// hdEmbree OpenImageIO-backed texture system for MaterialXCpp.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_OIIO_TEXTURE_SYSTEM_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_OIIO_TEXTURE_SYSTEM_H

#include <renderer/colorManagement.h>
#include <renderer/materials/MaterialXCpp/textureSystem.h>

#include "pxr/pxr.h"

#include <memory>

PXR_NAMESPACE_OPEN_SCOPE

class HdEmbreeOiioTextureSystem final : public mxcpp::TextureSystem
{
public:
    struct _Impl;

    HdEmbreeOiioTextureSystem();
    ~HdEmbreeOiioTextureSystem() override;

    mxcpp::Texture2DResult Sample2D(
        const mxcpp::Texture2DRequest& request) const override;

    /// Set the OpenImageIO tile-cache size, in MB, for both the shared and
    /// PNG texture systems. Values are clamped to at least 1 MB. No-op when
    /// hdEmbree is built without the OIIO plugin.
    void SetCacheSizeMB(int sizeMB);

    /// Select the destination working space for color-role texture samples.
    void SetRenderColorSpace(HdEmbreeRenderColorSpace colorSpace);

private:
    std::unique_ptr<_Impl> _impl;
    HdEmbreeRenderColorSpace _renderColorSpace =
        HdEmbreeRenderColorSpace::LinearRec709;
};

PXR_NAMESPACE_CLOSE_SCOPE

#endif
