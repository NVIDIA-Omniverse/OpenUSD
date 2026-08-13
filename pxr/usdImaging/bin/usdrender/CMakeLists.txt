set(PXR_PREFIX pxr/usdImaging)
set(PXR_PACKAGE usdrender)
if (NOT ${PXR_BUILD_GPU_SUPPORT})
    message(STATUS "Skipping ${PXR_PACKAGE} because PXR_BUILD_GPU_SUPPORT is OFF")
    return()
endif()
pxr_cpp_bin(usdrender
    LIBRARIES
        arch ar cameraUtil garch gf glf hd sdf tf trace usd usdGeom
        usdHydra usdImagingGL usdRender usdUtils vt
)
if (UNIX AND NOT APPLE)
    target_link_libraries(usdrender PRIVATE OpenGL::GL X11::X11)
endif()
target_sources(usdrender PRIVATE
    options.cpp
    offscreenContext.cpp
    overrides.cpp
    outputPath.cpp
    renderRequest.cpp
    renderDriver.cpp
)

pxr_build_test(testUsdRenderOverrides
    LIBRARIES
        gf sdf tf usd vt
    CPPFILES
        overrides.cpp
        testenv/testUsdRenderOverrides.cpp
)

pxr_register_test(testUsdRenderOverrides
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testUsdRenderOverrides"
    EXPECTED_RETURN_CODE 0
)

pxr_build_test(testUsdRenderOptions
    LIBRARIES
        usd
    CPPFILES
        options.cpp
        testenv/testUsdRenderOptions.cpp
)

pxr_register_test(testUsdRenderOptions
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testUsdRenderOptions"
    EXPECTED_RETURN_CODE 0
)

if (PXR_BUILD_EMBREE_PLUGIN AND BUILD_SHARED_LIBS)
    pxr_register_test(testUsdRenderSetOverrides
        COMMAND "${PYTHON_EXECUTABLE} testUsdRenderSetOverrides.py ${CMAKE_INSTALL_PREFIX}/bin/usdrender"
        EXPECTED_RETURN_CODE 0
        TESTENV testenv/SetOverrides
    )

    pxr_register_test(testUsdRenderCpuFrames
        COMMAND "${CMAKE_INSTALL_PREFIX}/bin/usdrender test.usda --disableGpu --enableCameraLight --frames 1:2 --outputRoot outputs"
        IMAGE_DIFF_COMPARE
            outputs/frame.0001.png
            outputs/frame.0002.png
        FAIL 0.01
        FAIL_PERCENT 0.001
        WARN 0.001
        WARN_PERCENT 0.001
        EXPECTED_RETURN_CODE 0
        TESTENV testenv/CpuFrames
    )

    pxr_register_test(testUsdRenderDefaults
        COMMAND "${PYTHON_EXECUTABLE} testUsdRenderDefaults.py ${CMAKE_INSTALL_PREFIX}/bin/usdrender"
        EXPECTED_RETURN_CODE 0
        TESTENV testenv/CpuFrames
    )

    pxr_register_test(testUsdRenderInvalidProduct
        COMMAND "${CMAKE_INSTALL_PREFIX}/bin/usdrender test.usda --disableGpu --renderer Embree --outputRoot ."
        EXPECTED_RETURN_CODE 1
        TESTENV testenv/InvalidProduct
    )
endif()
