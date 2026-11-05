set(PXR_PREFIX pxr/imaging)
set(PXR_PACKAGE pxOsd)

pxr_library(pxOsd
    LIBRARIES
        tf
        gf
        vt
        ${OPENSUBDIV_OSDCPU_LIBRARY}

    INCLUDE_DIRS
        ${OPENSUBDIV_INCLUDE_DIR}
        
    PUBLIC_HEADERS
        api.h

    PUBLIC_CLASSES
        meshTopology
        meshTopologyValidation
        refinerFactory
        subdivTags
        tokens
)

pxr_module(pxOsd
    LIBRARY pxOsd

    INCLUDE_DIRS
        ${OPENSUBDIV_INCLUDE_DIR}

    CPPFILES
        module.cpp
        wrapMeshTopology.cpp
        wrapMeshTopologyValidation.cpp
        wrapSubdivTags.cpp
        wrapTokens.cpp

    PYTHON_FILES
        __init__.py
)
