#include "offscreenContext.h"
#if defined(__linux__)
#include <X11/Xlib.h>
#include <GL/glx.h>
struct OffscreenContext::Impl{Display*display=nullptr;GLXContext context=nullptr;GLXPbuffer pbuffer=0;};
OffscreenContext::OffscreenContext(bool enabled):_impl(new Impl),_enabled(enabled){
 if(!enabled){return;}
 _impl->display=XOpenDisplay(nullptr);
 if(!_impl->display){_error="Could not open X display for GPU rendering";return;}
 const int attrs[]={GLX_X_RENDERABLE,True,GLX_DRAWABLE_TYPE,GLX_PBUFFER_BIT,GLX_RENDER_TYPE,GLX_RGBA_BIT,GLX_RED_SIZE,8,GLX_GREEN_SIZE,8,GLX_BLUE_SIZE,8,None};
 int count=0;GLXFBConfig*configs=glXChooseFBConfig(_impl->display,DefaultScreen(_impl->display),attrs,&count);
 if(!configs||count==0){_error="Could not choose a GLX framebuffer configuration";if(configs)XFree(configs);return;}
 const int pbAttrs[]={GLX_PBUFFER_WIDTH,1,GLX_PBUFFER_HEIGHT,1,None};_impl->pbuffer=glXCreatePbuffer(_impl->display,configs[0],pbAttrs);
 _impl->context=glXCreateNewContext(_impl->display,configs[0],GLX_RGBA_TYPE,nullptr,True);XFree(configs);
 if(!_impl->pbuffer||!_impl->context||!glXMakeContextCurrent(_impl->display,_impl->pbuffer,_impl->pbuffer,_impl->context))_error="Could not create or bind a GLX offscreen context";
}
OffscreenContext::~OffscreenContext(){if(!_impl)return;if(_impl->display){glXMakeContextCurrent(_impl->display,None,None,nullptr);if(_impl->context)glXDestroyContext(_impl->display,_impl->context);if(_impl->pbuffer)glXDestroyPbuffer(_impl->display,_impl->pbuffer);XCloseDisplay(_impl->display);}}
#else
struct OffscreenContext::Impl{};
OffscreenContext::OffscreenContext(bool enabled):_impl(new Impl),_enabled(enabled){if(enabled)_error="Native offscreen GPU context is not implemented on this platform";}
OffscreenContext::~OffscreenContext()=default;
#endif
bool OffscreenContext::IsValid()const{return !_enabled||_error.empty();}
const std::string&OffscreenContext::GetError()const{return _error;}
