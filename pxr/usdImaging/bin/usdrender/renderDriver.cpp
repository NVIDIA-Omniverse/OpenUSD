#include "renderDriver.h"
#include "offscreenContext.h"
#include "outputPath.h"
#include "pxr/base/arch/fileSystem.h"
#include "pxr/imaging/cameraUtil/framing.h"
#include "pxr/imaging/glf/simpleLight.h"
#include "pxr/imaging/glf/simpleMaterial.h"
#include "pxr/imaging/hd/aov.h"
#include "pxr/imaging/hd/renderSettings.h"
#include "pxr/usd/usd/editContext.h"
#include "pxr/usd/usdGeom/camera.h"
#include "pxr/usd/usdGeom/tokens.h"
#include "pxr/usd/usdRender/product.h"
#include "pxr/usdImaging/usdImagingGL/engine.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <sstream>
#include <thread>

PXR_NAMESPACE_USING_DIRECTIVE

// Accept commas and whitespace in the purpose list to match the mask parser.
static std::vector<std::string> _Split(std::string s)
{
    std::replace(s.begin(), s.end(), ',', ' ');
    std::istringstream i(s);
    std::vector<std::string> r;
    for (std::string x; i >> x;)
        r.push_back(x);
    return r;
}

// Check products without resolver semantics: productName has already been
// converted to the exact filesystem path that the renderer must create.
static bool _Exists(const std::string &p)
{
    FILE *f = ArchOpenFile(p.c_str(), "rb");
    if (!f)
        return false;
    fclose(f);
    return true;
}

bool RenderAll(const Options &o, const StageData &d, const RenderRequest &r)
{
    // GPU Hydra delegates need a current graphics context even though this
    // command never presents an image. CPU-only operation skips context setup.
    OffscreenContext context(o.gpu);
    if (!context.IsValid()) {
        std::cerr << context.GetError() << "\n";
        return false;
    }

    // Configure one Hydra engine for every requested frame. Offline mode tells
    // delegates to write authored RenderProducts after convergence rather than
    // behaving like an interactive viewport.
    UsdImagingGLEngine::Parameters p;
    p.rendererPluginId = r.renderer;
    p.gpuEnabled = o.gpu;
    p.enableUsdDrawModes = o.drawMode;
    UsdImagingGLEngine engine(p);
    engine.SetEnablePresentation(false);
    engine.SetRendererSetting(HdRenderSettingsTokens->enableInteractive,
                              VtValue(false));
    engine.SetActiveRenderSettingsPrimPath(r.settings);
    if (!r.pass.IsEmpty())
        engine.SetActiveRenderPassPrimPath(r.pass);
    for (const auto &v : r.customSettings)
        engine.SetRendererSetting(TfToken(v.first), v.second);
    engine.SetRendererAov(HdAovTokens->color);

    // Resolve frame syntax before rendering so an invalid range cannot leave a
    // partially rendered sequence on disk.
    std::vector<UsdTimeCode> frames;
    if (!ParseFrames(o, d.stage->GetStartTimeCode(), &frames))
        return false;
    const UsdGeomCamera camera(d.stage->GetPrimAtPath(r.camera));
    std::cout << "Camera: " << r.camera
              << "\nRenderer plugin: " << engine.GetCurrentRendererId() << "\n";
    const auto purposes = _Split(o.purposes);
    for (const UsdTimeCode time : frames) {
        // Use the authored product resolution unless it is invalid or the user
        // requested a width override. Preserve the camera aspect ratio when a
        // fallback resolution is required.
        const GfCamera gc = camera.GetCamera(time);
        GfVec2i size = r.products[0].resolution;
        if (o.imageWidth > 0 || size[0] <= 0 || size[1] <= 0) {
            const int w = o.imageWidth > 0 ? o.imageWidth : 960;
            size = GfVec2i(
                w,
                std::max(1, (int)(w / std::max(.0001f, gc.GetAspectRatio()))));
        }
        engine.SetCameraPath(r.camera);

        // Convert UsdRender's normalized data window into Hydra's inclusive,
        // top-left-origin pixel rectangle for this frame's resolved size.
        const GfRange2f ndc = r.products[0].dataWindow;
        const GfVec2i lo((int)std::floor(ndc.GetMin()[0] * size[0]),
                         (int)std::floor((1.0f - ndc.GetMax()[1]) * size[1]));
        const GfVec2i hi((int)std::ceil(ndc.GetMax()[0] * size[0]) - 1,
                         (int)std::ceil((1.0f - ndc.GetMin()[1]) * size[1]) -
                             1);
        const GfRange2f display(GfVec2f(0), GfVec2f(size));
        engine.SetFraming(CameraUtilFraming(display, GfRect2i(lo, hi),
                                            r.products[0].pixelAspectRatio));
        engine.SetRenderBufferSize(size);

        // The optional camera light follows the evaluated camera transform at
        // each time code; the low ambient term matches usdrecord behavior.
        const GfFrustum frustum = gc.GetFrustum();
        GlfSimpleLightVector lights;
        if (o.cameraLight) {
            const GfVec3d pos = frustum.GetPosition();
            GlfSimpleLight l(GfVec4f(pos[0], pos[1], pos[2], 1));
            l.SetTransform(frustum.ComputeViewInverse());
            lights.push_back(l);
        }
        GlfSimpleMaterial material;
        engine.SetLightingState(lights, material, GfVec4f(.01f, .01f, .01f, 1));

        // Redirect productName into the requested output root in the anonymous
        // session layer. Removing stale files makes post-render existence a
        // meaningful success check rather than accepting a previous result.
        std::vector<std::string> expected;
        {
            UsdEditContext edit(d.stage, d.session);
            for (const RenderProduct &product : r.products) {
                std::string path, error;
                if (!ResolveOutputPath(product.name, o.outputRoot, time, &path,
                                       &error)) {
                    std::cerr << error << "\n";
                    return false;
                }
                if (!UsdRenderProduct(d.stage->GetPrimAtPath(product.path))
                         .GetProductNameAttr()
                         .Set(TfToken(path))) {
                    std::cerr << "Could not override productName on <"
                              << product.path << ">\n";
                    return false;
                }
                if (_Exists(path) && ArchUnlinkFile(path.c_str()) != 0) {
                    std::cerr << "Could not remove stale RenderProduct '"
                              << path << "'\n";
                    return false;
                }
                expected.push_back(path);
            }
        }

        // Build the per-frame Hydra visibility and display parameters after
        // product paths are ready, so rendering cannot start with stale names.
        UsdImagingGLRenderParams rp;
        rp.frame = time;
        rp.complexity = o.complexity;
        rp.colorCorrectionMode = TfToken(o.colorCorrection);
        rp.clearColor = GfVec4f(0);
        rp.enableSceneMaterials = o.sceneMaterials;
        rp.showProxy = std::find(purposes.begin(), purposes.end(), "proxy") !=
                       purposes.end();
        rp.showRender = std::find(purposes.begin(), purposes.end(), "render") !=
                        purposes.end();
        rp.showGuides = std::find(purposes.begin(), purposes.end(), "guide") !=
                        purposes.end();
        std::cout << "Recording time code: "
                  << (time.IsDefault() ? 0 : time.GetValue()) << "\n";

        // Hydra rendering is progressive. Poll until the delegate reports
        // convergence and gently back off to avoid busy-waiting CPU renderers.
        unsigned delay = 10;
        do {
            engine.Render(d.stage->GetPseudoRoot(), rp);
            if (!engine.IsConverged()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                delay = std::min(100u, delay + 5);
            }
        } while (!engine.IsConverged());

        // Convergence alone does not prove an offline delegate wrote its
        // products. Require every resolved file before advancing to the next
        // frame so failures identify the first incomplete time code.
        for (const std::string &path : expected)
            if (!_Exists(path)) {
                std::cerr << "Missing expected RenderProduct '" << path
                          << "'\n";
                return false;
            }
    }
    return true;
}
