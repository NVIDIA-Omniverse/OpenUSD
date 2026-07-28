#ifndef PXR_USDIMAGING_BIN_USDRENDER_OVERRIDES_H
#define PXR_USDIMAGING_BIN_USDRENDER_OVERRIDES_H

#include "pxr/usd/sdf/layer.h"
#include "pxr/usd/sdf/path.h"
#include "pxr/usd/usd/stage.h"

#include <string>
#include <vector>

/// Validate and author command-line attribute overrides into a fresh anonymous
/// session layer. Target prims must already be defined, visible through the
/// stage population mask, and outside native instances. The session may have
/// sublayers but must contain no authored specs. Validation and parse failures
/// leave it unchanged; an authoring backend failure is reported through error.
/// Returns false with error identifying the offending original specification.
bool ApplyAttributeOverrides(const std::vector<std::string>& specs,
                             const pxr::UsdStageRefPtr& stage,
                             const pxr::SdfLayerRefPtr& session,
                             const pxr::SdfPath& settingsPath,
                             std::string* error);

#endif
