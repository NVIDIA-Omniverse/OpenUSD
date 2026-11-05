set(PXR_PREFIX fixture)
set(PXR_PACKAGE nativeProbe)
set(dependencies "")
if(PXR_ENABLE_PYTHON_SUPPORT)
    set(dependencies tf)
endif()
pxr_library(nativeProbe
    PYTHON_LIMITED_API
    LIBRARIES ${dependencies}
    CPPFILES native.cpp
    PUBLIC_HEADERS probe.h
    DISABLE_PRECOMPILED_HEADERS)
