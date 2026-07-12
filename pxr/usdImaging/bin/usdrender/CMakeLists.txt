set(PXR_PREFIX pxr/usdImaging)
set(PXR_PACKAGE usdrender)
if (NOT ${PXR_BUILD_GPU_SUPPORT})
    message(STATUS "Skipping ${PXR_PACKAGE} because PXR_BUILD_GPU_SUPPORT is OFF")
    return()
endif()
pxr_cpp_bin(usdrender
    LIBRARIES
        arch ar cameraUtil garch gf glf hd hdSt hdx hio sdf tf trace usd usdGeom
        usdHydra usdImagingGL usdRender usdUtils vt
)
if (UNIX AND NOT APPLE)
    target_link_libraries(usdrender PRIVATE OpenGL::GL X11::X11)
endif()
target_sources(usdrender PRIVATE
    options.cpp
    offscreenContext.cpp
    outputPath.cpp
    renderRequest.cpp
    renderDriver.cpp
)

if (PXR_BUILD_EMBREE_PLUGIN AND BUILD_SHARED_LIBS)
    pxr_register_test(testUsdRenderCpuFrames
        COMMAND "${CMAKE_INSTALL_PREFIX}/bin/usdrender test.usda --disableGpu --renderer Embree --frames 1:2 --outputRoot outputs"
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
endif()
