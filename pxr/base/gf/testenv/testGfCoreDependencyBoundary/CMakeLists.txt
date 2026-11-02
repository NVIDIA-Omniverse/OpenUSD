cmake_minimum_required(VERSION 3.26)
project(GfCoreDependencyBoundary LANGUAGES CXX)

find_package(pxr CONFIG REQUIRED COMPONENTS gf PATHS "${USD_PREFIX}" NO_DEFAULT_PATH)
if(NOT PXR_LIBRARIES STREQUAL "gf")
    message(FATAL_ERROR "Gf component exposes unrelated libraries: ${PXR_LIBRARIES}")
endif()
if(WIN32 AND TARGET Python3::Python)
    message(FATAL_ERROR "Gf core discovery requested version-specific Python")
endif()

function(check_dependencies target)
    get_property(visited GLOBAL PROPERTY checkedTargets)
    if(target IN_LIST visited)
        return()
    endif()
    set_property(GLOBAL APPEND PROPERTY checkedTargets "${target}")
    get_target_property(dependencies ${target} INTERFACE_LINK_LIBRARIES)
    foreach(dependency IN LISTS dependencies)
        if(dependency MATCHES "(^|::)(boost|python)(::|$)|tfBoostPython"
                OR (WIN32 AND dependency STREQUAL "Python3::Python"))
            message(FATAL_ERROR "${target} exposes binding/versioned Python dependency ${dependency}")
        endif()
        if(TARGET "${dependency}")
            check_dependencies("${dependency}")
        endif()
    endforeach()
endfunction()
check_dependencies(gf)

if(WIN32)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
file(STRINGS "${CORE_HEADERS}" headers)
set(headerSource "")
foreach(header IN LISTS headers)
    string(APPEND headerSource "#include \"pxr/base/gf/${header}\"\n")
endforeach()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/headers.cpp" "${headerSource}")
foreach(mode Full Limited)
    add_library(gfHeaders${mode} OBJECT "${CMAKE_CURRENT_BINARY_DIR}/headers.cpp")
    target_compile_features(gfHeaders${mode} PRIVATE cxx_std_17)
    target_link_libraries(gfHeaders${mode} PRIVATE gf)
    if(mode STREQUAL "Limited")
        target_compile_definitions(gfHeaders${mode} PRIVATE Py_LIMITED_API=0x030A0000)
    endif()
    if(MSVC)
        target_compile_options(gfHeaders${mode} PRIVATE /showIncludes)
    else()
        target_compile_options(gfHeaders${mode} PRIVATE -H)
    endif()
endforeach()

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/consumer.cpp" "
#include <pxr/base/gf/matrix3d.h>
#include <pxr/base/gf/vec3d.h>
int main() {
    PXR_NS::GfMatrix3d identity(1.0);
    PXR_NS::GfVec3d vector(1.0, 2.0, 3.0);
    return identity.GetDeterminant() == 1.0 && vector.GetLengthSq() == 14.0 ? 0 : 1;
}
")
add_executable(gfCoreConsumer "${CMAKE_CURRENT_BINARY_DIR}/consumer.cpp")
target_compile_features(gfCoreConsumer PRIVATE cxx_std_17)
target_link_libraries(gfCoreConsumer PRIVATE gf)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/consumer-$<CONFIG>.txt"
    CONTENT "$<TARGET_FILE:gfCoreConsumer>\n")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/core-$<CONFIG>.txt"
    CONTENT "$<TARGET_FILE:gf>\n")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/linker.txt" "${CMAKE_LINKER}\n")
