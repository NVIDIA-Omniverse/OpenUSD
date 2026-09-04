//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//

#include "pxr/usdImaging/usdImagingGL/unitTestGLDrawing.h"

#include "pxr/base/gf/frustum.h"
#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/tf/diagnostic.h"
#include "pxr/base/tf/errorMark.h"
#include "pxr/base/tf/stringUtils.h"
#include "pxr/imaging/hd/renderBuffer.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/hdSt/textureUtils.h"
#include "pxr/imaging/hdx/pickTask.h"
#include "pxr/imaging/hgi/hgi.h"
#include "pxr/imaging/hgi/texture.h"
#include "pxr/imaging/hgi/types.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usdImaging/usdImagingGL/engine.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

class _TestDrawing final : public UsdImagingGL_UnitTestGLDrawing
{
public:
    void InitTest() override;
    void DrawTest(bool offscreen) override;
    void ShutdownTest() override;

private:
    UsdImagingGLRenderParams _GetRenderParams() const;
    void _Render(int imageIndex);
    bool _ReadCenterElementId(int* value) const;
    bool _CaptureColor(std::vector<std::uint8_t>* bytes) const;

    UsdStageRefPtr _stage;
    std::unique_ptr<UsdImagingGLEngine> _engine;
    GfFrustum _frustum;
    GfMatrix4d _viewMatrix{1.0};
};

void
_TestDrawing::InitTest()
{
    _stage = UsdStage::Open(GetStageFilePath());
    if (!_stage) {
        TF_RUNTIME_ERROR("Could not open stage '%s'", GetStageFilePath().c_str());
        return;
    }

    _engine = std::make_unique<UsdImagingGLEngine>(
        _stage->GetPseudoRoot().GetPath(), SdfPathVector());
    if (!_GetRenderer().IsEmpty() &&
        !_engine->SetRendererPlugin(_GetRenderer())) {
        TF_RUNTIME_ERROR(
            "Could not set renderer plugin '%s'", _GetRenderer().GetText());
        return;
    }

    _engine->SetSelectionColor(GfVec4f(1.0f, 1.0f, 0.0f, 1.0f));
    _engine->SetRendererSetting(
        TfToken("ty:convergedSamplesPerPixel"), VtValue(1));
    _engine->SetRendererSetting(
        TfToken("ty:minSamplesBeforeAdaptive"), VtValue(1));

    const double aspect =
        static_cast<double>(GetWidth()) / static_cast<double>(GetHeight());
    _frustum.SetPerspective(60.0, aspect, 1.0, 100.0);
    _viewMatrix.SetTranslate(GfVec3d(0.0, 0.0, -4.0));
}

UsdImagingGLRenderParams
_TestDrawing::_GetRenderParams() const
{
    UsdImagingGLRenderParams params;
    params.drawMode = GetDrawMode();
    params.enableLighting = false;
    params.cullStyle = GetCullStyle();
    params.highlight = true;
    params.clearColor = GetClearColor();
    return params;
}

void
_TestDrawing::_Render(int imageIndex)
{
    _engine->SetCameraState(
        _viewMatrix, _frustum.ComputeProjectionMatrix());
    _engine->SetRenderViewport(
        GfVec4d(0.0, 0.0, GetWidth(), GetHeight()));
    _engine->SetRendererAov(HdAovTokens->color);

    do {
        _engine->Render(_stage->GetPseudoRoot(), _GetRenderParams());
    } while (!_engine->IsConverged());

    if (!GetOutputFilePath().empty()) {
        const std::string output = TfStringReplace(
            GetOutputFilePath(),
            ".png",
            TfStringPrintf("_%03d.png", imageIndex));
        TF_VERIFY(WriteToFile(_engine.get(), HdAovTokens->color, output));
    }
}

bool
_TestDrawing::_ReadCenterElementId(int* value) const
{
    if (!value) {
        return false;
    }
    HdRenderBuffer* const buffer =
        _engine->GetAovRenderBuffer(HdAovTokens->elementId);
    if (!buffer || buffer->GetFormat() != HdFormatInt32) {
        return false;
    }
    buffer->Resolve();
    int const* const data = static_cast<int const*>(buffer->Map());
    if (!data) {
        return false;
    }
    const int x = buffer->GetWidth() / 2;
    const int y = buffer->GetHeight() / 2;
    *value = data[y * buffer->GetWidth() + x];
    buffer->Unmap();
    return true;
}

bool
_TestDrawing::_CaptureColor(std::vector<std::uint8_t>* bytes) const
{
    if (!bytes) {
        return false;
    }
    HgiTextureHandle const texture =
        _engine->GetAovTexture(HdAovTokens->color);
    if (!texture) {
        return false;
    }

    size_t allocationSize = 0;
    HdStTextureUtils::AlignedBuffer<std::uint8_t> const readback =
        HdStTextureUtils::HgiTextureReadback<std::uint8_t>(
            _engine->GetHgi(), texture, &allocationSize);
    HgiTextureDesc const& descriptor = texture->GetDescriptor();
    const size_t byteCount =
        static_cast<size_t>(descriptor.dimensions[0]) *
        static_cast<size_t>(descriptor.dimensions[1]) *
        HgiGetDataSizeOfFormat(descriptor.format);
    if (!readback.get() || byteCount == 0 || allocationSize < byteCount) {
        return false;
    }
    bytes->assign(readback.get(), readback.get() + byteCount);
    return true;
}

void
_TestDrawing::DrawTest(bool)
{
    if (!_stage || !_engine) {
        return;
    }

    _Render(0);
    int elementId = -1;
    std::vector<std::uint8_t> unselectedColor;
    TF_VERIFY(_ReadCenterElementId(&elementId));
    TF_VERIFY(elementId == 1, "Expected authored curve 1, got %d", elementId);
    TF_VERIFY(_CaptureColor(&unselectedColor));

    UsdImagingGLEngine::IntersectionResultVector hits;
    const UsdImagingGLEngine::PickParams pickParams = {
        HdxPickTokens->resolveNearestToCenter};
    const bool didHit = _engine->TestIntersection(
        pickParams,
        _viewMatrix,
        _frustum.ComputeProjectionMatrix(),
        _stage->GetPseudoRoot(),
        _GetRenderParams(),
        &hits);
    TF_VERIFY(didHit);
    TF_VERIFY(hits.size() == 1);
    if (hits.size() == 1) {
        TF_VERIFY(hits[0].hitPrimPath == SdfPath("/Curves"));
    }

    _engine->SetSelected({SdfPath("/Curves")});
    _Render(1);
    std::vector<std::uint8_t> selectedColor;
    TF_VERIFY(_CaptureColor(&selectedColor));
    TF_VERIFY(
        unselectedColor.size() == selectedColor.size() &&
        unselectedColor != selectedColor,
        "BasisCurves highlight did not alter the presented color AOV");
    TF_VERIFY(_ReadCenterElementId(&elementId));
    TF_VERIFY(elementId == 1);
}

void
_TestDrawing::ShutdownTest()
{
    _engine.reset();
    _stage.Reset();
}

} // anonymous namespace

int
main(int argc, char* argv[])
{
    TfErrorMark mark;
    _TestDrawing drawing;
    drawing.RunTest(argc, argv);
    if (mark.IsClean()) {
        std::cout << "OK\n";
        return EXIT_SUCCESS;
    }
    std::cout << "FAILED\n";
    return EXIT_FAILURE;
}
