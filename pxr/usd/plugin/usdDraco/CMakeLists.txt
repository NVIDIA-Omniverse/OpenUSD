set(PXR_PREFIX pxr/usd)
set(PXR_PACKAGE usdDraco)

if(WIN32 AND NOT CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS)
  set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS 1)
  set(USD_DRACO_SET_WINDOWS_EXPORT 1)
endif()

pxr_plugin(${PXR_PACKAGE}
    LIBRARIES
        tf
        gf
        sdf
        usd
        usdGeom
        ${DRACO_LIBRARY}

    INCLUDE_DIRS
        ${DRACO_INCLUDES}
    
    PRIVATE_HEADERS
        exportAttribute.h
        flag.h
        importAttribute.h
    
    CPPFILES
        attributeDescriptor.cpp
        attributeFactory.cpp
        exportTranslator.cpp
        fileFormat.cpp
        importTranslator.cpp
        writer.cpp

    RESOURCE_FILES
        plugInfo.json

    DOXYGEN_FILES
        overview.dox
)

pxr_module(${PXR_PACKAGE}
    LIBRARY ${PXR_PACKAGE}

    INCLUDE_DIRS
        ${DRACO_INCLUDES}

    CPPFILES
        module.cpp
        wrapDraco.cpp

    PYTHON_FILES
        __init__.py
)

if(USD_DRACO_SET_WINDOWS_EXPORT)
  unset(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS)
endif()

