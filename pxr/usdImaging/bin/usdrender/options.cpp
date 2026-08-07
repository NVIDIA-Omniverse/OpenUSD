#include "options.h"
#include "pxr/base/tf/pxrCLI11/CLI11.h"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <string>

PXR_NAMESPACE_USING_DIRECTIVE
using namespace pxr_CLI;

bool ParseOptions(int argc, char **argv, Options *o)
{
    // CLI11 owns syntax, required-option, and constrained-value validation.
    // Temporary positive-form booleans are inverted after parsing so the
    // Options structure exposes the behavior the render driver needs.
    CLI::App app(
        "Renders authored RenderProducts from a USD file; the selected "
        "renderer must write every product");
    bool disableGpu = false, disableDrawMode = false,
         disableSceneMaterials = false;
    app.add_option("usdFilePath", o->usdFile, "USD file to render")->required();
    app.add_option("--mask", o->mask);
    app.add_option("--purposes", o->purposes);
    app.add_option("--sessionLayer", o->sessionLayer);
    app.add_flag("--disableGpu", disableGpu);
    app.add_flag("--disableDrawMode", disableDrawMode);
    app.add_flag("--enableCameraLight", o->cameraLight);
    app.add_flag("--disableSceneMaterials", disableSceneMaterials);
    app.add_option("--resolverContext", o->resolverContext)
        ->check(CLI::IsMember({"root", "inherit"}));
    app.add_option("--camera,-c", o->camera);
    app.add_option("--frames,-f", o->frames);
    app.add_flag("--defaultTime", o->defaultTime);
    std::string complexity = "low";
    app.add_option("--complexity", complexity)
        ->check(CLI::IsMember({"low", "medium", "high", "veryhigh"}));
    app.add_option("--colorCorrectionMode", o->colorCorrection);
    app.add_option("--renderer,-r", o->renderer);
    app.add_option("--imageWidth,-w", o->imageWidth);
    app.add_option("--renderPassPrimPath", o->renderPass);
    app.add_option("--renderSettingsPrimPath", o->renderSettings);
    app.add_option("--set,-s", o->setSpecs,
                   "Author an attribute override: "
                   "[uniform|varying] [type] /Prim.attribute = USDA-value. "
                   "Use {settings} for the resolved RenderSettings prim; it "
                   "is resolved before overrides. Relative asset paths are "
                   "unanchored and resolve from the working directory first. "
                   "Targets inside native instances or outside --mask are "
                   "rejected. "
                   "Examples: -s '{settings}.ty:maxBounces = 12', "
                   "-s '{settings}.ty:adaptiveThreshold = 0.005', "
                   "-s '/Camera.clippingRange = (0.1, 1000)'")
        ->allow_extra_args(false);
    app.add_flag("--printOverrides", o->printOverrides,
                 "Print the generated command-line override layer and continue; "
                 "requires --set");
    app.add_option("--outputRoot", o->outputRoot);
    app.add_option("--traceToFile", o->traceFile);
    app.add_option("--traceFormat", o->traceFormat)
        ->check(CLI::IsMember({"chrome", "trace"}));
    app.add_flag("--memstats", o->memstats);

    // Preserve usdrecord-compatible short aliases that CLI11 would otherwise
    // interpret as combined short flags rather than multi-character options.
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-rp") == 0)
            argv[i] = const_cast<char *>("--renderPassPrimPath");
        else if (std::strcmp(argv[i], "-rs") == 0)
            argv[i] = const_cast<char *>("--renderSettingsPrimPath");
    }
    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError &e) {
        app.exit(e);
        return false;
    }

    // Reject combinations whose meaning would otherwise depend on arbitrary
    // precedence outside CLI11's individual option validation.
    o->gpu = !disableGpu;
    o->drawMode = !disableDrawMode;
    o->sceneMaterials = !disableSceneMaterials;
    if (!o->frames.empty() && o->defaultTime) {
        std::cerr << "Cannot specify both --frames and --defaultTime\n";
        return false;
    }
    if (!o->renderPass.empty() && !o->renderSettings.empty()) {
        std::cerr << "Cannot specify both --renderSettingsPrimPath and "
                     "--renderPassPrimPath\n";
        return false;
    }
    if (o->printOverrides && o->setSpecs.empty()) {
        std::cerr << "--printOverrides requires at least one --set\n";
        return false;
    }

    // Translate user-facing complexity labels to Hydra refinement levels and
    // clamp negative width overrides to the documented automatic behavior.
    static const std::map<std::string, float> levels = {
        {"low", 1.0f}, {"medium", 1.1f}, {"high", 1.2f}, {"veryhigh", 1.3f}};
    o->complexity = levels.at(complexity);
    o->imageWidth = std::max(0, o->imageWidth);
    return true;
}

bool ParseFrames(const Options &o, double start,
                 std::vector<pxr::UsdTimeCode> *r)
{
    // Default time and an omitted frame range are distinct: the latter uses the
    // stage start time so animated stages render their authored first frame.
    if (o.defaultTime) {
        r->push_back(pxr::UsdTimeCode::Default());
        return true;
    }
    if (o.frames.empty()) {
        r->emplace_back(start);
        return true;
    }

    // Parse FIRST[:LAST][xSTEP]. A small tolerance includes LAST when repeated
    // floating-point addition lands just below the requested endpoint.
    std::string s = o.frames;
    double step = 1.0;
    const size_t x = s.find('x');
    try {
        if (x != std::string::npos) {
            step = std::stod(s.substr(x + 1));
            s.resize(x);
        }
        const size_t c = s.find(':');
        const double first = std::stod(s.substr(0, c));
        const double last =
            c == std::string::npos ? first : std::stod(s.substr(c + 1));
        if (step <= 0 || first > last)
            throw std::runtime_error("range");
        for (double f = first; f <= last + step * 1e-9; f += step)
            r->emplace_back(f);
    } catch (...) {
        std::cerr << "Invalid frame specification: " << o.frames << "\n";
        return false;
    }
    return true;
}
