#ifndef PXR_USDIMAGING_BIN_USDRENDER_OPTIONS_H
#define PXR_USDIMAGING_BIN_USDRENDER_OPTIONS_H
#include "pxr/usd/usd/timeCode.h"
#include <string>
#include <vector>

struct Options
{
    std::string usdFile, sessionLayer, mask, purposes = "proxy",
                                             resolverContext = "root";
    std::string camera, frames, renderer, outputRoot, renderPass,
        renderSettings;
    std::vector<std::string> setSpecs;
    std::string colorCorrection = "disabled", traceFile, traceFormat = "chrome";
    int imageWidth = 0;
    float complexity = 1.0f;
    bool defaultTime = false, gpu = true, drawMode = true, cameraLight = false,
         sceneMaterials = true, memstats = false, printOverrides = false;
};

/// Parse and validate command-line syntax into options. Returns false after
/// CLI11 or semantic diagnostics have been written to stderr.
bool ParseOptions(int argc, char **argv, Options *);

/// Expand FIRST[:LAST][xSTEP], default time, or the stage-start fallback into
/// ordered time codes. Returns false with a diagnostic for malformed, reversed,
/// or non-positive-step ranges. The result is appended to the supplied vector.
bool ParseFrames(const Options &, double start,
                 std::vector<pxr::UsdTimeCode> *);

#endif
