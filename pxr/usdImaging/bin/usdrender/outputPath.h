#ifndef PXR_USDIMAGING_BIN_USDRENDER_OUTPUT_PATH_H
#define PXR_USDIMAGING_BIN_USDRENDER_OUTPUT_PATH_H
#include "pxr/usd/usd/timeCode.h"
#include <string>

/// Resolve an authored product path under an optional output root, expand frame
/// placeholders, and create its parent directories. Returns false with err for
/// invalid placeholders or directory creation failures.
bool ResolveOutputPath(const std::string &authored, const std::string &root,
                       const pxr::UsdTimeCode &, std::string *, std::string *);

#endif
