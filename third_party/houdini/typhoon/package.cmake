cmake_minimum_required(VERSION 3.20)

set(_buildDir "${CMAKE_CURRENT_LIST_DIR}/build")
set(_packageRoot "${_buildDir}/package")

if(NOT EXISTS "${_buildDir}/cmake_install.cmake")
    message(FATAL_ERROR
        "The Houdini build is not configured; run houdini-configure first")
endif()

# CMake installs do not remove files from an older package layout. Recreate the
# staging root so it always represents exactly what should be copied into the
# Houdini user packages directory.
file(REMOVE_RECURSE "${_packageRoot}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${_buildDir}"
    COMMAND_ERROR_IS_FATAL ANY)
