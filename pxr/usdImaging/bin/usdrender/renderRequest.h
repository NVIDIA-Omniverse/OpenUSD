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
bool LoadStage(const Options &, StageData *);
bool BuildRenderRequest(const Options &, const StageData &, RenderRequest *);
#endif
