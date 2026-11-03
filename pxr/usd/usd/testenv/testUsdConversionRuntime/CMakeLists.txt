cmake_minimum_required(VERSION 3.26)
project(UsdConversionRuntime LANGUAGES CXX)
find_package(pxr CONFIG REQUIRED PATHS "${USD_PREFIX}" NO_DEFAULT_PATH)
set(core usd)
if(TARGET usd_m)
    set(core usd_m)
endif()
add_library(usdConversionNative SHARED native.cpp)
target_link_libraries(usdConversionNative PRIVATE ${core})
target_compile_features(usdConversionNative PRIVATE cxx_std_17)
target_compile_definitions(usdConversionNative PRIVATE CHECK_RUNTIME_CONTRACTS
    Py_LIMITED_API=0x030A0000 Py_NO_LINK_LIB)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/header.cpp" "
#include \"pxr/usd/usd/pyConversionRuntime.h\"
#if defined(EXPECT_FULL_API) && defined(Py_LIMITED_API)
#error The core must not restrict its binding consumers to the Limited API.
#endif
")
foreach(mode Full Limited)
    add_library(runtimeHeader${mode} OBJECT "${CMAKE_CURRENT_BINARY_DIR}/header.cpp")
    target_link_libraries(runtimeHeader${mode} PRIVATE ${core})
    target_compile_features(runtimeHeader${mode} PRIVATE cxx_std_17)
endforeach()
target_compile_definitions(runtimeHeaderFull PRIVATE EXPECT_FULL_API)
target_compile_definitions(runtimeHeaderLimited PRIVATE Py_LIMITED_API=0x030A0000)
add_library(runtimeSourceLimited OBJECT ../../pyConversionRuntime.cpp)
target_link_libraries(runtimeSourceLimited PRIVATE ${core})
target_compile_features(runtimeSourceLimited PRIVATE cxx_std_17)
target_compile_definitions(runtimeSourceLimited PRIVATE USD_EXPORTS
    Py_LIMITED_API=0x030A0000 Py_NO_LINK_LIB MFB_PACKAGE_NAME=usd MFB_ALT_PACKAGE_NAME=usd MFB_PACKAGE_MODULE=Usd)
foreach(target usdConversionNative runtimeHeaderFull runtimeHeaderLimited runtimeSourceLimited)
    if(MSVC)
        target_compile_options(${target} PRIVATE /showIncludes /bigobj)
    else()
        target_compile_options(${target} PRIVATE -H)
    endif()
endforeach()
if(WIN32)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/native-$<CONFIG>.txt"
    CONTENT "$<TARGET_FILE:usdConversionNative>")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/runtime-$<CONFIG>.txt"
    CONTENT "$<TARGET_FILE_DIR:TBB::tbb>")
