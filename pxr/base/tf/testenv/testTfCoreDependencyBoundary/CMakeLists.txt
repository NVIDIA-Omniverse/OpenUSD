cmake_minimum_required(VERSION 3.26)
project(TfCoreDependencyBoundary LANGUAGES CXX)

if(WIN32)
    # Avoid Windows min/max macros when exercising the complete C++ header set.
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# Exercise the installed package as an independent consumer, without inheriting
# include directories, compiler definitions or link libraries from USD's build.
find_package(pxr CONFIG REQUIRED PATHS "${USD_PREFIX}" NO_DEFAULT_PATH)

function(check_core_dependencies target)
    get_property(visited GLOBAL PROPERTY checkedTargets)
    if(target IN_LIST visited)
        return()
    endif()
    set_property(GLOBAL APPEND PROPERTY checkedTargets "${target}")
    get_target_property(dependencies ${target} INTERFACE_LINK_LIBRARIES)
    foreach(dependency IN LISTS dependencies)
        string(TOLOWER "${dependency}" dependencyLower)
        get_filename_component(dependencyName "${dependencyLower}" NAME)
        if(dependencyLower MATCHES "(^|::)(boost|python)(::|$)"
                OR dependencyName MATCHES "^(lib|usd_)?boost"
                OR dependencyName MATCHES "^(lib|usd_)?python([_.]|$)")
            message(FATAL_ERROR "${target} exposes binding dependency ${dependency}")
        endif()
        if(WIN32 AND (dependency STREQUAL "Python3::Python"
                OR dependencyName MATCHES "^(lib)?python[0-9][0-9]+"))
            message(FATAL_ERROR "${target} exposes versioned Python dependency ${dependency}")
        endif()
        if(TARGET "${dependency}")
            check_core_dependencies("${dependency}")
        endif()
    endforeach()
endfunction()
check_core_dependencies(tf)

file(STRINGS "${CORE_HEADERS}" headers)
set(headerSource "
#if defined(Py_LIMITED_API) && !defined(TF_CORE_EXPECT_LIMITED_API)
#error Full-API consumer unexpectedly inherited Py_LIMITED_API
#endif
#include \"pxr/base/tf/pySafePython.h\"
")
foreach(header IN LISTS headers)
    string(APPEND headerSource "#include \"pxr/base/tf/${header}\"\n")
endforeach()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/headers.cpp" "${headerSource}")

foreach(mode Full Limited)
    add_library(tfHeaders${mode} OBJECT "${CMAKE_CURRENT_BINARY_DIR}/headers.cpp")
    target_compile_features(tfHeaders${mode} PRIVATE cxx_std_17)
    target_link_libraries(tfHeaders${mode} PRIVATE tf)
    if(mode STREQUAL "Limited")
        target_compile_definitions(tfHeaders${mode} PRIVATE
            Py_LIMITED_API=0x030A0000 TF_CORE_EXPECT_LIMITED_API)
    endif()
    if(MSVC)
        target_compile_options(tfHeaders${mode} PRIVATE /showIncludes)
    else()
        target_compile_options(tfHeaders${mode} PRIVATE -H)
    endif()
endforeach()

add_executable(tfCoreConsumer consumer.cpp)
target_compile_features(tfCoreConsumer PRIVATE cxx_std_17)
target_compile_definitions(tfCoreConsumer PRIVATE Py_LIMITED_API=0x030A0000)
target_link_libraries(tfCoreConsumer PRIVATE tf)

file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/consumer-$<CONFIG>.txt"
    CONTENT "$<TARGET_FILE:tfCoreConsumer>\n")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/linker.txt"
    CONTENT "${CMAKE_LINKER}\n")
