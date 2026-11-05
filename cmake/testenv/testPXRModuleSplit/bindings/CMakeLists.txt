set(PXR_PREFIX fixture)
set(PXR_PACKAGE bindingProbe)
add_library(bindingOnly INTERFACE)
target_compile_definitions(bindingOnly INTERFACE BINDING_ONLY_MARKER)
set(coreRequirements "")
if(VERIFY_MONOLITHIC_LINKS)
    set(coreRequirements coreDependency)
endif()
pxr_module(bindingProbe
    LIBRARY nativeProbe
    LIBRARIES bindingOnly runtimeProbe ${coreRequirements}
    CPPFILES module.cpp
    PYTHON_FILES __init__.py
    DISABLE_PRECOMPILED_HEADERS)
