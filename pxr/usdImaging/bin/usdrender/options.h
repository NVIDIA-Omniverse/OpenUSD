#ifndef PXR_USDIMAGING_BIN_USDRENDER_OPTIONS_H
#define PXR_USDIMAGING_BIN_USDRENDER_OPTIONS_H
#include "pxr/usd/usd/timeCode.h"
#include <string>
#include <vector>
struct Options {
 std::string usdFile,sessionLayer,mask,purposes="proxy",resolverContext="root";
 std::string camera,frames,renderer,outputRoot,renderPass,renderSettings;
 std::string colorCorrection="disabled",traceFile,traceFormat="chrome";
 int imageWidth=0; float complexity=1.0f;
 bool defaultTime=false,gpu=true,drawMode=true,cameraLight=true,memstats=false;
};
bool ParseOptions(int argc,char**argv,Options*);
bool ParseFrames(const Options&,double start,std::vector<pxr::UsdTimeCode>*);
#endif
