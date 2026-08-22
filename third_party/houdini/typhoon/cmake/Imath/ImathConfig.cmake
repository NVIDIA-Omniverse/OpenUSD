if(NOT TARGET Imath::Imath)
    if(NOT TARGET Houdini::Dep::Imath_sidefx)
        message(FATAL_ERROR "Houdini's SideFX Imath target is unavailable")
    endif()

    add_library(Imath::Imath INTERFACE IMPORTED GLOBAL)
    set_target_properties(Imath::Imath PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${HFS}/toolkit/include"
        INTERFACE_LINK_LIBRARIES "Houdini::Dep::Imath_sidefx")
endif()

set(Imath_FOUND TRUE)
set(Imath_VERSION "3.2")
