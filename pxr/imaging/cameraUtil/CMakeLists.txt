set(PXR_PREFIX pxr/imaging)
set(PXR_PACKAGE cameraUtil)

pxr_library(cameraUtil
    LIBRARIES
        tf
        gf

    PUBLIC_CLASSES
        conformWindow
        framing
        screenWindowParameters
        
    PUBLIC_HEADERS
        api.h
)

pxr_module(cameraUtil
    LIBRARY cameraUtil

    CPPFILES
        module.cpp
        wrapConformWindow.cpp
        wrapFraming.cpp
        wrapScreenWindowParameters.cpp

    PYTHON_FILES
        __init__.py
)

pxr_test_scripts(
    testenv/testCameraUtil.py
)

pxr_register_test(testCameraUtil
    PYTHON
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testCameraUtil"
    EXPECTED_RETURN_CODE 0
)
