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

set(_hdEmbree_libraries
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

if (PXR_ENABLE_OPENQMC_SUPPORT)
    list(APPEND _hdEmbree_libraries OpenQMC::OpenQMC)
endif()

pxr_plugin(hdEmbree
   LIBRARIES
        ${_hdEmbree_libraries}

    INCLUDE_DIRS
        ${_hdEmbree_include_dirs}

    PUBLIC_CLASSES
        config
        instancer
        light
        lightSamplers
        material
        mesh
        meshSamplers
        mxcppAdapter
        pxrIES/ies
        pxrIES/pxrIES
        renderBuffer
        renderer
        rendererPlugin
        renderDelegate
        renderPass
        sampler
        MaterialXCpp/graph
        MaterialXCpp/slots
        MaterialXCpp/nodeRegistry
        MaterialXCpp/materials/bsdf
        MaterialXCpp/materials/standardSurface
        MaterialXCpp/materials/openPbr
        MaterialXCpp/materials/usdPreviewSurface
        MaterialXCpp/nodes/mathNodes
        MaterialXCpp/nodes/adjustmentNodes
        MaterialXCpp/nodes/channelNodes
        MaterialXCpp/nodes/conditionalNodes
        MaterialXCpp/nodes/compositingNodes
        MaterialXCpp/nodes/geometricNodes
        MaterialXCpp/nodes/applicationNodes
        MaterialXCpp/nodes/textureNodes
        MaterialXCpp/nodes/proceduralNodes
        MaterialXCpp/nodes/colorNodes

    PUBLIC_HEADERS
        context.h
        renderParam.h
        sampling.h
        pxrPbrt/pbrtUtils.h
        MaterialXCpp/types.h
        MaterialXCpp/mxcpp_math.h
        MaterialXCpp/textureSystem.h
        MaterialXCpp/mxcpp_value.h
        MaterialXCpp/mxcpp_graph_types.h
        MaterialXCpp/slots.h

    PRIVATE_CLASSES
        debugCodes
        implicitSurfaceSceneIndexPlugin
        oiioTextureSystem

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
endif()

if (Imath_FOUND)
    set(__MXCPP_TEST_IMATH_LIBS "Imath::Imath")
else()
    set(__MXCPP_TEST_IMATH_LIBS ${OPENEXR_Imath_LIBRARY})
endif()

pxr_build_test(testMaterialXCpp
    LIBRARIES
        ${__MXCPP_TEST_IMATH_LIBS}
    CPPFILES
        MaterialXCpp/tests/testMaterialXCppMain.cpp
        MaterialXCpp/tests/testMaterialXCppBsdf.cpp
        MaterialXCpp/tests/testMaterialXCppNodes.cpp
        MaterialXCpp/tests/testMaterialXCppMaterials.cpp
        MaterialXCpp/tests/testMaterialXCppGraph.cpp
        MaterialXCpp/materials/bsdf.cpp
        MaterialXCpp/materials/standardSurface.cpp
        MaterialXCpp/materials/openPbr.cpp
        MaterialXCpp/materials/usdPreviewSurface.cpp
        MaterialXCpp/slots.cpp
        MaterialXCpp/nodeRegistry.cpp
        MaterialXCpp/graph.cpp
        MaterialXCpp/nodes/mathNodes.cpp
        MaterialXCpp/nodes/adjustmentNodes.cpp
        MaterialXCpp/nodes/channelNodes.cpp
        MaterialXCpp/nodes/conditionalNodes.cpp
        MaterialXCpp/nodes/compositingNodes.cpp
        MaterialXCpp/nodes/geometricNodes.cpp
        MaterialXCpp/nodes/applicationNodes.cpp
        MaterialXCpp/nodes/textureNodes.cpp
        MaterialXCpp/nodes/proceduralNodes.cpp
        MaterialXCpp/nodes/colorNodes.cpp
)

if (PXR_ENABLE_OPENQMC_SUPPORT)
    target_compile_definitions(hdEmbree PRIVATE PXR_HDEMBREE_ENABLE_OPENQMC=1)
endif()
