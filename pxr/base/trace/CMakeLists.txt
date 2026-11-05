set(PXR_PREFIX pxr/base)
set(PXR_PACKAGE trace)

pxr_library(trace
    LIBRARIES
        arch
        js
        tf
        TBB::tbb

    PUBLIC_CLASSES
        aggregateTree
        aggregateNode
        category
        collection
        collectionNotice
        collector
        counterAccumulator
        dataBuffer
        dynamicKey
        event
        eventContainer
        eventData
        eventList
        eventNode
        eventTree
        key
        reporter
        reporterBase
        reporterDataSourceBase
        reporterDataSourceCollection
        reporterDataSourceCollector
        serialization
        staticKeyData
        threads

    PRIVATE_CLASSES
        aggregateTreeBuilder
        eventTreeBuilder
        jsonSerialization

    PUBLIC_HEADERS
        api.h
        concurrentList.h
        stringHash.h
        trace.h

    DOXYGEN_FILES
        overview.dox
        detailedOverview.dox
)

pxr_module(trace
    LIBRARY trace

    CPPFILES
        module.cpp
        wrapAggregateNode.cpp
        wrapAggregateTree.cpp
        wrapCollector.cpp
        wrapReporter.cpp
        wrapTestTrace.cpp

    PYTHON_FILES
        __init__.py
        __main__.py
)

if(PXR_BUILD_TESTS AND PXR_ENABLE_PYTHON_SUPPORT AND BUILD_SHARED_LIBS
        AND NOT PXR_BUILD_MONOLITHIC)
    get_target_property(traceCoreHeaders trace PXR_PUBLIC_HEADERS)
    list(REMOVE_DUPLICATES traceCoreHeaders)
    list(SORT traceCoreHeaders)
    string(REPLACE ";" "\n" traceCoreHeaders "${traceCoreHeaders}")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/coreHeaders.txt" "${traceCoreHeaders}\n")
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/coreBoundary-$<CONFIG>.txt"
        CONTENT "links\t$<TARGET_PROPERTY:trace,LINK_LIBRARIES>\ninterface_links\t$<TARGET_PROPERTY:trace,INTERFACE_LINK_LIBRARIES>\ndefinitions\t$<TARGET_PROPERTY:trace,COMPILE_DEFINITIONS>\ninterface_definitions\t$<TARGET_PROPERTY:trace,INTERFACE_COMPILE_DEFINITIONS>\n")
    add_test(NAME testTraceCoreDependencyBoundary
        COMMAND "${PYTHON_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/testenv/testTraceCoreDependencyBoundary.py"
            --source-dir "${CMAKE_CURRENT_SOURCE_DIR}/testenv/testTraceCoreDependencyBoundary"
            --build-dir "${CMAKE_CURRENT_BINARY_DIR}/testTraceCoreDependencyBoundary/$<CONFIG>"
            --prefix "${CMAKE_INSTALL_PREFIX}"
            --headers "${CMAKE_CURRENT_BINARY_DIR}/coreHeaders.txt"
            --ownership "${CMAKE_CURRENT_BINARY_DIR}/coreBoundary-$<CONFIG>.txt"
            --install-manifest "${PROJECT_BINARY_DIR}/install_manifest.txt"
            --cmake "${CMAKE_COMMAND}" --generator "${CMAKE_GENERATOR}"
            --config "$<CONFIG>")
    if(WIN32)
        add_test(NAME testTracePackageComponents
            COMMAND "${PYTHON_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/pxr/base/tf/testenv/testTfPackageComponents.py"
                --source-dir "${PROJECT_SOURCE_DIR}/pxr/base/tf/testenv/testTfPackageComponents"
                --build-dir "${CMAKE_CURRENT_BINARY_DIR}/testTracePackageComponents/$<CONFIG>"
                --prefix "${CMAKE_INSTALL_PREFIX}"
                --core-component trace --binding-component tfBoostPython
                --cmake "${CMAKE_COMMAND}" --generator "${CMAKE_GENERATOR}"
                --config "$<CONFIG>")
    endif()
endif()

pxr_build_test(testTraceCategory
    LIBRARIES
        trace
    CPPFILES
        testenv/testTraceCategory.cpp
)

if(PXR_ENABLE_PYTHON_SUPPORT)
    pxr_build_test(testTracePythonEvents
        LIBRARIES trace tf
        CPPFILES testenv/testTracePythonEvents.cpp)
    if(TARGET testTracePythonEvents)
        target_compile_definitions(testTracePythonEvents PRIVATE
            Py_LIMITED_API=0x030A0000)
        pxr_register_test(testTracePythonEvents
            COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testTracePythonEvents")
    endif()
endif()

pxr_build_test(testTraceData
    LIBRARIES
        trace
    CPPFILES
        testenv/testTraceData.cpp
)

pxr_build_test(testTraceHash
    LIBRARIES
        trace
    CPPFILES
        testenv/testTraceHash.cpp
)

pxr_build_test(testTraceMacros
    LIBRARIES
        trace
    CPPFILES
        testenv/testTraceMacros.cpp
)

pxr_build_test(testTraceOverhead
    LIBRARIES
        trace
    CPPFILES
        testenv/testTraceOverhead.cpp
)

pxr_build_test(testTraceThreading
    LIBRARIES
        trace
    CPPFILES
        testenv/testTraceThreading.cpp
)

pxr_build_test(testTraceSerialization
    LIBRARIES
        trace
    CPPFILES
        testenv/testTraceSerialization.cpp
)

pxr_build_test(testTraceCounters
    LIBRARIES
        trace
    CPPFILES
        testenv/testTraceCounters.cpp
)

pxr_build_test(testTraceMarkers
    LIBRARIES
        trace
    CPPFILES
        testenv/testTraceMarkers.cpp
)

pxr_build_test(testTraceReportPerf
    LIBRARIES
        trace
    CPPFILES
        testenv/testTraceReportPerf.cpp
)

pxr_build_test(testTraceEventContainer
    LIBRARIES
        trace
    CPPFILES
        testenv/testTraceEventContainer.cpp
)

pxr_test_scripts(
    testenv/testTrace.py
    testenv/testTraceRecursion.py
    testenv/testTraceReports.py
    testenv/testTraceReporter.py
    testenv/testTraceReporterLoadTrace.py
)

pxr_register_test(testTraceCategory
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testTraceCategory"
)

pxr_register_test(testTraceData
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testTraceData"
)

pxr_register_test(testTraceHash
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testTraceHash"
)

pxr_register_test(testTraceMacros
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testTraceMacros"
)

pxr_register_test(testTraceThreading
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testTraceThreading"
)

pxr_register_test(testTrace
    PYTHON
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testTrace"
)

pxr_register_test(testTraceRecursion1
    PYTHON
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testTraceRecursion"
    TESTENV testenv/testTraceRecursion.testenv
    DIFF_COMPARE recursion_5begins.out
)

pxr_register_test(testTraceRecursion2
    PYTHON
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testTraceRecursion"
    TESTENV testenv/testTraceRecursion.testenv
    DIFF_COMPARE recursion_typical.out
)

pxr_register_test(testTraceRecursion3
    PYTHON
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testTraceRecursion"
    TESTENV testenv/testTraceRecursion.testenv
    DIFF_COMPARE recursion_newnode.out
)

pxr_register_test(testTraceRecursion4
    PYTHON
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testTraceRecursion"
    TESTENV testenv/testTraceRecursion.testenv
    DIFF_COMPARE recursion_inner.out
)

pxr_register_test(testTraceRecursion5
    PYTHON
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testTraceRecursion"
    TESTENV testenv/testTraceRecursion.testenv
    DIFF_COMPARE recursion_1817.out
)

pxr_register_test(testTraceRecursion6
    PYTHON
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testTraceRecursion"
    TESTENV testenv/testTraceRecursion.testenv
    DIFF_COMPARE recursion_branch.out
)

pxr_register_test(testTraceRecursion7
    PYTHON
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testTraceRecursion"
    TESTENV testenv/testTraceRecursion.testenv
    DIFF_COMPARE recursion_marker_merge.out
)

pxr_register_test(testTraceReports
    PYTHON
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testTraceReports"
)

pxr_register_test(testTraceReporter
    PYTHON
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testTraceReporter"
)

pxr_register_test(testTraceReporterLoadTrace
    PYTHON
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testTraceReporterLoadTrace"
)

pxr_register_test(testTraceSerialization
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testTraceSerialization"
)

pxr_register_test(testTraceCounters
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testTraceCounters"
)

pxr_register_test(testTraceMarkers
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testTraceMarkers"
)

pxr_register_test(testTraceReportPerf
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testTraceReportPerf"
)    

pxr_register_test(testTraceEventContainer
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testTraceEventContainer"
)
