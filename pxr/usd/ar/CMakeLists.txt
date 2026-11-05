set(PXR_PREFIX pxr/usd)
set(PXR_PACKAGE ar)

pxr_library(ar
    PYTHON_LIMITED_API
    LIBRARIES
        arch
        js
        tf
        plug
        vt
        TBB::tbb

    PUBLIC_CLASSES
        asset
        assetInfo
        defaultResolver
        defaultResolverContext
        definePackageResolver
        defineResolver
        filesystemAsset
        filesystemWritableAsset
        inMemoryAsset
        notice
        packageResolver
        packageUtils
        resolvedPath
        resolver
        resolverContext
        resolverContextBinder
        resolverScopedCache
        timestamp
        writableAsset

    PUBLIC_HEADERS
        api.h
        ar.h
        defineResolverContext.h
        threadLocalScopedCache.h

    PRIVATE_HEADERS
        debugCodes.h
        testenv/TestArURIResolver_plugin.h

    CPPFILES
        debugCodes.cpp

    RESOURCE_FILES
        plugInfo.json

    DOXYGEN_FILES
        overview.dox
)

pxr_module(ar
    LIBRARY ar

    CPPFILES
        module.cpp
        wrapAsset.cpp
        wrapAssetInfo.cpp
        wrapDefaultResolverContext.cpp
        wrapDefaultResolver.cpp
        wrapNotice.cpp
        wrapPackageUtils.cpp
        wrapResolvedPath.cpp
        wrapResolverContextBinder.cpp
        wrapResolverContext.cpp
        wrapResolverScopedCache.cpp
        wrapResolver.cpp
        wrapTimestamp.cpp

    PYTHON_FILES
        __init__.py
)

# Preserve the native header requirement independently of binding helpers.
target_compile_features(ar PUBLIC cxx_std_17)

if(PXR_ENABLE_PYTHON_SUPPORT)
    # Headers for downstream resolver bindings; no additional compiled library.
    add_library(arBoostPython INTERFACE)
    target_link_libraries(arBoostPython INTERFACE tfBoostPython)
    if(NOT PXR_BUILD_MONOLITHIC)
        target_link_libraries(arBoostPython INTERFACE ar)
    endif()
    set(arBindingHeaders boost/pyResolverContext.h)
    _copy_headers(arBoostPython FILES ${arBindingHeaders} PREFIX "${PXR_PREFIX}")
    set(arBindingIncludeRoot "${PROJECT_BINARY_DIR}/${PXR_INSTALL_SUBDIR}/include")
    _get_install_dir(include arBindingInstallDir)
    target_sources(arBoostPython INTERFACE FILE_SET HEADERS
        BASE_DIRS "${arBindingIncludeRoot}"
        FILES "${arBindingIncludeRoot}/${PXR_PREFIX}/ar/boost/pyResolverContext.h")
    install(TARGETS arBoostPython EXPORT pxrTargets
        FILE_SET HEADERS DESTINATION "${arBindingInstallDir}")
    export(TARGETS arBoostPython APPEND FILE "${PROJECT_BINARY_DIR}/pxrTargets.cmake")
    foreach(module _ar _ar_d)
        if(TARGET ${module})
            target_link_libraries(${module} PRIVATE arBoostPython)
        endif()
    endforeach()
endif()

if(PXR_BUILD_TESTS AND PXR_ENABLE_PYTHON_SUPPORT AND BUILD_SHARED_LIBS)
    add_test(NAME testArContextBindingHelpers
        COMMAND "${PYTHON_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/testenv/testArContextBindingHelpers.py"
            --source-dir "${CMAKE_CURRENT_SOURCE_DIR}/testenv/testArContextBindingHelpers"
            --build-dir "${CMAKE_CURRENT_BINARY_DIR}/testArContextBindingHelpers/$<CONFIG>"
            --prefix "${CMAKE_INSTALL_PREFIX}"
            --cmake "${CMAKE_COMMAND}" --generator "${CMAKE_GENERATOR}"
            --config "$<CONFIG>")
endif()

if(PXR_BUILD_TESTS AND PXR_ENABLE_PYTHON_SUPPORT AND BUILD_SHARED_LIBS
        AND NOT PXR_BUILD_MONOLITHIC)
    get_target_property(arCoreHeaders ar PXR_PUBLIC_HEADERS)
    list(REMOVE_DUPLICATES arCoreHeaders)
    list(SORT arCoreHeaders)
    string(REPLACE ";" "\n" arCoreHeaders "${arCoreHeaders}")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/coreHeaders.txt" "${arCoreHeaders}\n")
    get_target_property(arSources ar SOURCES)
    set(arCoreSources "")
    foreach(source IN LISTS arSources)
        if(source MATCHES "[.]cpp$" AND NOT source MATCHES "pch[.]cpp$")
            if(NOT IS_ABSOLUTE "${source}")
                if(source STREQUAL "moduleDeps.cpp")
                    set(source "${CMAKE_CURRENT_BINARY_DIR}/${source}")
                else()
                    set(source "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
                endif()
            endif()
            string(APPEND arCoreSources "${source}\n")
        endif()
    endforeach()
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/coreSources.txt" "${arCoreSources}")
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/coreBoundary-$<CONFIG>.txt"
        CONTENT "links\t$<TARGET_PROPERTY:ar,LINK_LIBRARIES>\ninterface_links\t$<TARGET_PROPERTY:ar,INTERFACE_LINK_LIBRARIES>\ndefinitions\t$<TARGET_PROPERTY:ar,COMPILE_DEFINITIONS>\ninterface_definitions\t$<TARGET_PROPERTY:ar,INTERFACE_COMPILE_DEFINITIONS>\n")
    add_test(NAME testArCoreDependencyBoundary
        COMMAND "${PYTHON_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/testenv/testArCoreDependencyBoundary.py"
            --source-dir "${CMAKE_CURRENT_SOURCE_DIR}/testenv/testArCoreDependencyBoundary"
            --build-dir "${CMAKE_CURRENT_BINARY_DIR}/testArCoreDependencyBoundary/$<CONFIG>"
            --prefix "${CMAKE_INSTALL_PREFIX}"
            --headers "${CMAKE_CURRENT_BINARY_DIR}/coreHeaders.txt"
            --sources "${CMAKE_CURRENT_BINARY_DIR}/coreSources.txt"
            --ownership "${CMAKE_CURRENT_BINARY_DIR}/coreBoundary-$<CONFIG>.txt"
            --module-deps "${CMAKE_CURRENT_BINARY_DIR}/moduleDeps.cpp"
            --install-manifest "${PROJECT_BINARY_DIR}/install_manifest.txt"
            --cmake "${CMAKE_COMMAND}" --generator "${CMAKE_GENERATOR}"
            --config "$<CONFIG>")
    if(WIN32)
        add_test(NAME testArPackageComponents
            COMMAND "${PYTHON_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/pxr/base/tf/testenv/testTfPackageComponents.py"
                --source-dir "${PROJECT_SOURCE_DIR}/pxr/base/tf/testenv/testTfPackageComponents"
                --build-dir "${CMAKE_CURRENT_BINARY_DIR}/testArPackageComponents/$<CONFIG>"
                --prefix "${CMAKE_INSTALL_PREFIX}"
                --core-component ar --binding-component arBoostPython
                --cmake "${CMAKE_COMMAND}" --generator "${CMAKE_GENERATOR}"
                --config "$<CONFIG>")
    endif()
endif()

pxr_test_scripts(
    testenv/testArAssetInfo.py
    testenv/testArAdvancedAPI.py
    testenv/testArDefaultResolver.py
    testenv/testArOpenAsset.py
    testenv/testArPackageUtils.py
    testenv/testArPackageResolver.py
    testenv/testArResolvedPath.py
    testenv/testArResolverContext.py
    testenv/testArTimestamp.py
)

pxr_build_test_shared_lib(TestArAdvancedAPI
    INSTALL_PREFIX ArPlugins
    LIBRARIES
        tf
        ar
    CPPFILES
        testenv/TestArAdvancedAPI_plugin.cpp
) 

pxr_build_test_shared_lib(TestArPackageResolver
    INSTALL_PREFIX ArPlugins
    LIBRARIES
        tf
        ar
    CPPFILES
        testenv/TestArPackageResolver_plugin.cpp
) 

pxr_build_test_shared_lib(TestArOptionalImplementation
    INSTALL_PREFIX ArPlugins
    LIBRARIES
        tf
        ar
    CPPFILES
        testenv/TestArOptionalImplementation_plugin.cpp
) 

pxr_test_scripts(
    testenv/testArOptionalImplementation.py
)

pxr_register_test(testArOptionalImplementation_1
    PYTHON
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testArOptionalImplementation _TestResolver"
    TESTENV testenv/testArOptionalImplementation
    STDOUT_REDIRECT base.txt
    DIFF_COMPARE base.txt
)

pxr_register_test(testArOptionalImplementation_2
    PYTHON
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testArOptionalImplementation _TestResolverWithContextMethods"
    TESTENV testenv/testArOptionalImplementation
    STDOUT_REDIRECT resolver_with_context.txt
    DIFF_COMPARE resolver_with_context.txt
)

pxr_register_test(testArOptionalImplementation_3
    PYTHON
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testArOptionalImplementation _TestDerivedResolverWithContextMethods"
    TESTENV testenv/testArOptionalImplementation
    STDOUT_REDIRECT derived_resolver_with_context.txt
    DIFF_COMPARE derived_resolver_with_context.txt
)

pxr_register_test(testArOptionalImplementation_4
    PYTHON
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testArOptionalImplementation _TestResolverWithCacheMethods"
    TESTENV testenv/testArOptionalImplementation
    STDOUT_REDIRECT resolver_with_cache.txt
    DIFF_COMPARE resolver_with_cache.txt
)

pxr_register_test(testArOptionalImplementation_5
    PYTHON
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testArOptionalImplementation _TestDerivedResolverWithCacheMethods"
    TESTENV testenv/testArOptionalImplementation
    STDOUT_REDIRECT derived_resolver_with_cache.txt
    DIFF_COMPARE derived_resolver_with_cache.txt
)

# This test relies on a plugin built specifically for this test.
# The build system currently doesn't support this for static library
# builds, so we only enable this test on shared library builds.
if (BUILD_SHARED_LIBS)
    pxr_build_test_shared_lib(TestArURIResolver
        INSTALL_PREFIX ArPlugins
        LIBRARIES
            tf
            ar
        CPPFILES
            testenv/TestArURIResolver_plugin.cpp
    ) 

    pxr_build_test(testArURIResolver_CPP
        LIBRARIES
            arch
            tf
            ar
        CPPFILES
            testenv/testArURIResolver.cpp
    )

    pxr_test_scripts(
        testenv/testArURIResolver.py
    )

    pxr_register_test(testArURIResolver
        PYTHON
        COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testArURIResolver"
    )

    pxr_register_test(testArURIResolver_CPP
        COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testArURIResolver_CPP"
    )
endif()

pxr_build_test(testArDefaultResolver_CPP
    LIBRARIES
        arch
        tf
        ar
    CPPFILES
        testenv/testArDefaultResolver.cpp
) 

pxr_build_test(testArNotice_CPP
    LIBRARIES
        arch
        tf
        ar
    CPPFILES
        testenv/testArNotice.cpp
) 

pxr_register_test(testArNotice_CPP
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testArNotice_CPP"
)

pxr_build_test(testArResolverContext_CPP
    LIBRARIES
        arch
        tf
        ar
    CPPFILES
        testenv/testArResolverContext.cpp
) 

pxr_build_test(testArThreadedAssetCreation
    LIBRARIES
        arch
        tf
        ar
    CPPFILES
        testenv/testArThreadedAssetCreation.cpp
) 

pxr_register_test(testArAssetInfo
    PYTHON
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testArAssetInfo"
)

pxr_register_test(testArAdvancedAPI
    PYTHON
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testArAdvancedAPI"
)

pxr_register_test(testArDefaultResolver
    PYTHON
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testArDefaultResolver"
)

pxr_register_test(testArDefaultResolver_CPP
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testArDefaultResolver_CPP"
)

pxr_register_test(testArOpenAsset
    PYTHON
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testArOpenAsset"
)

pxr_register_test(testArPackageResolver
    PYTHON
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testArPackageResolver"
)

pxr_register_test(testArPackageUtils
    PYTHON
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testArPackageUtils"
)

pxr_register_test(testArResolvedPath
    PYTHON
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testArResolvedPath"
)

pxr_register_test(testArTimestamp
    PYTHON
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testArTimestamp"
)

pxr_register_test(testArResolverContext
    PYTHON
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testArResolverContext"
)

pxr_register_test(testArResolverContext_CPP
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testArResolverContext_CPP"
)

pxr_register_test(testArThreadedAssetCreation
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testArThreadedAssetCreation"
)
