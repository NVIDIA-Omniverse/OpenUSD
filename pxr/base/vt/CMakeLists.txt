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
        arrayPyBuffer
        valueFromPython
        valueToPython
        wrapArray

    PUBLIC_HEADERS
        addExtVisitValueType.h
        api.h
        traits.h
        typeHeaders.h
        visitValue.h
    
    PYTHON_PUBLIC_HEADERS
        pyOperators.h
        wrapArrayEdit.h

    PYMODULE_CPPFILES
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

    PYMODULE_FILES
        __init__.py

    DOXYGEN_FILES
        overview.dox
)

if(PXR_ENABLE_PYTHON_SUPPORT)
    # Reusable binding adapters, without another compiled library. Core Vt
    # owns the registry; modules own the registered extraction callbacks.
    add_library(vtBoostPython INTERFACE)
    target_link_libraries(vtBoostPython INTERFACE tfBoostPython)
    if(NOT PXR_BUILD_MONOLITHIC)
        target_link_libraries(vtBoostPython INTERFACE vt)
    endif()
    set(vtBindingHeaders
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

if(PXR_BUILD_TESTS AND PXR_ENABLE_PYTHON_SUPPORT AND BUILD_SHARED_LIBS)
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
    testenv/testVtArray.py
    testenv/testVtArrayEdit.py
    testenv/testVtValue.py
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
