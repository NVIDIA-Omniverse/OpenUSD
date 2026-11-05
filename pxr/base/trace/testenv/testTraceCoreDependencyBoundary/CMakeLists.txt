cmake_minimum_required(VERSION 3.26)
project(TraceCoreDependencyBoundary LANGUAGES CXX)

find_package(pxr CONFIG REQUIRED COMPONENTS trace PATHS "${USD_PREFIX}" NO_DEFAULT_PATH)
if(NOT PXR_LIBRARIES STREQUAL "trace")
    message(FATAL_ERROR "Trace core component exposes unrelated libraries: ${PXR_LIBRARIES}")
endif()
if(WIN32 AND TARGET Python3::Python)
    message(FATAL_ERROR "Native Trace requested version-specific Python")
endif()
function(check_dependencies target)
    get_property(visited GLOBAL PROPERTY checkedTargets)
    if(target IN_LIST visited)
        return()
    endif()
    set_property(GLOBAL APPEND PROPERTY checkedTargets "${target}")
    get_target_property(dependencies "${target}" INTERFACE_LINK_LIBRARIES)
    foreach(dependency IN LISTS dependencies)
        if(dependency MATCHES "(^|::)(boost|python)(::|$)|BoostPython"
                OR (WIN32 AND dependency STREQUAL "Python3::Python"))
            message(FATAL_ERROR "${target} exposes binding dependency ${dependency}")
        endif()
        if(TARGET "${dependency}")
            check_dependencies("${dependency}")
        endif()
    endforeach()
endfunction()
check_dependencies(trace)

if(MSVC)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    add_compile_options(/showIncludes /bigobj)
else()
    add_compile_options(-H)
endif()
file(STRINGS "${CORE_HEADERS}" headers)
set(headerSource "#include \"pxr/base/tf/pyTracing.h\"\n")
foreach(header IN LISTS headers)
    string(APPEND headerSource "#include \"pxr/base/trace/${header}\"\n")
endforeach()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/headers.cpp" "${headerSource}")
foreach(mode Full Limited)
    add_library(traceHeaders${mode} OBJECT "${CMAKE_CURRENT_BINARY_DIR}/headers.cpp")
    target_link_libraries(traceHeaders${mode} PRIVATE trace)
    target_compile_features(traceHeaders${mode} PRIVATE cxx_std_17)
    if(mode STREQUAL "Limited")
        target_compile_definitions(traceHeaders${mode} PRIVATE Py_LIMITED_API=0x030A0000)
    endif()
endforeach()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/eventValues.cpp" "
#include \"pxr/base/tf/pyTracing.h\"
#ifdef Py_LIMITED_API
#error Core Trace must not impose Limited API restrictions on its consumers
#endif
static_assert(PXR_NS::TfPyTraceCall == PyTrace_CALL);
static_assert(PXR_NS::TfPyTraceException == PyTrace_EXCEPTION);
static_assert(PXR_NS::TfPyTraceLine == PyTrace_LINE);
static_assert(PXR_NS::TfPyTraceReturn == PyTrace_RETURN);
static_assert(PXR_NS::TfPyTraceCCall == PyTrace_C_CALL);
static_assert(PXR_NS::TfPyTraceCException == PyTrace_C_EXCEPTION);
static_assert(PXR_NS::TfPyTraceCReturn == PyTrace_C_RETURN);
")
add_library(traceEventValues OBJECT "${CMAKE_CURRENT_BINARY_DIR}/eventValues.cpp")
target_link_libraries(traceEventValues PRIVATE trace)
target_compile_features(traceEventValues PRIVATE cxx_std_17)

get_filename_component(openusdSource "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
add_library(traceCollectorLimited OBJECT "${openusdSource}/pxr/base/trace/collector.cpp")
target_link_libraries(traceCollectorLimited PRIVATE trace)
target_include_directories(traceCollectorLimited PRIVATE "${openusdSource}")
target_compile_features(traceCollectorLimited PRIVATE cxx_std_17)
target_compile_definitions(traceCollectorLimited PRIVATE TRACE_EXPORTS
    Py_LIMITED_API=0x030A0000 MFB_PACKAGE_NAME=trace MFB_ALT_PACKAGE_NAME=trace MFB_PACKAGE_MODULE=Trace)
add_executable(traceCoreConsumer "${CMAKE_CURRENT_SOURCE_DIR}/../testTracePythonEvents.cpp")
target_link_libraries(traceCoreConsumer PRIVATE trace)
target_compile_features(traceCoreConsumer PRIVATE cxx_std_17)
target_compile_definitions(traceCoreConsumer PRIVATE Py_LIMITED_API=0x030A0000)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/consumer-$<CONFIG>.txt"
    CONTENT "$<TARGET_FILE:traceCoreConsumer>")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/core-$<CONFIG>.txt"
    CONTENT "$<TARGET_FILE:trace>")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/binding-$<CONFIG>.txt"
    CONTENT "$<TARGET_FILE_NAME:python>")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/runtime-$<CONFIG>.txt"
    CONTENT "$<TARGET_FILE_DIR:TBB::tbb>")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/linker.txt" "${CMAKE_LINKER}")
