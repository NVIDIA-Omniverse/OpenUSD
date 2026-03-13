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

pxr_plugin(hdEmbree
   LIBRARIES
        tf
        vt
        gf
        work
        hd
        hdsi
        hio
        ${EMBREE_LIBRARY}

    INCLUDE_DIRS
        ${EMBREE_INCLUDE_DIR}

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
        MaterialXCpp/nodeRegistry
        MaterialXCpp/materials/bsdf
        MaterialXCpp/materials/standardSurface
        MaterialXCpp/materials/openPbr
        MaterialXCpp/materials/usdPreviewSurface
        MaterialXCpp/nodes/mathNodes
        MaterialXCpp/nodes/adjustmentNodes
        MaterialXCpp/nodes/channelNodes
        MaterialXCpp/nodes/conditionalNodes
        MaterialXCpp/nodes/geometricNodes
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
        MaterialXCpp/mxcpp_value.h
        MaterialXCpp/mxcpp_graph_types.h

    PRIVATE_CLASSES
        debugCodes
        implicitSurfaceSceneIndexPlugin

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
        MaterialXCpp/nodeRegistry.cpp
        MaterialXCpp/graph.cpp
        MaterialXCpp/nodes/mathNodes.cpp
        MaterialXCpp/nodes/adjustmentNodes.cpp
        MaterialXCpp/nodes/channelNodes.cpp
        MaterialXCpp/nodes/conditionalNodes.cpp
        MaterialXCpp/nodes/geometricNodes.cpp
        MaterialXCpp/nodes/textureNodes.cpp
        MaterialXCpp/nodes/proceduralNodes.cpp
        MaterialXCpp/nodes/colorNodes.cpp
)
