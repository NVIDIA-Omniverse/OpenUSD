set(PXR_PREFIX pxr/usd)
set(PXR_PACKAGE usdProc)

pxr_library(usdProc
    PYMODULE_LIBRARIES
        usdBoostPython

    INCLUDE_SCHEMA_FILES

    LIBRARIES
        tf
        usd
        usdGeom

    PUBLIC_HEADERS
        api.h

    PYMODULE_FILES
        __init__.py

    DOXYGEN_FILES
        overview.dox

)
