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

add_subdirectory(renderer/materials/BSDL)

set(_hdEmbree_libraries
    BSDL::BSDL
    BSDL_spectral_luts
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

    CPPFILES
        renderer/colorManagement.cpp
        renderer/aov/aovOutput.cpp
        renderer/camera/camera.cpp
        renderer/geometry/displacementEvaluation.cpp
        renderer/geometry/wireframe.cpp
        renderer/integrator/ambientOcclusion.cpp
        renderer/integrator/lighting.cpp
        renderer/integrator/pathIntegrator.cpp
        renderer/integrator/surfaceShading.cpp
        renderer/integrator/unlitIntegrator.cpp
        renderer/integrator/visibility.cpp
        renderer/integrator/volumeTransport.cpp
        renderer/lights/cylinderLight.cpp
        renderer/lights/diskLight.cpp
        renderer/lights/distantLight.cpp
        renderer/lights/domeLight.cpp
        renderer/lights/rectLight.cpp
        renderer/lights/sphereLight.cpp

    PRIVATE_CLASSES
        delegate/adaptiveSubdivision
        delegate/basisCurves
        delegate/instancer
        delegate/light
        renderer/lights/lightSampler
        renderer/lights/lightSamplerCommon
        renderer/lights/lightRegistry
        delegate/material
        delegate/mesh
        renderer/geometry/curveEmbree
        renderer/geometry/curveGeometry
        renderer/geometry/curveSamplers
        renderer/geometry/curveTopology
        renderer/geometry/intersectionFilter
        renderer/geometry/meshSamplers
        renderer/integrator/medium
        renderer/materials/mxcppAdapter
        renderer/lights/pxrIES/ies
        renderer/lights/pxrIES/pxrIES
        delegate/renderBuffer
        renderer/renderer
        delegate/rendererPlugin
        delegate/renderDelegate
        delegate/renderPass
        renderer/geometry/primvarSampler
        renderer/materials/MaterialXCpp/graph
        renderer/materials/MaterialXCpp/slots
        renderer/materials/MaterialXCpp/nodeRegistry
        renderer/materials/MaterialXCpp/materials/bsdf
        renderer/materials/MaterialXCpp/materials/bsdf/energyCompensation
        renderer/materials/MaterialXCpp/materials/bsdf/thinFilm
        renderer/materials/MaterialXCpp/materials/bsdf/microfacet
        renderer/materials/MaterialXCpp/materials/bsdf/diffuse
        renderer/materials/MaterialXCpp/materials/bsdf/reflectionOnlyInterfaces
        renderer/materials/MaterialXCpp/materials/bsdf/dielectric
        renderer/materials/MaterialXCpp/materials/bsdf/legacySurface
        renderer/materials/MaterialXCpp/materials/bsdf/closureTraversal
        renderer/materials/MaterialXCpp/materials/adobeOpenPbr
        renderer/materials/MaterialXCpp/materials/disneyPrincipled
        renderer/materials/MaterialXCpp/materials/gltfPbr
        renderer/materials/MaterialXCpp/materials/standardSurface
        renderer/materials/MaterialXCpp/materials/openPbr
        renderer/materials/MaterialXCpp/materials/usdPreviewSurface
        renderer/materials/MaterialXCpp/nodes/mathNodes
        renderer/materials/MaterialXCpp/nodes/convolutionNodes
        renderer/materials/MaterialXCpp/nodes/nprNodes
        renderer/materials/MaterialXCpp/nodes/adjustmentNodes
        renderer/materials/MaterialXCpp/nodes/channelNodes
        renderer/materials/MaterialXCpp/nodes/conditionalNodes
        renderer/materials/MaterialXCpp/nodes/compositingNodes
        renderer/materials/MaterialXCpp/nodes/geometricNodes
        renderer/materials/MaterialXCpp/nodes/applicationNodes
        renderer/materials/MaterialXCpp/nodes/textureNodes
        renderer/materials/MaterialXCpp/nodes/texture3dNodes
        renderer/materials/MaterialXCpp/nodes/proceduralNodes
        renderer/materials/MaterialXCpp/nodes/procedural2dNodes
        renderer/materials/MaterialXCpp/nodes/procedural3dNodes
        renderer/materials/MaterialXCpp/nodes/pbrNodes
        renderer/materials/MaterialXCpp/nodes/colorTransformNodes

        renderer/debugCodes
        delegate/implicitSurfaceSceneIndexPlugin
        renderer/materials/oiioTextureSystem
        renderer/integrator/sss
        renderer/geometry/surfaceDerivatives
        renderer/integrator/closureClassification

    PRIVATE_HEADERS
        renderer/api.h
        renderer/colorManagement.h
        renderer/embreeCompat.h
        renderer/heroWavelength.h
        renderer/rayUtil.h
        renderer/renderSettings.h
        renderer/rendererMath.h
        renderer/geometry/context.h
        renderer/geometry/displacementEvaluation.h
        renderer/geometry/normalTransforms.h
        renderer/geometry/primvarSampling.h
        renderer/geometry/triangleMesh.h
        renderer/geometry/wireframe.h
        renderer/integrator/shadingNormal.h
        renderer/integrator/transportPolicy.h
        delegate/displacement.h
        renderer/lights/light.h
        renderer/lights/lightLinking.h
        renderer/lights/lightSamplerDispatch.h
        renderer/materials/materialEvalContext.h
        renderer/materials/material.h
        renderer/renderBuffer.h
        delegate/renderParam.h
        renderer/sampling/sampling.h
        renderer/materials/MaterialXCpp/mathTypes.h
        renderer/materials/MaterialXCpp/shadingContext.h
        renderer/materials/MaterialXCpp/spectral.h
        renderer/materials/MaterialXCpp/surfaceClosure.h
        renderer/materials/MaterialXCpp/surfaceShaderUtils.h
        renderer/materials/MaterialXCpp/materials/closureTree.h
        renderer/materials/MaterialXCpp/materials/surfaceInteraction.h
        renderer/materials/MaterialXCpp/materials/bsdf/mathPrimitives.h
        renderer/materials/MaterialXCpp/materials/bsdf/shadingFrame.h
        renderer/materials/MaterialXCpp/materials/bsdf/fresnel.h
        renderer/materials/MaterialXCpp/materials/bsdf/sheen.h
        renderer/materials/MaterialXCpp/materials/bsdf/ggxEnergyLut.h
        renderer/materials/MaterialXCpp/materials/bsdf/dielectricReflFrontLut.h
        renderer/materials/MaterialXCpp/materials/bsdf/dielectricBothLut.h
        renderer/materials/MaterialXCpp/materials/bsdf/dielectricTransmissionLut.h
        renderer/materials/MaterialXCpp/paramMap.h
        renderer/materials/MaterialXCpp/textureSystem.h
        renderer/materials/MaterialXCpp/value.h
        renderer/materials/MaterialXCpp/nodes/helpers/valueHelper.h
        renderer/materials/MaterialXCpp/graphTypes.h
        renderer/materials/MaterialXCpp/slots.h

    RESOURCE_FILES
        plugInfo.json

    DOXYGEN_FILES
        overview.dox
)

# First-party plugin headers use paths relative to the hdEmbree source root.
# Keep this build-only: hdEmbree does not install or expose a C++ header API.
target_include_directories(hdEmbree
    PRIVATE
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
)

if (WIN32)
    # Tests link private plugin symbols, which Windows does not export by default.
    set_target_properties(hdEmbree PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

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

if (TARGET testHdEmbreeLightSamplers)
    _pxr_init_rpath(_hdEmbreeLightSamplersTestRpath "tests")
    _pxr_add_rpath(
        _hdEmbreeLightSamplersTestRpath "${CMAKE_INSTALL_PREFIX}/lib")
    _pxr_add_rpath(
        _hdEmbreeLightSamplersTestRpath "${CMAKE_INSTALL_PREFIX}/plugin/usd")
    _pxr_install_rpath(
        _hdEmbreeLightSamplersTestRpath testHdEmbreeLightSamplers)
endif()

pxr_register_test(testHdEmbreeLightSamplers
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testHdEmbreeLightSamplers"
    EXPECTED_RETURN_CODE 0
)
endif()

# Keep renderer-specific imaging coverage with hdEmbree while reusing the
# generic usdImagingGL drawing harness.
pxr_build_test(testHdEmbreeBasisCurvesImaging
    LIBRARIES
        arch
        garch
        glf
        hd
        hdEmbree
        hdSt
        hdx
        hgi
        sdf
        tf
        usd
        usdGeom
        usdImaging
        usdImagingGL
        usdUtils
        vt
    CPPFILES
        ../../../usdImaging/usdImagingGL/unitTestGLDrawing.cpp
        testenv/testHdEmbreeBasisCurvesImaging.cpp
)

if (TARGET testHdEmbreeBasisCurvesImaging)
    _pxr_init_rpath(_hdEmbreeBasisCurvesImagingTestRpath "tests")
    _pxr_add_rpath(
        _hdEmbreeBasisCurvesImagingTestRpath
        "${CMAKE_INSTALL_PREFIX}/lib")
    _pxr_add_rpath(
        _hdEmbreeBasisCurvesImagingTestRpath
        "${CMAKE_INSTALL_PREFIX}/plugin/usd")
    _pxr_install_rpath(
        _hdEmbreeBasisCurvesImagingTestRpath
        testHdEmbreeBasisCurvesImaging)
endif()

pxr_register_test(testHdEmbreeBasisCurvesImaging
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testHdEmbreeBasisCurvesImaging -renderer HdEmbreeRendererPlugin -offscreen -stage test.usda -write testHdEmbreeBasisCurvesImaging.png"
    IMAGE_DIFF_COMPARE
        testHdEmbreeBasisCurvesImaging_000.png
        testHdEmbreeBasisCurvesImaging_001.png
    FAIL_PERCENT 1
    PERCEPTUAL
    EXPECTED_RETURN_CODE 0
    TESTENV testenv/testHdEmbreeBasisCurvesImaging
    ENV
        HDEMBREE_JITTER_CAMERA=0
        HDEMBREE_AMBIENT_OCCLUSION_SAMPLES=0
        HDEMBREE_SAMPLES_TO_CONVERGENCE=1
        USDIMAGINGGL_ENGINE_ENABLE_SCENE_INDEX=false
)

pxr_register_test(testHdEmbreeBasisCurvesImaging_SceneIndex
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testHdEmbreeBasisCurvesImaging -renderer HdEmbreeRendererPlugin -offscreen -stage test.usda -write testHdEmbreeBasisCurvesImaging.png"
    IMAGE_DIFF_COMPARE
        testHdEmbreeBasisCurvesImaging_000.png
        testHdEmbreeBasisCurvesImaging_001.png
    FAIL_PERCENT 1
    PERCEPTUAL
    EXPECTED_RETURN_CODE 0
    TESTENV testenv/testHdEmbreeBasisCurvesImaging
    ENV
        HDEMBREE_JITTER_CAMERA=0
        HDEMBREE_AMBIENT_OCCLUSION_SAMPLES=0
        HDEMBREE_SAMPLES_TO_CONVERGENCE=1
        USDIMAGINGGL_ENGINE_ENABLE_SCENE_INDEX=true
)

pxr_build_test(testHdEmbreeBasisCurves
    LIBRARIES
        hd
        hdEmbree
        hdx
        sdf
        tf
        usd
        usdGeom
        usdImaging
        vt
    CPPFILES
        ../../hdx/unitTestUtils.cpp
        testenv/testHdEmbreeBasisCurves.cpp
)

if (TARGET testHdEmbreeBasisCurves)
    _pxr_init_rpath(_hdEmbreeBasisCurvesTestRpath "tests")
    _pxr_add_rpath(
        _hdEmbreeBasisCurvesTestRpath "${CMAKE_INSTALL_PREFIX}/lib")
    _pxr_add_rpath(
        _hdEmbreeBasisCurvesTestRpath "${CMAKE_INSTALL_PREFIX}/plugin/usd")
    _pxr_install_rpath(
        _hdEmbreeBasisCurvesTestRpath testHdEmbreeBasisCurves)
endif()

pxr_register_test(testHdEmbreeBasisCurves
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testHdEmbreeBasisCurves"
    EXPECTED_RETURN_CODE 0
)

pxr_build_test(testHdEmbreeCurveIntersections
    LIBRARIES
        gf
        hd
        hdEmbree
        tf
        vt
        ${EMBREE_LIBRARY}
    CPPFILES
        testenv/testHdEmbreeCurveIntersections.cpp
)

if (TARGET testHdEmbreeCurveIntersections)
    _pxr_init_rpath(_hdEmbreeCurveIntersectionsTestRpath "tests")
    _pxr_add_rpath(
        _hdEmbreeCurveIntersectionsTestRpath "${CMAKE_INSTALL_PREFIX}/lib")
    _pxr_add_rpath(
        _hdEmbreeCurveIntersectionsTestRpath
        "${CMAKE_INSTALL_PREFIX}/plugin/usd")
    _pxr_install_rpath(
        _hdEmbreeCurveIntersectionsTestRpath
        testHdEmbreeCurveIntersections)
endif()

pxr_register_test(testHdEmbreeCurveIntersections
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testHdEmbreeCurveIntersections"
    EXPECTED_RETURN_CODE 0
)

pxr_build_test(testHdEmbreeSampling
    LIBRARIES
        tf
    CPPFILES
        testenv/testHdEmbreeSampling.cpp
)

pxr_register_test(testHdEmbreeSampling
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testHdEmbreeSampling"
    EXPECTED_RETURN_CODE 0
)

pxr_build_test(testHdEmbreeMaterialNormals
    LIBRARIES
        gf
        vt
        ${EMBREE_LIBRARY}
    CPPFILES
        testenv/testHdEmbreeMaterialNormals.cpp
)

pxr_register_test(testHdEmbreeMaterialNormals
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testHdEmbreeMaterialNormals"
    EXPECTED_RETURN_CODE 0
)

pxr_build_test(testHdEmbreeWireframe
    LIBRARIES
        hd
        hdEmbree
    CPPFILES
        testenv/testHdEmbreeWireframe.cpp
)

if (TARGET testHdEmbreeWireframe)
    _pxr_init_rpath(_hdEmbreeWireframeTestRpath "tests")
    _pxr_add_rpath(
        _hdEmbreeWireframeTestRpath "${CMAKE_INSTALL_PREFIX}/lib")
    _pxr_add_rpath(
        _hdEmbreeWireframeTestRpath "${CMAKE_INSTALL_PREFIX}/plugin/usd")
    _pxr_install_rpath(
        _hdEmbreeWireframeTestRpath testHdEmbreeWireframe)
endif()

pxr_register_test(testHdEmbreeWireframe
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testHdEmbreeWireframe"
    EXPECTED_RETURN_CODE 0
)

pxr_build_test(testHdEmbreeSubdivision
    LIBRARIES
        gf
        hd
        hdEmbree
        sdf
        tf
        usd
        usdImaging
        vt
    CPPFILES
        testenv/testHdEmbreeSubdivision.cpp
)

if (TARGET testHdEmbreeSubdivision)
    _pxr_init_rpath(_hdEmbreeSubdivisionTestRpath "tests")
    _pxr_add_rpath(
        _hdEmbreeSubdivisionTestRpath "${CMAKE_INSTALL_PREFIX}/lib")
    _pxr_add_rpath(
        _hdEmbreeSubdivisionTestRpath "${CMAKE_INSTALL_PREFIX}/plugin/usd")
    _pxr_install_rpath(
        _hdEmbreeSubdivisionTestRpath testHdEmbreeSubdivision)
endif()

pxr_register_test(testHdEmbreeSubdivision
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testHdEmbreeSubdivision"
    EXPECTED_RETURN_CODE 0
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

pxr_test_scripts(
    testenv/testHdEmbreeSchemaValidation.py
)

if (TARGET usdGenSchema_script)
    pxr_register_test(testHdEmbreeSchemaValidation
        PYTHON
        COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testHdEmbreeSchemaValidation --schema=${CMAKE_CURRENT_SOURCE_DIR}/schema/schema.usda --generated-schema=${CMAKE_CURRENT_SOURCE_DIR}/schema/generatedSchema.usda --plug-info=${CMAKE_CURRENT_SOURCE_DIR}/plugInfo.json --usd-gen-schema=${CMAKE_INSTALL_PREFIX}/bin/usdGenSchema"
        EXPECTED_RETURN_CODE 0
    )
endif()

pxr_build_test(testHdEmbreeRenderSetup
    LIBRARIES
        hd
        hdEmbree
        tf
    CPPFILES
        testenv/testHdEmbreeRenderSetup.cpp
)

if (TARGET testHdEmbreeRenderSetup)
    _pxr_init_rpath(_hdEmbreeRenderSetupTestRpath "tests")
    _pxr_add_rpath(
        _hdEmbreeRenderSetupTestRpath "${CMAKE_INSTALL_PREFIX}/lib")
    _pxr_add_rpath(
        _hdEmbreeRenderSetupTestRpath "${CMAKE_INSTALL_PREFIX}/plugin/usd")
    _pxr_install_rpath(
        _hdEmbreeRenderSetupTestRpath testHdEmbreeRenderSetup)
endif()

pxr_register_test(testHdEmbreeRenderSetup
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testHdEmbreeRenderSetup"
    EXPECTED_RETURN_CODE 0
)

if (Imath_FOUND)
    set(__MXCPP_TEST_IMATH_LIBS "Imath::Imath")
else()
    set(__MXCPP_TEST_IMATH_LIBS ${OPENEXR_Imath_LIBRARY})
endif()

set(__MXCPP_TEST_LIBS
    BSDL::BSDL
    BSDL_spectral_luts
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
        renderer/materials/MaterialXCpp/tests/testMaterialXCppMain.cpp
        renderer/materials/MaterialXCpp/tests/testMaterialXCppBsdf.cpp
        renderer/materials/MaterialXCpp/tests/testMaterialXCppNodes.cpp
        renderer/materials/MaterialXCpp/tests/testMaterialXCppMaterials.cpp
        renderer/materials/MaterialXCpp/tests/testMaterialXCppGraph.cpp
        renderer/materials/MaterialXCpp/tests/testMaterialXCppAdapter.cpp
        renderer/colorManagement.cpp
        renderer/materials/MaterialXCpp/tests/testClosureClassification.cpp
        renderer/integrator/closureClassification.cpp
        renderer/integrator/medium.cpp
        renderer/materials/mxcppAdapter.cpp
        renderer/integrator/sss.cpp
        renderer/materials/MaterialXCpp/materials/bsdf.cpp
        renderer/materials/MaterialXCpp/materials/bsdf/energyCompensation.cpp
        renderer/materials/MaterialXCpp/materials/bsdf/thinFilm.cpp
        renderer/materials/MaterialXCpp/materials/bsdf/microfacet.cpp
        renderer/materials/MaterialXCpp/materials/bsdf/diffuse.cpp
        renderer/materials/MaterialXCpp/materials/bsdf/reflectionOnlyInterfaces.cpp
        renderer/materials/MaterialXCpp/materials/bsdf/dielectric.cpp
        renderer/materials/MaterialXCpp/materials/bsdf/legacySurface.cpp
        renderer/materials/MaterialXCpp/materials/bsdf/closureTraversal.cpp
        renderer/materials/MaterialXCpp/materials/adobeOpenPbr.cpp
        renderer/materials/MaterialXCpp/materials/disneyPrincipled.cpp
        renderer/materials/MaterialXCpp/materials/gltfPbr.cpp
        renderer/materials/MaterialXCpp/materials/standardSurface.cpp
        renderer/materials/MaterialXCpp/materials/openPbr.cpp
        renderer/materials/MaterialXCpp/materials/usdPreviewSurface.cpp
        renderer/materials/MaterialXCpp/slots.cpp
        renderer/materials/MaterialXCpp/nodeRegistry.cpp
        renderer/materials/MaterialXCpp/graph.cpp
        renderer/materials/MaterialXCpp/nodes/mathNodes.cpp
        renderer/materials/MaterialXCpp/nodes/convolutionNodes.cpp
        renderer/materials/MaterialXCpp/nodes/nprNodes.cpp
        renderer/materials/MaterialXCpp/nodes/adjustmentNodes.cpp
        renderer/materials/MaterialXCpp/nodes/channelNodes.cpp
        renderer/materials/MaterialXCpp/nodes/conditionalNodes.cpp
        renderer/materials/MaterialXCpp/nodes/compositingNodes.cpp
        renderer/materials/MaterialXCpp/nodes/geometricNodes.cpp
        renderer/materials/MaterialXCpp/nodes/applicationNodes.cpp
        renderer/materials/MaterialXCpp/nodes/textureNodes.cpp
        renderer/materials/MaterialXCpp/nodes/texture3dNodes.cpp
        renderer/materials/MaterialXCpp/nodes/proceduralNodes.cpp
        renderer/materials/MaterialXCpp/nodes/procedural2dNodes.cpp
        renderer/materials/MaterialXCpp/nodes/procedural3dNodes.cpp
        renderer/materials/MaterialXCpp/nodes/pbrNodes.cpp
        renderer/materials/MaterialXCpp/nodes/colorTransformNodes.cpp
)

add_executable(generateHdEmbreeDielectricTransmissionLut EXCLUDE_FROM_ALL
    renderer/materials/MaterialXCpp/lut/generateDielectricTransmissionLut.cpp)
target_link_libraries(generateHdEmbreeDielectricTransmissionLut
    PRIVATE BSDL::BSDL Threads::Threads ${__MXCPP_TEST_IMATH_LIBS})
target_compile_features(generateHdEmbreeDielectricTransmissionLut
    PRIVATE cxx_std_17)

pxr_register_test(testMaterialXCpp
    COMMAND "${CMAKE_INSTALL_PREFIX}/tests/testMaterialXCpp"
    EXPECTED_RETURN_CODE 0
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
