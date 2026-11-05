set(PXR_PREFIX {{ pxrPrefix }})
set(PXR_PACKAGE {{ libraryName }})

pxr_library({{ libraryName }}
    INCLUDE_SCHEMA_FILES

    LIBRARIES
        # List dependency libraries

    CPPFILES
        # List non-schema cpp files
)

pxr_module({{ libraryName }}
    LIBRARY {{ libraryName }}
    INCLUDE_SCHEMA_FILES

    LIBRARIES
        usdBoostPython

    PYTHON_FILES
        __init__.py
)
