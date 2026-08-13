#include "renderRequest.h"
#include "overrides.h"
#include "pxr/usd/ar/resolver.h"
#include "pxr/usd/sdf/path.h"
#include "pxr/usd/usdGeom/camera.h"
#include "pxr/usd/usdRender/pass.h"
#include "pxr/usd/usdRender/settings.h"
#include "pxr/usd/usdRender/spec.h"
#include "pxr/usd/usdUtils/pipeline.h"
#include "pxr/usdImaging/usdImagingGL/engine.h"
#include <algorithm>
#include <iostream>
#include <sstream>

PXR_NAMESPACE_USING_DIRECTIVE

static SdfPath _Settings(const Options &o, const UsdStagePtr &s,
                         SdfPath *passPath);

// Accept commas and whitespace so population masks match the purpose-list
// syntax accepted elsewhere by usdrender.
static std::vector<std::string> _Split(std::string s)
{
    std::replace(s.begin(), s.end(), ',', ' ');
    std::istringstream i(s);
    std::vector<std::string> r;
    for (std::string x; i >> x;)
        r.push_back(x);
    return r;
}

bool LoadStage(const Options &o, StageData *d)
{
    // Bind asset resolution to either the input asset or the caller's current
    // resolver context before opening any layers that may contain dependencies.
    const ArResolverContext c =
        o.resolverContext == "inherit"
            ? ArGetResolver().CreateDefaultContext()
            : ArGetResolver().CreateDefaultContextForAsset(o.usdFile);
    const SdfLayerRefPtr root = SdfLayer::FindOrOpen(o.usdFile);
    if (!root) {
        std::cerr << "Could not open layer: " << o.usdFile << "\n";
        return false;
    }

    // Preserve a supplied session layer below command-line opinions. The
    // anonymous wrapper keeps --set stronger without modifying the user's file.
    const SdfLayerRefPtr userSession =
        o.sessionLayer.empty() ? SdfLayerRefPtr()
                               : SdfLayer::FindOrOpen(o.sessionLayer);
    if (!o.sessionLayer.empty() && !userSession) {
        std::cerr << "Could not open layer: " << o.sessionLayer << "\n";
        return false;
    }
    if (!o.setSpecs.empty() && userSession) {
        d->session = SdfLayer::CreateAnonymous();
        d->session->SetSubLayerPaths({userSession->GetIdentifier()});
    } else {
        d->session = userSession ? userSession : SdfLayer::CreateAnonymous();
    }

    // Apply the population mask while opening the stage. Override validation
    // consequently rejects targets hidden by the mask instead of authoring
    // opinions that cannot affect this invocation.
    if (o.mask.empty())
        d->stage = UsdStage::Open(root, d->session, c);
    else {
        UsdStagePopulationMask m;
        for (const auto &p : _Split(o.mask))
            m.Add(SdfPath(p));
        d->stage = UsdStage::OpenMasked(root, d->session, c, m);
    }
    if (!d->stage) {
        std::cerr << "Could not open USD stage: " << o.usdFile << "\n";
        return false;
    }

    // Resolve the active pass and settings paths before applying overrides so
    // every {settings} token has one stable meaning for the whole command.
    d->settings = _Settings(o, d->stage, &d->pass);

    // Validate and author the complete override set as one operation. Printing
    // exports the effective wrapper, including a supplied session sublayer.
    if (!o.setSpecs.empty()) {
        std::string error;
        if (!ApplyAttributeOverrides(o.setSpecs, d->stage, d->session,
                                     d->settings, &error)) {
            std::cerr << error << "\n";
            return false;
        }
        if (o.printOverrides) {
            std::string layerText;
            if (!d->session->ExportToString(&layerText)) {
                std::cerr << "Could not print command-line override layer\n";
                return false;
            }
            std::cout << layerText;
        }
    }
    return true;
}

// Resolve the selected RenderSettings path using the same precedence as the
// render request: an explicit settings path, an explicit pass relationship,
// then stage metadata. An invalid explicit pass is diagnosed here so override
// expansion and request construction cannot select different settings prims.
static SdfPath _Settings(const Options &o, const UsdStagePtr &s,
                         SdfPath *passPath)
{
    if (!o.renderSettings.empty())
        return SdfPath(o.renderSettings);
    if (!o.renderPass.empty()) {
        *passPath = SdfPath(o.renderPass);
        UsdRenderPass p(s->GetPrimAtPath(*passPath));
        if (!p) {
            std::cerr << "Unknown render pass <" << o.renderPass << ">\n";
            return {};
        }
        SdfPathVector t;
        p.GetRenderSourceRel().GetTargets(&t);
        if (t.empty()) {
            std::cerr << "Render source not authored on " << o.renderPass
                      << "\n";
            return {};
        }
        if (t.size() > 1)
            TF_WARN("Render pass has multiple targets; using <%s>",
                    t[0].GetText());
        return t[0];
    }
    std::string p;
    s->GetMetadata(UsdRenderTokens->renderSettingsPrimPath, &p);
    return SdfPath(p);
}

// Resolve the command-line renderer, then the RenderPass renderer, and finally
// the offline tool's Embree default by plugin id or display name. The sentinel
// token distinguishes an unavailable selection from a valid plugin.
static TfToken _Renderer(const Options &o, const UsdStagePtr &s,
                         const SdfPath &pass)
{
    std::string q = o.renderer;
    if (q.empty() && !pass.IsEmpty()) {
        UsdPrim prim = s->GetPrimAtPath(pass);
        TfToken t;
        prim.GetAttribute(TfToken("hydra:rendererName")).Get(&t);
        q = t.GetString();
    }
    if (q.empty())
        q = "Embree";
    for (const TfToken &id : UsdImagingGLEngine::GetRendererPlugins())
        if (id == TfToken(q) ||
            UsdImagingGLEngine::GetRendererDisplayName(id) == q)
            return id;
    std::cerr << "Unknown renderer plugin: " << q << "\n";
    return TfToken("__invalid__");
}

bool BuildRenderRequest(const Options &o, const StageData &d, RenderRequest *r)
{
    // Require one valid RenderSettings prim before computing products. The
    // paths were resolved during stage loading and are not reinterpreted here.
    r->settings = d.settings;
    r->pass = d.pass;
    if (r->settings.IsEmpty()) {
        std::cerr << "No RenderSettings prim specified or authored in stage "
                     "metadata\n";
        return false;
    }
    UsdRenderSettings settings(d.stage->GetPrimAtPath(r->settings));
    if (!settings) {
        std::cerr << "Unknown RenderSettings prim <" << r->settings << ">\n";
        return false;
    }
    r->renderer = _Renderer(o, d.stage, r->pass);
    if (r->renderer == TfToken("__invalid__"))
        return false;

    // Flatten the UsdRender network and retain only products that the Hydra
    // render-product path can write. Empty product names cannot be verified.
    const UsdRenderSpec spec = UsdRenderComputeSpec(settings, {});
    for (const auto &p : spec.products)
        if (!p.name.IsEmpty() && (p.type == UsdRenderTokens->raster ||
                                  p.type == UsdRenderTokens->raw))
            r->products.push_back({p.renderProductPath, p.cameraPath,
                                   p.resolution, p.dataWindowNDC,
                                   p.pixelAspectRatio, p.name.GetString()});
    if (r->products.empty()) {
        std::cerr << "RenderSettings <" << r->settings
                  << "> has no supported RenderProduct with a non-empty "
                     "productName\n";
        return false;
    }

    // Hydra renders one camera and framing per engine invocation. Reject a
    // product set that would require different scene renders instead of
    // silently producing some products with the wrong view.
    const auto &first = r->products.front();
    for (const auto &p : r->products)
        if (p.camera != first.camera || p.resolution != first.resolution ||
            p.dataWindow != first.dataWindow) {
            std::cerr
                << "RenderProducts have incompatible camera, resolution, or "
                   "data window\n";
            return false;
        }

    // The command-line camera overrides authored product cameras. A bare name
    // is interpreted at the pseudo-root for compatibility with usdrecord; the
    // pipeline primary camera is the final fallback.
    if (!o.camera.empty()) {
        r->camera = SdfPath(o.camera);
        if (!r->camera.IsAbsolutePath())
            r->camera =
                SdfPath::AbsoluteRootPath().AppendChild(TfToken(o.camera));
    } else
        r->camera = first.camera;
    if (r->camera.IsEmpty())
        r->camera = SdfPath::AbsoluteRootPath().AppendChild(
            UsdUtilsGetPrimaryCameraName());
    if (!UsdGeomCamera(d.stage->GetPrimAtPath(r->camera))) {
        std::cerr << "Could not find camera <" << r->camera << ">\n";
        return false;
    }

    // Generic unnamespaced custom attributes are not represented in
    // UsdRenderSpec. Forward them directly as Hydra renderer settings while
    // namespaced settings continue through the active RenderSettings prim.
    for (const UsdAttribute &a : settings.GetPrim().GetAuthoredAttributes())
        if (a.IsCustom() && a.GetNamespace().IsEmpty()) {
            VtValue v;
            if (a.Get(&v))
                r->customSettings[a.GetName()] = v;
        }
    return true;
}
