#include "outputPath.h"
#include "pxr/base/tf/fileUtils.h"
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <sstream>
static bool _Expand(const std::string&in,const pxr::UsdTimeCode&t,std::string*out,std::string*err){
 *out=in;size_t p=0;while((p=out->find("{frame",p))!=std::string::npos){
  const size_t e=out->find('}',p);if(e==std::string::npos){*err="unterminated frame placeholder in '"+in+"'";return false;}
  const std::string f=out->substr(p+1,e-p-1);const double v=t.IsDefault()?0:t.GetValue();std::ostringstream s;
  if(f=="frame"){if(std::floor(v)==v)s<<(long long)v;else s<<v;}
  else if(f.rfind("frame:",0)==0&&f.size()>6&&f.back()=='d'){
   if(std::floor(v)!=v){*err="integer frame format used with non-integral frame in '"+in+"'";return false;}
   const std::string w=f.substr(6,f.size()-7);int width=0;
   try{size_t n=0;width=std::stoi(w,&n);if(n!=w.size()||width<0)throw 0;}catch(...){*err="invalid frame format {"+f+"} in '"+in+"'";return false;}
   s<<std::setfill(!w.empty()&&w[0]=='0'?'0':' ')<<std::setw(width)<<(long long)v;
  }else{*err="unsupported frame placeholder {"+f+"} in '"+in+"'";return false;}
  out->replace(p,e-p+1,s.str());p+=s.str().size();
 }return true;
}
bool ResolveOutputPath(const std::string&a,const std::string&r,const pxr::UsdTimeCode&t,std::string*out,std::string*err){
 std::filesystem::path p=a;if(!r.empty())p=std::filesystem::absolute(r)/p.relative_path();
 if(!_Expand(p.string(),t,out,err))return false;const std::string parent=std::filesystem::path(*out).parent_path().string();
 if(!parent.empty()&&!pxr::TfMakeDirs(parent,-1,true)){*err="could not create output directory '"+parent+"'";return false;}return true;
}
