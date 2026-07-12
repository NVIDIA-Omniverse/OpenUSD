#include "options.h"
#include "pxr/base/tf/pxrCLI11/CLI11.h"
#include <algorithm>
#include <iostream>
#include <cstring>
#include <string>
PXR_NAMESPACE_USING_DIRECTIVE
using namespace pxr_CLI;
bool ParseOptions(int argc,char**argv,Options*o){
 CLI::App app("Generates images from a USD file");
 bool disableGpu=false,disableDrawMode=false,disableCameraLight=false;
 app.add_option("usdFilePath",o->usdFile,"USD file to render")->required();
 app.add_option("--mask",o->mask); app.add_option("--purposes",o->purposes);
 app.add_option("--sessionLayer",o->sessionLayer);
 app.add_flag("--disableGpu",disableGpu);
 app.add_flag("--disableDrawMode",disableDrawMode);
 app.add_flag("--disableCameraLight",disableCameraLight);
 app.add_option("--resolverContext",o->resolverContext)->check(CLI::IsMember({"root","inherit"}));
 app.add_option("--camera,-c",o->camera); app.add_option("--frames,-f",o->frames);
 app.add_flag("--defaultTime",o->defaultTime);
 std::string complexity="low";
 app.add_option("--complexity",complexity)->check(CLI::IsMember({"low","medium","high","veryhigh"}));
 app.add_option("--colorCorrectionMode",o->colorCorrection);
 app.add_option("--renderer,-r",o->renderer); app.add_option("--imageWidth,-w",o->imageWidth);
 app.add_option("--renderPassPrimPath",o->renderPass);
 app.add_option("--renderSettingsPrimPath",o->renderSettings);
 app.add_option("--outputRoot",o->outputRoot);
 app.add_option("--traceToFile",o->traceFile);
 app.add_option("--traceFormat",o->traceFormat)->check(CLI::IsMember({"chrome","trace"}));
 app.add_flag("--memstats",o->memstats);
 for(int i=1;i<argc;++i){if(std::strcmp(argv[i],"-rp")==0)argv[i]=const_cast<char*>("--renderPassPrimPath");else if(std::strcmp(argv[i],"-rs")==0)argv[i]=const_cast<char*>("--renderSettingsPrimPath");}
 try{app.parse(argc,argv);}catch(const CLI::ParseError&e){app.exit(e);return false;}
 o->gpu=!disableGpu;o->drawMode=!disableDrawMode;o->cameraLight=!disableCameraLight;
 if(!o->frames.empty()&&o->defaultTime){std::cerr<<"Cannot specify both --frames and --defaultTime\n";return false;}
 if(!o->renderPass.empty()&&!o->renderSettings.empty()){std::cerr<<"Cannot specify both --renderSettingsPrimPath and --renderPassPrimPath\n";return false;}
 static const std::map<std::string,float> levels={{"low",1.0f},{"medium",1.1f},{"high",1.2f},{"veryhigh",1.3f}};
 o->complexity=levels.at(complexity); o->imageWidth=std::max(0,o->imageWidth); return true;
}
bool ParseFrames(const Options&o,double start,std::vector<pxr::UsdTimeCode>*r){
 if(o.defaultTime){r->push_back(pxr::UsdTimeCode::Default());return true;}
 if(o.frames.empty()){r->emplace_back(start);return true;}
 std::string s=o.frames; double step=1.0; const size_t x=s.find('x');
 try{if(x!=std::string::npos){step=std::stod(s.substr(x+1));s.resize(x);}
  const size_t c=s.find(':'); const double first=std::stod(s.substr(0,c));
  const double last=c==std::string::npos?first:std::stod(s.substr(c+1));
  if(step<=0||first>last)throw std::runtime_error("range");
  for(double f=first;f<=last+step*1e-9;f+=step)r->emplace_back(f);
 }catch(...){std::cerr<<"Invalid frame specification: "<<o.frames<<"\n";return false;}
 return true;
}
