set(PXR_PREFIX pxr/usd)
set(PXR_PACKAGE usdHydra)

pxr_library(usdHydra
    INCLUDE_SCHEMA_FILES

    LIBRARIES
        tf
        usd
        usdShade

    PUBLIC_CLASSES
        discoveryPlugin
        renderPassAPI

    PUBLIC_HEADERS
        api.h

    RESOURCE_FILES
        shaders/shaderDefs.usda
        shaders/empty.glslfx

    DOXYGEN_FILES
        overview.dox
)

pxr_module(usdHydra
    LIBRARY usdHydra
    INCLUDE_SCHEMA_FILES

    LIBRARIES
        usdBoostPython

    PYTHON_FILES
        __init__.py
)

