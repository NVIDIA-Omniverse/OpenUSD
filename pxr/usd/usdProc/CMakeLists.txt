set(PXR_PREFIX pxr/usd)
set(PXR_PACKAGE usdProc)

pxr_library(usdProc
    INCLUDE_SCHEMA_FILES

    LIBRARIES
        tf
        usd
        usdGeom

    PUBLIC_HEADERS
        api.h

    DOXYGEN_FILES
        overview.dox
)

pxr_module(usdProc
    LIBRARY usdProc
    INCLUDE_SCHEMA_FILES

    LIBRARIES
        usdBoostPython

    PYTHON_FILES
        __init__.py
)
