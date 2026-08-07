install(
    DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/"
    DESTINATION lib/python/renderLab
    FILES_MATCHING
        PATTERN "*.py"
        PATTERN "plugInfo.json"
        PATTERN "testenv" EXCLUDE
)

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/plugInfo.json"
    "{\n    \"Includes\": [ \"renderLab/\" ]\n}\n")
install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/plugInfo.json"
    DESTINATION lib/python
)

set(PXR_PACKAGE renderLab)
add_custom_target(${PXR_PACKAGE})

pxr_test_scripts(
    testenv/testRenderLabRenderSettings.py
)

pxr_register_test(testRenderLabRenderSettings
    PYTHON
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testRenderLabRenderSettings"
    EXPECTED_RETURN_CODE 0
    ENV
        QT_QPA_PLATFORM=offscreen
)

get_property(_pxrExtraPluginsHelp CACHE PXR_EXTRA_PLUGINS PROPERTY HELPSTRING)
list(APPEND PXR_EXTRA_PLUGINS "lib/python")
set(PXR_EXTRA_PLUGINS "${PXR_EXTRA_PLUGINS}"
    CACHE INTERNAL "${_pxrExtraPluginsHelp}")
