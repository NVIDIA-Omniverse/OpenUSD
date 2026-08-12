install(
    DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/"
    DESTINATION "${PXR_PYTHON_INSTALL_DIR}/renderLab"
    FILES_MATCHING
        PATTERN "*.py"
        PATTERN "testenv" EXCLUDE
)

install(
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/plugInfo.json"
    DESTINATION "lib/usd/renderLab/resources"
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
