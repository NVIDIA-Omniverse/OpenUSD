set(PXR_PREFIX pxr/base)
set(PXR_PACKAGE vt)

pxr_library(vt
    LIBRARIES
        arch
        tf
        gf
        trace
        TBB::tbb

    PUBLIC_CLASSES
        array
        arrayEdit
        arrayEditBuilder
        arrayEditOps
        debugCodes
        dictionary
        hash
        streamOut
        types
        value
        valueCommon
        valueComposeOver
        valueRef
        valueTransform

    PYTHON_PUBLIC_CLASSES
        valueFromPython
        valueToPython

    PUBLIC_HEADERS
        addExtVisitValueType.h
        api.h
        traits.h
        typeHeaders.h
        visitValue.h
    
    PRIVATE_HEADERS
        arrayPyBuffer.h

    DOXYGEN_FILES
        overview.dox
)

pxr_module(vt
    LIBRARY vt

    CPPFILES
        arrayPyBuffer.cpp
        module.cpp
        wrapArrayBase.cpp
        wrapArrayDualQuaternion.cpp
        wrapArrayFloat.cpp
        wrapArrayIntegral.cpp
        wrapArrayMatrix.cpp
        wrapArrayQuaternion.cpp
        wrapArrayRange.cpp
        wrapArrayString.cpp
        wrapArrayToken.cpp
        wrapArrayVec.cpp
        wrapDictionary.cpp
        wrapValue.cpp
        wrapValueRef.cpp

    PYTHON_FILES
        __init__.py
)

# Preserve Vt's C++ requirement without relying on a binding target to supply it.
target_compile_features(vt PUBLIC cxx_std_17)

if(PXR_ENABLE_PYTHON_SUPPORT)
    # Reusable binding adapters, without another compiled library. Core Vt
    # owns the registry; modules own the registered extraction callbacks.
    add_library(vtBoostPython INTERFACE)
    target_link_libraries(vtBoostPython INTERFACE tfBoostPython)
    if(NOT PXR_BUILD_MONOLITHIC)
        target_link_libraries(vtBoostPython INTERFACE vt)
    endif()
    set(vtBindingHeaders
        boost/pyOperators.h
        boost/wrapArray.h
        boost/wrapArrayEdit.h
        boost/valueBoost.h
        boost/valueFromPythonBoost.h
        boost/valueToPythonBoost.h)
    _copy_headers(vtBoostPython FILES ${vtBindingHeaders} PREFIX "${PXR_PREFIX}")
    set(vtBindingIncludeRoot "${PROJECT_BINARY_DIR}/${PXR_INSTALL_SUBDIR}/include")
    _get_install_dir(include vtBindingInstallDir)
    set(vtBindingBuildHeaders "")
    foreach(header IN LISTS vtBindingHeaders)
        list(APPEND vtBindingBuildHeaders
            "${vtBindingIncludeRoot}/${PXR_PREFIX}/vt/${header}")
    endforeach()
    target_sources(vtBoostPython INTERFACE FILE_SET HEADERS
        BASE_DIRS "${vtBindingIncludeRoot}"
        FILES ${vtBindingBuildHeaders})
    install(TARGETS vtBoostPython EXPORT pxrTargets
        FILE_SET HEADERS DESTINATION "${vtBindingInstallDir}")
    export(TARGETS vtBoostPython APPEND FILE "${PROJECT_BINARY_DIR}/pxrTargets.cmake")
    foreach(module _vt _vt_d)
        if(TARGET ${module})
            target_link_libraries(${module} PRIVATE vtBoostPython)
        endif()
    endforeach()
endif()

if(PXR_ENABLE_PYTHON_SUPPORT AND BUILD_SHARED_LIBS)
    set(vtPythonTarget _vt)
    if(WIN32 AND PXR_USE_DEBUG_PYTHON)
        set(vtPythonTarget _vt_d)
    endif()
    set(vtCoreTarget vt)
    if(PXR_BUILD_MONOLITHIC)
        set(vtCoreTarget usd_m)
    endif()
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/arrayOwnership-$<CONFIG>.txt"
        CONTENT "core\t$<TARGET_PROPERTY:vt,SOURCES>\nmodule\t$<TARGET_PROPERTY:${vtPythonTarget},SOURCES>\ncore_file\t$<TARGET_FILE:${vtCoreTarget}>\nmodule_file\t$<TARGET_FILE:${vtPythonTarget}>\n")
endif()

if(PXR_BUILD_TESTS AND PXR_ENABLE_PYTHON_SUPPORT AND BUILD_SHARED_LIBS)
    add_test(NAME testVtArrayBindingHelpers
        COMMAND "${PYTHON_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/testenv/testVtArrayBindingHelpers.py"
            --source-dir "${CMAKE_CURRENT_SOURCE_DIR}/testenv/testVtArrayBindingHelpers"
            --build-dir "${CMAKE_CURRENT_BINARY_DIR}/testVtArrayBindingHelpers/$<CONFIG>"
            --prefix "${CMAKE_INSTALL_PREFIX}"
            --ownership "${CMAKE_CURRENT_BINARY_DIR}/arrayOwnership-$<CONFIG>.txt"
            --install-manifest "${PROJECT_BINARY_DIR}/install_manifest.txt"
            --cmake "${CMAKE_COMMAND}" --generator "${CMAKE_GENERATOR}"
            --config "$<CONFIG>")
    add_test(NAME testVtValueBindingExtraction
        COMMAND "${PYTHON_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/testenv/testVtValueBindingExtraction.py"
            --source-dir "${CMAKE_CURRENT_SOURCE_DIR}/testenv/testVtValueBindingExtraction"
            --build-dir "${CMAKE_CURRENT_BINARY_DIR}/testVtValueBindingExtraction/$<CONFIG>"
            --prefix "${CMAKE_INSTALL_PREFIX}"
            --cmake "${CMAKE_COMMAND}"
            --generator "${CMAKE_GENERATOR}"
            --config "$<CONFIG>")
    add_test(NAME testVtConversionRegistry
        COMMAND "${PYTHON_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/testenv/testVtConversionRegistry.py"
            --source-dir "${CMAKE_CURRENT_SOURCE_DIR}/testenv/testVtConversionRegistry"
            --build-dir "${CMAKE_CURRENT_BINARY_DIR}/testVtConversionRegistry/$<CONFIG>"
            --prefix "${CMAKE_INSTALL_PREFIX}"
            --cmake "${CMAKE_COMMAND}"
            --generator "${CMAKE_GENERATOR}"
            --config "$<CONFIG>")
endif()

if(PXR_BUILD_TESTS AND PXR_ENABLE_PYTHON_SUPPORT AND BUILD_SHARED_LIBS
        AND NOT PXR_BUILD_MONOLITHIC)
    get_target_property(vtCoreHeaders vt PXR_PUBLIC_HEADERS)
    list(REMOVE_DUPLICATES vtCoreHeaders)
    list(SORT vtCoreHeaders)
    string(REPLACE ";" "\n" vtCoreHeaders "${vtCoreHeaders}")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/coreHeaders.txt" "${vtCoreHeaders}\n")
    get_target_property(vtSources vt SOURCES)
    set(vtCoreSources "")
    foreach(source IN LISTS vtSources)
        if(source MATCHES "[.]cpp$" AND NOT source MATCHES "pch[.]cpp$")
            if(NOT IS_ABSOLUTE "${source}")
                if(source STREQUAL "moduleDeps.cpp")
                    set(source "${CMAKE_CURRENT_BINARY_DIR}/${source}")
                else()
                    set(source "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
                endif()
            endif()
            string(APPEND vtCoreSources "${source}\n")
        endif()
    endforeach()
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/coreSources.txt" "${vtCoreSources}")
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/coreBoundary-$<CONFIG>.txt"
        CONTENT "links\t$<TARGET_PROPERTY:vt,LINK_LIBRARIES>\ninterface_links\t$<TARGET_PROPERTY:vt,INTERFACE_LINK_LIBRARIES>\ndefinitions\t$<TARGET_PROPERTY:vt,COMPILE_DEFINITIONS>\ninterface_definitions\t$<TARGET_PROPERTY:vt,INTERFACE_COMPILE_DEFINITIONS>\n")
    add_test(NAME testVtCoreDependencyBoundary
        COMMAND "${PYTHON_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/testenv/testVtCoreDependencyBoundary.py"
            --source-dir "${CMAKE_CURRENT_SOURCE_DIR}/testenv/testVtCoreDependencyBoundary"
            --build-dir "${CMAKE_CURRENT_BINARY_DIR}/testVtCoreDependencyBoundary/$<CONFIG>"
            --prefix "${CMAKE_INSTALL_PREFIX}"
            --headers "${CMAKE_CURRENT_BINARY_DIR}/coreHeaders.txt"
            --sources "${CMAKE_CURRENT_BINARY_DIR}/coreSources.txt"
            --ownership "${CMAKE_CURRENT_BINARY_DIR}/coreBoundary-$<CONFIG>.txt"
            --module-deps "${CMAKE_CURRENT_BINARY_DIR}/moduleDeps.cpp"
            --install-manifest "${PROJECT_BINARY_DIR}/install_manifest.txt"
            --cmake "${CMAKE_COMMAND}" --generator "${CMAKE_GENERATOR}"
            --config "$<CONFIG>")
    add_test(NAME testVtPackageComponents
        COMMAND "${PYTHON_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/pxr/base/tf/testenv/testTfPackageComponents.py"
            --source-dir "${PROJECT_SOURCE_DIR}/pxr/base/tf/testenv/testTfPackageComponents"
            --build-dir "${CMAKE_CURRENT_BINARY_DIR}/testVtPackageComponents/$<CONFIG>"
            --prefix "${CMAKE_INSTALL_PREFIX}"
            --core-component vt --binding-component vtBoostPython
            --cmake "${CMAKE_COMMAND}" --generator "${CMAKE_GENERATOR}"
            --config "$<CONFIG>")
endif()

pxr_build_test(testVtCpp
    LIBRARIES
        tf
        gf
        vt
    CPPFILES
        testenv/testVtCpp.cpp
)
pxr_build_test(testVtArrayEditCpp
    LIBRARIES
        tf
        gf
        vt
    CPPFILES
        testenv/testVtArrayEdit.cpp
)
pxr_test_scripts(
    testenv/testVtBufferProtocol.py
    testenv/testVtArray.py
    testenv/testVtArrayEdit.py
    testenv/testVtValue.py
)
pxr_register_test(testVtBufferProtocol
    PYTHON
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testVtBufferProtocol"
)
pxr_register_test(testVtValue
    PYTHON
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testVtValue"
)
pxr_register_test(testVtArray
    PYTHON
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testVtArray"
)
pxr_register_test(testVtArrayEdit
    PYTHON
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testVtArrayEdit"
)
pxr_register_test(testVtCpp
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testVtCpp"
)
pxr_register_test(testVtArrayEditCpp
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testVtArrayEditCpp"
)
