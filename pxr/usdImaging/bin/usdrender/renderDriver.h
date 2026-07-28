#ifndef PXR_USDIMAGING_BIN_USDRENDER_RENDER_DRIVER_H
#define PXR_USDIMAGING_BIN_USDRENDER_RENDER_DRIVER_H
#include "options.h"
#include "renderRequest.h"

/// Render every requested time code to convergence and verify that each
/// authored product was freshly written. Returns false after diagnosing context,
/// framing, output-path, renderer, or missing-product failures.
bool RenderAll(const Options &, const StageData &, const RenderRequest &);

#endif
