install(
    DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/"
    DESTINATION lib/python/renderLab
    FILES_MATCHING
        PATTERN "*.py"
        PATTERN "plugInfo.json"
)

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/plugInfo.json"
    "{\n    \"Includes\": [ \"renderLab/\" ]\n}\n")
install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/plugInfo.json"
    DESTINATION lib/python
)

get_property(_pxrExtraPluginsHelp CACHE PXR_EXTRA_PLUGINS PROPERTY HELPSTRING)
list(APPEND PXR_EXTRA_PLUGINS "lib/python")
set(PXR_EXTRA_PLUGINS "${PXR_EXTRA_PLUGINS}"
    CACHE INTERNAL "${_pxrExtraPluginsHelp}")
