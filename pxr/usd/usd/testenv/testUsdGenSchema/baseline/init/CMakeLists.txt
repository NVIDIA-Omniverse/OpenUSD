set(PXR_PREFIX pxr/usd)
set(PXR_PACKAGE init)

pxr_library(init
    INCLUDE_SCHEMA_FILES

    LIBRARIES
        # List dependency libraries

    CPPFILES
        # List non-schema cpp files
)

pxr_module(init
    LIBRARY init
    INCLUDE_SCHEMA_FILES

    LIBRARIES
        usdBoostPython

    PYTHON_FILES
        __init__.py
)
