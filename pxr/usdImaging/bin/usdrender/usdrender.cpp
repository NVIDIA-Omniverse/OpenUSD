#include "options.h"
#include "pxr/base/tf/mallocTag.h"
#include "pxr/base/trace/collector.h"
#include "pxr/base/trace/reporter.h"
#include "renderDriver.h"
#include "renderRequest.h"
#include <fstream>
#include <iostream>

PXR_NAMESPACE_USING_DIRECTIVE

int main(int argc, char **argv)
{
    // Resolve all command-line, stage, and render-product state before creating
    // the Hydra engine, so invalid requests cannot start rendering.
    Options options;
    if (!ParseOptions(argc, argv, &options))
        return 1;
    if (options.memstats) {
        std::string error;
        if (!TfMallocTag::Initialize(&error))
            std::cerr << "Could not initialize MallocTag: " << error << "\n";
    }
    if (!options.traceFile.empty())
        TraceCollector::GetInstance().SetEnabled(true);
    StageData stage;
    if (!LoadStage(options, &stage))
        return 1;
    RenderRequest request;
    if (!BuildRenderRequest(options, stage, &request))
        return 1;

    // Rendering may fail after tracing or allocation was enabled. Always emit
    // requested diagnostics before returning the render result.
    const bool ok = RenderAll(options, stage, request);
    if (!options.traceFile.empty()) {
        TraceCollector::GetInstance().SetEnabled(false);
        std::ofstream out(options.traceFile);
        if (options.traceFormat == "chrome")
            TraceReporter::GetGlobalReporter()->ReportChromeTracing(out);
        else
            TraceReporter::GetGlobalReporter()->Report(out);
    }
    if (options.memstats && TfMallocTag::IsInitialized())
        std::cout << "Memory consumption: "
                  << TfMallocTag::GetTotalBytes() / (1024.0 * 1024.0)
                  << " Mb\n";
    return ok ? 0 : 1;
}
