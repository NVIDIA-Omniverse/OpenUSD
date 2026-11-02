cmake_minimum_required(VERSION 3.26)
project(TfPackageComponents LANGUAGES CXX)

if(PREFIND_PYTHON)
    find_package(Python3 REQUIRED COMPONENTS Interpreter Development Development.SABIModule)
endif()
if(PACKAGE_COMPONENT STREQUAL "default")
    find_package(pxr CONFIG REQUIRED PATHS "${USD_PREFIX}" NO_DEFAULT_PATH)
elseif(PACKAGE_COMPONENT STREQUAL "optional")
    find_package(pxr CONFIG REQUIRED OPTIONAL_COMPONENTS unavailable
        PATHS "${USD_PREFIX}" NO_DEFAULT_PATH)
    if(pxr_unavailable_FOUND)
        message(FATAL_ERROR "An unknown optional component was reported available")
    endif()
else()
    find_package(pxr CONFIG REQUIRED COMPONENTS ${PACKAGE_COMPONENT}
        PATHS "${USD_PREFIX}" NO_DEFAULT_PATH)
endif()

if(PACKAGE_COMPONENT STREQUAL "tf" OR PACKAGE_COMPONENT STREQUAL "vt")
    if(TARGET Python3::Python OR NOT TARGET Python3::SABIModule)
        message(FATAL_ERROR "Core discovery must supply only the stable Python target")
    endif()
    if(NOT PXR_LIBRARIES STREQUAL PACKAGE_COMPONENT)
        message(FATAL_ERROR "Core component exposes unrelated libraries: ${PXR_LIBRARIES}")
    endif()
    set(componentIncludes "")
    set(componentCheck "")
    if(PACKAGE_COMPONENT STREQUAL "vt")
        set(componentIncludes "#include <pxr/base/vt/value.h>")
        set(componentCheck "PXR_NS::VtValue vtValue(42); valid = valid && vtValue.Get<int>() == 42;")
    endif()
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/consumer.cpp" "
#include <pxr/base/tf/token.h>
#include <pxr/base/tf/pyLock.h>
#include <cstdio>
${componentIncludes}
static_assert(PY_MINOR_VERSION == ${PYTHON_MINOR}, \"Incorrect consumer Python headers\");
int main() {
    Py_Initialize();
    PXR_NS::TfPyLock lock;
    PXR_NS::TfToken token(\"version-neutral consumer\");
    PyObject *value = PyLong_FromLong(42);
    if (!value) return 1;
    bool valid = PyLong_AsLong(value) == 42;
    Py_DECREF(value);
    ${componentCheck}
    std::puts(Py_GetVersion());
    return valid && token.GetString() == \"version-neutral consumer\" ? 0 : 1;
}
")
    add_executable(tfPackageConsumer "${CMAKE_CURRENT_BINARY_DIR}/consumer.cpp")
    target_compile_features(tfPackageConsumer PRIVATE cxx_std_17)
    target_compile_definitions(tfPackageConsumer PRIVATE Py_LIMITED_API=0x030A0000
        NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(tfPackageConsumer PRIVATE ${PACKAGE_COMPONENT})
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/consumer-$<CONFIG>.txt"
        CONTENT "$<TARGET_FILE:tfPackageConsumer>\n")
elseif(NOT PACKAGE_COMPONENT STREQUAL "optional")
    if(NOT TARGET Python3::Python)
        message(FATAL_ERROR "Binding discovery must supply the versioned Python target")
    endif()
endif()
