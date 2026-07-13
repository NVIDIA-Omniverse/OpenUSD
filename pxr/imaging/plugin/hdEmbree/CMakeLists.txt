set(PXR_PREFIX pxr/imaging)
set(PXR_PACKAGE hdEmbree)

if(NOT EMBREE_FOUND)
    message(WARNING "Not building ${PXR_PACKAGE} because of missing dependency: Embree")
    return()
endif()
if (NOT ${PXR_BUILD_GPU_SUPPORT})
    message(STATUS
        "Skipping ${PXR_PACKAGE} because PXR_BUILD_GPU_SUPPORT is OFF")
    return()
endif()

add_subdirectory(renderer/BSDL)

set(_hdEmbree_libraries
    BSDL::BSDL
    tf
    vt
    gf
    work
    hd
    hdsi
    hio
    ${EMBREE_LIBRARY}
)

set(_hdEmbree_include_dirs
    ${EMBREE_INCLUDE_DIR}
)

if (PXR_BUILD_OPENIMAGEIO_PLUGIN)
    if (Imath_FOUND)
        set(__HDEMBREE_OIIO_IMATH_LIBS "Imath::Imath")
        set(__HDEMBREE_OIIO_IMATH_INCLUDE "")
    else()
        set(__HDEMBREE_OIIO_IMATH_INCLUDE ${OPENEXR_INCLUDE_DIRS})
        set(__HDEMBREE_OIIO_IMATH_LIBS ${OPENEXR_LIBRARIES})
    endif()

    list(APPEND _hdEmbree_libraries
        ${OIIO_LIBRARIES}
        ${__HDEMBREE_OIIO_IMATH_LIBS}
    )
    list(APPEND _hdEmbree_include_dirs
        ${OIIO_INCLUDE_DIRS}
        ${__HDEMBREE_OIIO_IMATH_INCLUDE}
    )
endif()

list(APPEND _hdEmbree_libraries OpenQMC::OpenQMC)

if (PXR_ENABLE_ADOBE_OPENPBR_SUPPORT)
    list(APPEND _hdEmbree_libraries
        AdobeOpenPBR::AdobeOpenPBR
        glm::glm
    )
endif()

pxr_plugin(hdEmbree
    INCLUDE_SCHEMA_FILES
    SCHEMA_CLASSES_FILE schema/generatedSchema.classes.txt

   LIBRARIES
        ${_hdEmbree_libraries}

    INCLUDE_DIRS
        ${_hdEmbree_include_dirs}

    PUBLIC_CLASSES
        renderer/config
        delegate/instancer
        delegate/light
        renderer/lightSamplers
        delegate/material
        delegate/mesh
        renderer/meshSamplers
        renderer/medium
        renderer/mxcppAdapter
        renderer/pxrIES/ies
        renderer/pxrIES/pxrIES
        delegate/renderBuffer
        renderer/renderer
        delegate/rendererPlugin
        delegate/renderDelegate
        delegate/renderPass
        renderer/sampler
        renderer/MaterialXCpp/graph
        renderer/MaterialXCpp/slots
        renderer/MaterialXCpp/nodeRegistry
        renderer/MaterialXCpp/materials/bsdf
        renderer/MaterialXCpp/materials/adobeOpenPbr
        renderer/MaterialXCpp/materials/disneyPrincipled
        renderer/MaterialXCpp/materials/gltfPbr
        renderer/MaterialXCpp/materials/standardSurface
        renderer/MaterialXCpp/materials/openPbr
        renderer/MaterialXCpp/materials/usdPreviewSurface
        renderer/MaterialXCpp/nodes/mathNodes
        renderer/MaterialXCpp/nodes/convolutionNodes
        renderer/MaterialXCpp/nodes/nprNodes
        renderer/MaterialXCpp/nodes/adjustmentNodes
        renderer/MaterialXCpp/nodes/channelNodes
        renderer/MaterialXCpp/nodes/conditionalNodes
        renderer/MaterialXCpp/nodes/compositingNodes
        renderer/MaterialXCpp/nodes/geometricNodes
        renderer/MaterialXCpp/nodes/applicationNodes
        renderer/MaterialXCpp/nodes/textureNodes
        renderer/MaterialXCpp/nodes/texture3dNodes
        renderer/MaterialXCpp/nodes/proceduralNodes
        renderer/MaterialXCpp/nodes/procedural2dNodes
        renderer/MaterialXCpp/nodes/procedural3dNodes
        renderer/MaterialXCpp/nodes/pbrNodes
        renderer/MaterialXCpp/nodes/colorTransformNodes

    PUBLIC_HEADERS
        renderer/context.h
        renderer/light.h
        renderer/lightLinking.h
        renderer/material.h
        renderer/renderBuffer.h
        delegate/renderParam.h
        renderer/sampling.h
        renderer/pxrPbrt/pbrtUtils.h
        renderer/MaterialXCpp/mathTypes.h
        renderer/MaterialXCpp/shadingContext.h
        renderer/MaterialXCpp/spectral.h
        renderer/MaterialXCpp/surfaceClosure.h
        renderer/MaterialXCpp/surfaceShaderUtils.h
        renderer/MaterialXCpp/materials/closureTree.h
        renderer/MaterialXCpp/paramMap.h
        renderer/MaterialXCpp/textureSystem.h
        renderer/MaterialXCpp/value.h
        renderer/MaterialXCpp/nodes/helpers/valueHelper.h
        renderer/MaterialXCpp/graphTypes.h
        renderer/MaterialXCpp/slots.h

    PRIVATE_CLASSES
        renderer/debugCodes
        delegate/implicitSurfaceSceneIndexPlugin
        renderer/oiioTextureSystem
        renderer/sss

    RESOURCE_FILES
        plugInfo.json

    DOXYGEN_FILES
        overview.dox
)

if (X11_FOUND OR APPLE)
pxr_build_test(testHdEmbree
    LIBRARIES
        arch
        garch
        hdEmbree
        hdSt
        hdx
        hio
    CPPFILES
        testenv/testHdEmbree.cpp
)

pxr_build_test(testHdEmbreeLightSamplers
    LIBRARIES
        gf
        hdEmbree
        tf
    CPPFILES
        testenv/testHdEmbreeLightSamplers.cpp
)
endif()

pxr_build_test(testHdEmbreeSampling
    LIBRARIES
        tf
    CPPFILES
        testenv/testHdEmbreeSampling.cpp
)

pxr_build_test(testHdEmbreeLightLinking
    LIBRARIES
        hd
        hdEmbree
        tf
        vt
    CPPFILES
        testenv/testHdEmbreeLightLinking.cpp
)

if (TARGET testHdEmbreeLightLinking)
    _pxr_init_rpath(_hdEmbreeLightLinkingTestRpath "tests")
    _pxr_add_rpath(
        _hdEmbreeLightLinkingTestRpath "${CMAKE_INSTALL_PREFIX}/lib")
    _pxr_add_rpath(
        _hdEmbreeLightLinkingTestRpath "${CMAKE_INSTALL_PREFIX}/plugin/usd")
    _pxr_install_rpath(
        _hdEmbreeLightLinkingTestRpath testHdEmbreeLightLinking)
endif()

pxr_register_test(testHdEmbreeLightLinking
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testHdEmbreeLightLinking"
    EXPECTED_RETURN_CODE 0
)

pxr_build_test(testHdEmbreeRenderSettings
    LIBRARIES
        arch
        hd
        hdEmbree
        plug
        tf
        usd
        usdRender
    CPPFILES
        testenv/testHdEmbreeRenderSettings.cpp
)

if (TARGET testHdEmbreeRenderSettings)
    _pxr_init_rpath(_hdEmbreeRenderSettingsTestRpath "tests")
    _pxr_add_rpath(_hdEmbreeRenderSettingsTestRpath "${CMAKE_INSTALL_PREFIX}/lib")
    _pxr_add_rpath(_hdEmbreeRenderSettingsTestRpath "${CMAKE_INSTALL_PREFIX}/plugin/usd")
    _pxr_install_rpath(_hdEmbreeRenderSettingsTestRpath testHdEmbreeRenderSettings)
endif()

pxr_register_test(testHdEmbreeRenderSettings
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testHdEmbreeRenderSettings"
    ENV
        ${PXR_PLUGINPATH_NAME}=${CMAKE_INSTALL_PREFIX}/plugin/usd
    EXPECTED_RETURN_CODE 0
)

if (Imath_FOUND)
    set(__MXCPP_TEST_IMATH_LIBS "Imath::Imath")
else()
    set(__MXCPP_TEST_IMATH_LIBS ${OPENEXR_Imath_LIBRARY})
endif()

set(__MXCPP_TEST_LIBS
    BSDL::BSDL
    ${__MXCPP_TEST_IMATH_LIBS}
    ${EMBREE_LIBRARY}
)
if (PXR_ENABLE_ADOBE_OPENPBR_SUPPORT)
    list(APPEND __MXCPP_TEST_LIBS
        AdobeOpenPBR::AdobeOpenPBR
        glm::glm
    )
endif()

pxr_build_test(testMaterialXCpp
    LIBRARIES
        ${__MXCPP_TEST_LIBS}
        gf
        hd
        sdf
        tf
        vt
    CPPFILES
        renderer/MaterialXCpp/tests/testMaterialXCppMain.cpp
        renderer/MaterialXCpp/tests/testMaterialXCppBsdf.cpp
        renderer/MaterialXCpp/tests/testMaterialXCppNodes.cpp
        renderer/MaterialXCpp/tests/testMaterialXCppMaterials.cpp
        renderer/MaterialXCpp/tests/testMaterialXCppGraph.cpp
        renderer/MaterialXCpp/tests/testMaterialXCppAdapter.cpp
        renderer/medium.cpp
        renderer/mxcppAdapter.cpp
        renderer/sss.cpp
        renderer/MaterialXCpp/materials/bsdf.cpp
        renderer/MaterialXCpp/materials/adobeOpenPbr.cpp
        renderer/MaterialXCpp/materials/disneyPrincipled.cpp
        renderer/MaterialXCpp/materials/gltfPbr.cpp
        renderer/MaterialXCpp/materials/standardSurface.cpp
        renderer/MaterialXCpp/materials/openPbr.cpp
        renderer/MaterialXCpp/materials/usdPreviewSurface.cpp
        renderer/MaterialXCpp/slots.cpp
        renderer/MaterialXCpp/nodeRegistry.cpp
        renderer/MaterialXCpp/graph.cpp
        renderer/MaterialXCpp/nodes/mathNodes.cpp
        renderer/MaterialXCpp/nodes/convolutionNodes.cpp
        renderer/MaterialXCpp/nodes/nprNodes.cpp
        renderer/MaterialXCpp/nodes/adjustmentNodes.cpp
        renderer/MaterialXCpp/nodes/channelNodes.cpp
        renderer/MaterialXCpp/nodes/conditionalNodes.cpp
        renderer/MaterialXCpp/nodes/compositingNodes.cpp
        renderer/MaterialXCpp/nodes/geometricNodes.cpp
        renderer/MaterialXCpp/nodes/applicationNodes.cpp
        renderer/MaterialXCpp/nodes/textureNodes.cpp
        renderer/MaterialXCpp/nodes/texture3dNodes.cpp
        renderer/MaterialXCpp/nodes/proceduralNodes.cpp
        renderer/MaterialXCpp/nodes/procedural2dNodes.cpp
        renderer/MaterialXCpp/nodes/procedural3dNodes.cpp
        renderer/MaterialXCpp/nodes/pbrNodes.cpp
        renderer/MaterialXCpp/nodes/colorTransformNodes.cpp
)

if (TARGET testHdEmbreeSampling)
    target_link_libraries(testHdEmbreeSampling
        PRIVATE OpenQMC::OpenQMC)
endif()

if (PXR_ENABLE_ADOBE_OPENPBR_SUPPORT)
    target_compile_definitions(hdEmbree
        PRIVATE PXR_HDEMBREE_ENABLE_ADOBE_OPENPBR=1)
    if (TARGET testMaterialXCpp)
        target_compile_definitions(testMaterialXCpp
            PRIVATE PXR_HDEMBREE_ENABLE_ADOBE_OPENPBR=1)
    endif()
endif()
