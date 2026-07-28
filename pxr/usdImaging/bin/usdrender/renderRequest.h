#ifndef PXR_USDIMAGING_BIN_USDRENDER_RENDER_REQUEST_H
#define PXR_USDIMAGING_BIN_USDRENDER_RENDER_REQUEST_H
#include "options.h"
#include "pxr/base/gf/range2f.h"
#include "pxr/base/gf/vec2i.h"
#include "pxr/base/tf/token.h"
#include "pxr/base/vt/dictionary.h"
#include "pxr/usd/sdf/layer.h"
#include "pxr/usd/sdf/path.h"
#include "pxr/usd/usd/stage.h"
#include <string>
#include <vector>

struct StageData
{
    pxr::UsdStageRefPtr stage;
    pxr::SdfLayerRefPtr session;
    pxr::SdfPath settings, pass;
};

struct RenderProduct
{
    pxr::SdfPath path, camera;
    pxr::GfVec2i resolution;
    pxr::GfRange2f dataWindow;
    float pixelAspectRatio;
    std::string name;
};

struct RenderRequest
{
    pxr::SdfPath settings, pass, camera;
    pxr::TfToken renderer;
    std::vector<RenderProduct> products;
    pxr::VtDictionary customSettings;
};

/// Open the root and optional user session layers, create the command-line
/// session wrapper, resolve the active RenderSettings/RenderPass paths, and
/// apply all attribute overrides. Returns false with a diagnostic on stderr if
/// any layer, path, mask, or override is invalid. A supplied session layer is
/// never modified.
bool LoadStage(const Options &, StageData *);

/// Compute the writable render products, common framing, camera, renderer, and
/// generic custom settings for one loaded stage. All products must share a
/// camera, resolution, and data window. Returns false with a diagnostic on
/// stderr when the authored render request cannot be executed as one render.
bool BuildRenderRequest(const Options &, const StageData &, RenderRequest *);

#endif
