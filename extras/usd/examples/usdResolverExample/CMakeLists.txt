set(PXR_PREFIX pxr/usd)
set(PXR_PACKAGE usdResolverExample)

pxr_plugin(${PXR_PACKAGE}
    LIBRARIES
        ar
        arch
        js
        tf
        vt

    PUBLIC_CLASSES
        resolver
        resolverContext

    PUBLIC_HEADERS
        api.h

    PYMODULE_CPPFILES
        module.cpp
        wrapResolverContext.cpp

    PYMODULE_FILES
        __init__.py

    RESOURCE_FILES
        plugInfo.json

    DOXYGEN_FILES
        overview.dox

    DISABLE_PRECOMPILED_HEADERS
)

if(PXR_ENABLE_PYTHON_SUPPORT)
    foreach(module _usdResolverExample _usdResolverExample_d)
        if(TARGET ${module})
            target_link_libraries(${module} PRIVATE arBoostPython)
        endif()
    endforeach()
endif()

pxr_test_scripts(
    testenv/testUsdResolverExample.py
)

pxr_register_test(testUsdResolverExample
    PYTHON
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testUsdResolverExample"
    TESTENV testenv/testUsdResolverExample
    EXPECTED_RETURN_CODE 0
    PRE_PATH
        ${CMAKE_INSTALL_PREFIX}/share/usd/examples/plugin
    ENV
        ${PXR_PLUGINPATH_NAME}=${CMAKE_INSTALL_PREFIX}/share/usd/examples/plugin/usdResolverExample/resources
        USD_RESOLVER_EXAMPLE_ASSET_DIR=./assets
)
    
