#ifndef PXR_USDIMAGING_BIN_USDRENDER_OFFSCREEN_CONTEXT_H
#define PXR_USDIMAGING_BIN_USDRENDER_OFFSCREEN_CONTEXT_H
#include <memory>
#include <string>
class OffscreenContext {
public:
 explicit OffscreenContext(bool enabled);
 ~OffscreenContext();
 OffscreenContext(const OffscreenContext&)=delete;
 OffscreenContext&operator=(const OffscreenContext&)=delete;
 bool IsValid()const;
 const std::string&GetError()const;
private:
 struct Impl;std::unique_ptr<Impl>_impl;std::string _error;bool _enabled;
};
#endif
