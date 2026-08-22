cmake_minimum_required(VERSION 3.20)

project(TyphoonHoudini VERSION 0.1.0 LANGUAGES CXX)

set(TYPHOON_PACKAGE_NAME "typhoon")

if(NOT HFS AND DEFINED ENV{HFS})
    set(HFS "$ENV{HFS}")
endif()
set(HFS "${HFS}" CACHE PATH "Houdini installation root")
if(NOT HFS)
    message(FATAL_ERROR "Set HFS to a Houdini 22 installation")
endif()
get_filename_component(HFS "${HFS}" ABSOLUTE)

find_package(Houdini REQUIRED CONFIG PATHS "${HFS}/toolkit/cmake"
    NO_DEFAULT_PATH)
if(NOT Houdini_VERSION_MAJOR EQUAL 22)
    message(FATAL_ERROR
        "This package target supports Houdini 22, found ${Houdini_VERSION}")
endif()

set(TYPHOON_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../../../pxr/imaging/plugin/hdEmbree")
get_filename_component(TYPHOON_ROOT "${TYPHOON_ROOT}" ABSOLUTE)
set(TYPHOON_REPOSITORY_ROOT "${TYPHOON_ROOT}/../../../..")
get_filename_component(
    TYPHOON_REPOSITORY_ROOT "${TYPHOON_REPOSITORY_ROOT}" ABSOLUTE)

set(TYPHOON_OPENQMC_ROOT "$ENV{CONDA_PREFIX}" CACHE PATH
    "Root containing the pixi-provided OpenQMC 0.7.1")
find_package(OpenQMC 0.7.1 EXACT CONFIG REQUIRED
    PATHS
        "${TYPHOON_OPENQMC_ROOT}/lib/cmake/OpenQMC"
        "${TYPHOON_OPENQMC_ROOT}/Library/lib/cmake/OpenQMC"
    NO_DEFAULT_PATH)
foreach(_locationProperty
        IMPORTED_LOCATION_RELEASE
        IMPORTED_LOCATION_RELWITHDEBINFO
        IMPORTED_LOCATION_NOCONFIG
        IMPORTED_LOCATION)
    get_target_property(_location OpenQMC::OpenQMC ${_locationProperty})
    if(_location AND NOT _location MATCHES "-NOTFOUND$")
        set(TYPHOON_OPENQMC_RUNTIME "${_location}")
        break()
    endif()
endforeach()
if(NOT TYPHOON_OPENQMC_RUNTIME OR
   TYPHOON_OPENQMC_RUNTIME MATCHES "-NOTFOUND$")
    message(FATAL_ERROR
        "OpenQMC::OpenQMC does not provide a shared-library runtime")
endif()
get_filename_component(TYPHOON_OPENQMC_RUNTIME_NAME
    "${TYPHOON_OPENQMC_RUNTIME}" NAME)

# HoudiniConfig.cmake keeps these libraries outside its public Houdini target.
# Search the layouts used by Linux, Windows, and the macOS application bundle;
# each result remains a cache entry so an SDK with a different layout can be
# configured explicitly.
set(_typhoonHoudiniLibraryRoots "${HFS}")
foreach(_rootVariable _houdini_hfs_root _houdini_install_root)
    if(DEFINED ${_rootVariable})
        list(APPEND _typhoonHoudiniLibraryRoots "${${_rootVariable}}")
    endif()
endforeach()
list(REMOVE_DUPLICATES _typhoonHoudiniLibraryRoots)

set(_typhoonHoudiniLibraryHints)
foreach(_root IN LISTS _typhoonHoudiniLibraryRoots)
    list(APPEND _typhoonHoudiniLibraryHints
        "${_root}/dsolib"
        "${_root}/bin"
        "${_root}/custom/houdini/dsolib"
        "${_root}/Libraries"
        "${_root}/Frameworks/Houdini.framework/Versions/Current/Libraries")
endforeach()
list(REMOVE_DUPLICATES _typhoonHoudiniLibraryHints)

find_library(TYPHOON_EMBREE_LIBRARY embree_sidefx
    HINTS ${_typhoonHoudiniLibraryHints} NO_DEFAULT_PATH REQUIRED)
find_library(TYPHOON_OIIO_LIBRARY OpenImageIO_sidefx
    HINTS ${_typhoonHoudiniLibraryHints} NO_DEFAULT_PATH REQUIRED)
find_library(TYPHOON_OIIO_UTIL_LIBRARY OpenImageIO_Util_sidefx
    HINTS ${_typhoonHoudiniLibraryHints} NO_DEFAULT_PATH REQUIRED)

file(STRINGS "${HFS}/toolkit/include/pxr/pxr.h" _pxrVersionLine
    REGEX "^#define PXR_VERSION [0-9]+$")
string(REGEX REPLACE ".*PXR_VERSION ([0-9]+).*" "\\1"
    TYPHOON_PXR_VERSION "${_pxrVersionLine}")
if(NOT TYPHOON_PXR_VERSION EQUAL 2605)
    message(FATAL_ERROR
        "Houdini 22 target expects OpenUSD 0.26.5, found PXR_VERSION "
        "${TYPHOON_PXR_VERSION}")
endif()

file(STRINGS
    "${HFS}/toolkit/include/embree3/rtcore_config.h"
    _embreeVersionLines REGEX "^#define RTC_VERSION_(MAJOR|MINOR|PATCH) ")
foreach(_component MAJOR MINOR PATCH)
    string(REGEX MATCH
        "RTC_VERSION_${_component} ([0-9]+)"
        _match "${_embreeVersionLines}")
    set(TYPHOON_EMBREE_VERSION_${_component} "${CMAKE_MATCH_1}")
endforeach()
set(TYPHOON_EMBREE_VERSION
    "${TYPHOON_EMBREE_VERSION_MAJOR}.${TYPHOON_EMBREE_VERSION_MINOR}.${TYPHOON_EMBREE_VERSION_PATCH}")
if(NOT TYPHOON_EMBREE_VERSION_MAJOR EQUAL 3)
    message(FATAL_ERROR
        "Houdini package requires Embree 3, found ${TYPHOON_EMBREE_VERSION}")
endif()

file(STRINGS
    "${HFS}/toolkit/include/OpenImageIO/oiioversion.h"
    _oiioVersionLines REGEX "^#define OIIO_VERSION_(MAJOR|MINOR|PATCH) ")
foreach(_component MAJOR MINOR PATCH)
    string(REGEX MATCH
        "OIIO_VERSION_${_component} ([0-9]+)"
        _match "${_oiioVersionLines}")
    set(TYPHOON_OIIO_VERSION_${_component} "${CMAKE_MATCH_1}")
endforeach()
set(TYPHOON_OIIO_VERSION
    "${TYPHOON_OIIO_VERSION_MAJOR}.${TYPHOON_OIIO_VERSION_MINOR}.${TYPHOON_OIIO_VERSION_PATCH}")
if(NOT TYPHOON_OIIO_VERSION_MAJOR EQUAL 2 OR
   NOT TYPHOON_OIIO_VERSION_MINOR EQUAL 5)
    message(FATAL_ERROR
        "Houdini package requires OIIO 2.5, found ${TYPHOON_OIIO_VERSION}")
endif()

set(Imath_DIR "${CMAKE_CURRENT_LIST_DIR}/cmake/Imath")
set(BSDL_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(
    "${TYPHOON_ROOT}/renderer/materials/BSDL"
    "${CMAKE_CURRENT_BINARY_DIR}/BSDL"
    EXCLUDE_FROM_ALL)

function(_typhoon_add_houdini_library target location)
    add_library(${target} SHARED IMPORTED)
    if(WIN32)
        set_target_properties(${target} PROPERTIES
            IMPORTED_IMPLIB "${location}"
            INTERFACE_INCLUDE_DIRECTORIES "${HFS}/toolkit/include")
    else()
        set_target_properties(${target} PROPERTIES
            IMPORTED_LOCATION "${location}"
            INTERFACE_INCLUDE_DIRECTORIES "${HFS}/toolkit/include")
    endif()
endfunction()

_typhoon_add_houdini_library(Embree::Embree
    "${TYPHOON_EMBREE_LIBRARY}")
_typhoon_add_houdini_library(OpenImageIO::OpenImageIO
    "${TYPHOON_OIIO_LIBRARY}")
_typhoon_add_houdini_library(OpenImageIO::OpenImageIO_Util
    "${TYPHOON_OIIO_UTIL_LIBRARY}")

file(GLOB_RECURSE TYPHOON_SOURCES CONFIGURE_DEPENDS
    "${TYPHOON_ROOT}/delegate/*.cpp"
    "${TYPHOON_ROOT}/renderer/*.cpp")
list(FILTER TYPHOON_SOURCES EXCLUDE REGEX "/tests/")
list(FILTER TYPHOON_SOURCES EXCLUDE REGEX "/materials/BSDL/")
list(FILTER TYPHOON_SOURCES EXCLUDE REGEX "/materials/MaterialXCpp/lut/")

add_library(hdEmbree SHARED ${TYPHOON_SOURCES})
target_compile_features(hdEmbree PRIVATE cxx_std_20)
target_compile_definitions(hdEmbree PRIVATE
    HDEMBREE_EXPORTS
    PXR_OIIO_PLUGIN_ENABLED
    TYPHOON_HOUDINI_BUILD
    TYPHOON_HOUDINI_FORCE_MULTISAMPLED)
target_include_directories(hdEmbree PRIVATE "${TYPHOON_ROOT}")
target_link_libraries(hdEmbree PRIVATE
    Houdini
    BSDL::BSDL
    BSDL_spectral_luts
    OpenQMC::OpenQMC
    Embree::Embree
    OpenImageIO::OpenImageIO
    OpenImageIO::OpenImageIO_Util
    Imath::Imath)

if(MSVC)
    target_compile_options(hdEmbree PRIVATE /utf-8 /bigobj)
endif()

get_filename_component(_typhoonEmbreeLibraryDir
    "${TYPHOON_EMBREE_LIBRARY}" DIRECTORY)
get_filename_component(_typhoonOiioLibraryDir
    "${TYPHOON_OIIO_LIBRARY}" DIRECTORY)
set(_typhoonHoudiniRuntimeDirs
    "${_typhoonEmbreeLibraryDir}"
    "${_typhoonOiioLibraryDir}")
list(REMOVE_DUPLICATES _typhoonHoudiniRuntimeDirs)

if(APPLE)
    set(_typhoonBuildRpath
        "${_typhoonHoudiniRuntimeDirs};${TYPHOON_OPENQMC_ROOT}/lib")
    set(_typhoonInstallRpath
        "@loader_path/../../lib;${_typhoonHoudiniRuntimeDirs}")
elseif(UNIX)
    set(_typhoonBuildRpath
        "${_typhoonHoudiniRuntimeDirs};${TYPHOON_OPENQMC_ROOT}/lib")
    set(_typhoonInstallRpath
        "\$ORIGIN/../../lib;${_typhoonHoudiniRuntimeDirs}")
else()
    set(_typhoonBuildRpath "")
    set(_typhoonInstallRpath "")
endif()

set_target_properties(hdEmbree PROPERTIES
    PREFIX ""
    OUTPUT_NAME "hdEmbree"
    BUILD_RPATH "${_typhoonBuildRpath}"
    INSTALL_RPATH "${_typhoonInstallRpath}")

set(PLUG_INFO_LIBRARY_PATH "../hdEmbree${CMAKE_SHARED_LIBRARY_SUFFIX}")
set(PLUG_INFO_RESOURCE_PATH "resources")
set(PLUG_INFO_ROOT "..")
configure_file(
    "${TYPHOON_ROOT}/plugInfo.json"
    "${CMAKE_CURRENT_BINARY_DIR}/plugInfo.json"
    @ONLY)
if(WIN32)
    file(TO_NATIVE_PATH "${HFS}" HFS_NATIVE)
    set(TYPHOON_HUSK_WRAPPER_NAME "husk-typhoon.cmd")
    set(TYPHOON_HUSK_WRAPPER_COMMAND ".\\husk-typhoon.cmd")
    configure_file(
        "${CMAKE_CURRENT_LIST_DIR}/husk-typhoon.cmd.in"
        "${CMAKE_CURRENT_BINARY_DIR}/${TYPHOON_HUSK_WRAPPER_NAME}"
        @ONLY)
else()
    set(TYPHOON_HUSK_WRAPPER_NAME "husk-typhoon")
    set(TYPHOON_HUSK_WRAPPER_COMMAND "./husk-typhoon")
    configure_file(
        "${CMAKE_CURRENT_LIST_DIR}/husk-typhoon.in"
        "${CMAKE_CURRENT_BINARY_DIR}/${TYPHOON_HUSK_WRAPPER_NAME}"
        @ONLY)
endif()
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/doc/package-README.md.in"
    "${CMAKE_CURRENT_BINARY_DIR}/README.md"
    @ONLY)
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/build-info.json.in"
    "${CMAKE_CURRENT_BINARY_DIR}/build-info.json"
    @ONLY)
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/typhoon.json.in"
    "${CMAKE_CURRENT_BINARY_DIR}/typhoon.json"
    @ONLY)

install(TARGETS hdEmbree
    LIBRARY DESTINATION "${TYPHOON_PACKAGE_NAME}/dso/usd_plugins"
    RUNTIME DESTINATION "${TYPHOON_PACKAGE_NAME}/dso/usd_plugins")
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/plugInfo.json"
    "${TYPHOON_ROOT}/schema/generatedSchema.usda"
    "${TYPHOON_ROOT}/schema/schema.usda"
    DESTINATION
        "${TYPHOON_PACKAGE_NAME}/dso/usd_plugins/hdEmbree/resources")
install(FILES "${CMAKE_CURRENT_LIST_DIR}/plugInfo.json"
    DESTINATION "${TYPHOON_PACKAGE_NAME}/dso/usd_plugins")
install(FILES "${CMAKE_CURRENT_LIST_DIR}/UsdRenderers.json"
    DESTINATION "${TYPHOON_PACKAGE_NAME}")
install(FILES
    "${CMAKE_CURRENT_LIST_DIR}/soho/parameters/HdEmbreeRendererPlugin_Global.ds"
    DESTINATION "${TYPHOON_PACKAGE_NAME}/soho/parameters")
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/README.md"
    "${CMAKE_CURRENT_BINARY_DIR}/build-info.json"
    DESTINATION "${TYPHOON_PACKAGE_NAME}")
install(FILES "${TYPHOON_REPOSITORY_ROOT}/LICENSE.txt"
    DESTINATION "${TYPHOON_PACKAGE_NAME}/licenses"
    RENAME OpenUSD-LICENSE.txt)
install(FILES "${TYPHOON_REPOSITORY_ROOT}/NOTICE.txt"
    DESTINATION "${TYPHOON_PACKAGE_NAME}/licenses"
    RENAME OpenUSD-NOTICE.txt)
install(FILES "${CMAKE_CURRENT_LIST_DIR}/doc/licenses/OpenQMC-LICENSE.txt"
    DESTINATION "${TYPHOON_PACKAGE_NAME}/licenses")
install(PROGRAMS
    "${CMAKE_CURRENT_BINARY_DIR}/${TYPHOON_HUSK_WRAPPER_NAME}"
    DESTINATION "${TYPHOON_PACKAGE_NAME}")
install(FILES "${TYPHOON_OPENQMC_RUNTIME}"
    DESTINATION "${TYPHOON_PACKAGE_NAME}/lib")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/typhoon.json" DESTINATION .)

message(STATUS
    "Typhoon Houdini target: Houdini ${Houdini_VERSION}, OpenUSD 0.26.5, "
    "Embree ${TYPHOON_EMBREE_VERSION}, OIIO ${TYPHOON_OIIO_VERSION}")
