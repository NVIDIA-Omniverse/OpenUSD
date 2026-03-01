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
        pxrIES/ies
        pxrIES/pxrIES
        renderBuffer
        renderer
        rendererPlugin
        renderDelegate
        renderPass
        sampler
        mxLite/graph
        mxLite/nodeRegistry
        mxLite/materials/bsdf
        mxLite/materials/standardSurface
        mxLite/materials/openPbr
        mxLite/materials/usdPreviewSurface
        mxLite/nodes/mathNodes
        mxLite/nodes/adjustmentNodes
        mxLite/nodes/channelNodes
        mxLite/nodes/conditionalNodes
        mxLite/nodes/geometricNodes
        mxLite/nodes/textureNodes
        mxLite/nodes/proceduralNodes
        mxLite/nodes/colorNodes

    PUBLIC_HEADERS
        context.h
        renderParam.h
        pxrPbrt/pbrtUtils.h
        mxLite/types.h

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

pxr_build_test(testMxLite
    LIBRARIES
        tf
        gf
        vt
        sdf
        hd
    CPPFILES
        mxLite/tests/testMxLiteMain.cpp
        mxLite/tests/testMxLiteBsdf.cpp
        mxLite/tests/testMxLiteNodes.cpp
        mxLite/tests/testMxLiteMaterials.cpp
        mxLite/tests/testMxLiteGraph.cpp
        mxLite/materials/bsdf.cpp
        mxLite/materials/standardSurface.cpp
        mxLite/materials/openPbr.cpp
        mxLite/materials/usdPreviewSurface.cpp
        mxLite/nodeRegistry.cpp
        mxLite/graph.cpp
        mxLite/nodes/mathNodes.cpp
        mxLite/nodes/adjustmentNodes.cpp
        mxLite/nodes/channelNodes.cpp
        mxLite/nodes/conditionalNodes.cpp
        mxLite/nodes/geometricNodes.cpp
        mxLite/nodes/textureNodes.cpp
        mxLite/nodes/proceduralNodes.cpp
        mxLite/nodes/colorNodes.cpp
)
