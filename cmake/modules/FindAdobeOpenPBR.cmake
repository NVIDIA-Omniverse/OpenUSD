#
# Copyright 2026 Pixar
#
# Licensed under the terms set forth in the LICENSE.txt file available at
# https://openusd.org/license.
#

find_path(ADOBEOPENPBR_INCLUDE_DIR
    NAMES openpbr/openpbr.h
    PATHS
        ${ADOBEOPENPBR_ROOT}
        $ENV{ADOBEOPENPBR_ROOT}
    PATH_SUFFIXES
        include
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(AdobeOpenPBR
    REQUIRED_VARS
        ADOBEOPENPBR_INCLUDE_DIR
)

if (ADOBEOPENPBR_FOUND AND NOT TARGET AdobeOpenPBR::AdobeOpenPBR)
    add_library(AdobeOpenPBR::AdobeOpenPBR INTERFACE IMPORTED)
    set_target_properties(AdobeOpenPBR::AdobeOpenPBR PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${ADOBEOPENPBR_INCLUDE_DIR}"
    )
endif()

mark_as_advanced(ADOBEOPENPBR_INCLUDE_DIR)
