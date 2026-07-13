//
// hdEmbree OpenImageIO-backed texture system for MaterialXCpp.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_OIIO_TEXTURE_SYSTEM_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_OIIO_TEXTURE_SYSTEM_H

#include "pxr/pxr.h"
#include "pxr/imaging/plugin/hdEmbree/renderer/MaterialXCpp/textureSystem.h"

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

private:
    std::unique_ptr<_Impl> _impl;
};

PXR_NAMESPACE_CLOSE_SCOPE

#endif
