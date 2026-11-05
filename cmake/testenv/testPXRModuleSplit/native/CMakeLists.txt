set(PXR_PREFIX fixture)
set(PXR_PACKAGE nativeProbe)
set(dependencies "")
if(PXR_ENABLE_PYTHON_SUPPORT)
    set(dependencies tf)
endif()
pxr_library(nativeProbe
    LIBRARIES ${dependencies}
    CPPFILES native.cpp
    PUBLIC_HEADERS probe.h
    DISABLE_PRECOMPILED_HEADERS)
if(PXR_ENABLE_PYTHON_SUPPORT)
    target_compile_definitions(nativeProbe PRIVATE EXPECT_NATIVE_POLICY)
endif()

pxr_library(pythonFreeProbe CPPFILES native.cpp
    PUBLIC_HEADERS probe.h DISABLE_PRECOMPILED_HEADERS)
if(PXR_ENABLE_PYTHON_SUPPORT)
    target_compile_definitions(pythonFreeProbe PRIVATE EXPECT_NATIVE_POLICY)
endif()
target_compile_definitions(pythonFreeProbe PRIVATE NATIVEPROBE_EXPORTS)
get_target_property(freeLinks pythonFreeProbe LINK_LIBRARIES)
if(freeLinks MATCHES "[Pp]ython|BoostPython")
    message(FATAL_ERROR "Compile enforcement introduced a Python dependency")
endif()

pxr_library(runtimeProbe PYTHON_CPPFILES runtime.cpp
    CPPFILES runtimeAnchor.cpp DISABLE_PRECOMPILED_HEADERS)
if(PXR_ENABLE_PYTHON_SUPPORT)
    get_target_property(runtimeLinks runtimeProbe LINK_LIBRARIES)
    if(NOT "pythonRuntime" IN_LIST runtimeLinks OR runtimeLinks MATCHES "BoostPython")
        message(FATAL_ERROR "Native Python fields did not select the neutral runtime")
    endif()
    pxr_library(supportProbe PYTHON_BINDING_SUPPORT
        LIBRARIES Python3::Python CPPFILES full.cpp
        DISABLE_PRECOMPILED_HEADERS)
    if(ILLEGAL_PYTHON_API)
        pxr_library(illegalProbe PYTHON_CPPFILES full.cpp
            DISABLE_PRECOMPILED_HEADERS)
    endif()
    if(BINDING_DEPENDENCY)
        add_library(bindingProxy INTERFACE)
        target_link_libraries(bindingProxy INTERFACE supportProbe)
        target_link_libraries(nativeProbe PRIVATE bindingProxy)
    endif()
endif()
