set(PXR_PREFIX pxr/usdImaging)
set(PXR_PACKAGE usdrender_script)

# usdrender has usdAppUtils as a dependency, which is only built when GPU
# support is enabled.
if (NOT ${PXR_BUILD_GPU_SUPPORT})
    message(STATUS
        "Skipping ${PXR_PACKAGE} because PXR_BUILD_GPU_SUPPORT is OFF")
    return()
endif()

pxr_python_bin(usdrender
    DEPENDENCIES
        usd
        usdAppUtils)
