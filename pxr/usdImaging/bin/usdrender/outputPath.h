#ifndef PXR_USDIMAGING_BIN_USDRENDER_OUTPUT_PATH_H
#define PXR_USDIMAGING_BIN_USDRENDER_OUTPUT_PATH_H
#include "pxr/usd/usd/timeCode.h"
#include <string>
bool ResolveOutputPath(const std::string &authored, const std::string &root,
                       const pxr::UsdTimeCode &, std::string *, std::string *);
#endif
