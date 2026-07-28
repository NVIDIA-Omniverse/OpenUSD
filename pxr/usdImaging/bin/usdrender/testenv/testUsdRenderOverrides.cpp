#include "overrides.h"

#include "pxr/base/gf/vec2i.h"
#include "pxr/usd/sdf/attributeSpec.h"
#include "pxr/usd/sdf/assetPath.h"
#include "pxr/usd/sdf/layer.h"
#include "pxr/usd/sdf/primSpec.h"
#include "pxr/usd/sdf/types.h"
#include "pxr/usd/usd/attribute.h"
#include "pxr/usd/usd/prim.h"
#include "pxr/usd/usd/stage.h"

#include <iostream>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

const char* const _stageText = R"usda(#usda 1.0
(
    renderSettingsPrimPath = "/Render/Settings"
)
def Camera "Camera"
{
    float focalLength = 50
}
def Scope "Render"
{
    def RenderSettings "Settings"
    {
        custom uniform int existing = 2

        def Scope "Child"
        {
        }
    }
    def RenderProduct "Product"
    {
        int2 resolution = (64, 32)
    }
}
over "Undefined"
{
}
def Xform "Prototype"
{
    def Xform "Child"
    {
        float value = 1
    }
}
def Xform "Instance" (
    instanceable = true
    references = </Prototype>
)
{
}
)usda";

struct _StageFixture
{
    SdfLayerRefPtr root;
    SdfLayerRefPtr session;
    UsdStageRefPtr stage;
};

_StageFixture
_OpenFixture(const SdfLayerRefPtr& suppliedSession = SdfLayerRefPtr())
{
    _StageFixture fixture;
    fixture.root = SdfLayer::CreateAnonymous("root.usda");
    fixture.root->ImportFromString(_stageText);
    fixture.session = suppliedSession ? suppliedSession
                                      : SdfLayer::CreateAnonymous("session.usda");
    fixture.stage = UsdStage::Open(fixture.root, fixture.session);
    return fixture;
}

bool
_Expect(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << message << "\n";
    }
    return condition;
}

bool
_Apply(_StageFixture* fixture, const std::vector<std::string>& specs,
       std::string* error)
{
    return ApplyAttributeOverrides(specs, fixture->stage, fixture->session,
                                   SdfPath("/Render/Settings"), error);
}

bool
_TestSuccessfulOverrides()
{
    _StageFixture fixture = _OpenFixture();
    std::string error;
    const bool applied = _Apply(
        &fixture,
        {"/Render/Product.resolution = (32, 16)",
         "float /Camera.focalLength = 35",
         "bool {SETTINGS}.domeLightCameraVisibility = false",
         "{settings}.ty:maxBounces = 11",
         "int {settings}/Child.value = 9",
         "{settings}.existing = 3",
         "int /Render/Settings.customValue = 1",
         "int /Render/Settings.customValue = 2",
         "uniform int /Render/Settings.uniformCustom = 7",
         "asset /Render/Settings.assetValue = @textures/test.exr@"},
        &error);
    if (!_Expect(applied, error)) {
        return false;
    }

    GfVec2i resolution;
    float focalLength = 0.0f;
    bool domeVisibility = true;
    int customValue = 0;
    int uniformCustom = 0;
    int existing = 0;
    int maxBounces = 0;
    int childValue = 0;
    SdfAssetPath assetValue;
    const UsdPrim settings = fixture.stage->GetPrimAtPath(
        SdfPath("/Render/Settings"));
    const UsdAttribute custom = settings.GetAttribute(TfToken("customValue"));
    const UsdAttribute uniform =
        settings.GetAttribute(TfToken("uniformCustom"));
    const UsdAttribute existingAttribute =
        settings.GetAttribute(TfToken("existing"));
    const SdfPrimSpecHandle settingsSpec =
        fixture.session->GetPrimAtPath(SdfPath("/Render/Settings"));
    return _Expect(
        fixture.stage->GetAttributeAtPath(
            SdfPath("/Render/Product.resolution"))
                .Get(&resolution) &&
            resolution == GfVec2i(32, 16),
        "resolution override did not compose") &&
        _Expect(fixture.stage->GetAttributeAtPath(
                    SdfPath("/Camera.focalLength"))
                    .Get(&focalLength) &&
                    focalLength == 35.0f,
                "non-settings override did not compose") &&
        _Expect(settings.GetAttribute(
                    TfToken("domeLightCameraVisibility"))
                    .Get(&domeVisibility) &&
                    !domeVisibility,
                "{settings} custom override did not compose") &&
        _Expect(settings.GetAttribute(TfToken("ty:maxBounces"))
                        .Get(&maxBounces) &&
                    maxBounces == 11,
                "unauthored applied-schema attribute was not inferred") &&
        _Expect(fixture.stage->GetAttributeAtPath(
                    SdfPath("/Render/Settings/Child.value"))
                        .Get(&childValue) &&
                    childValue == 9,
                "{settings}/Child prefix override did not compose") &&
        _Expect(custom && custom.IsCustom() && custom.Get(&customValue) &&
                    customValue == 2 &&
                    custom.GetVariability() == SdfVariabilityVarying,
                "compatible duplicate did not author one last-value-wins "
                "custom attribute") &&
        _Expect(existingAttribute && existingAttribute.IsCustom() &&
                    existingAttribute.Get(&existing) && existing == 3 &&
                    existingAttribute.GetVariability() == SdfVariabilityUniform,
                "existing custom attribute lost its custom or variability "
                "state") &&
        _Expect(uniform && uniform.IsCustom() && uniform.Get(&uniformCustom) &&
                    uniformCustom == 7 &&
                    uniform.GetVariability() == SdfVariabilityUniform,
                "explicit custom variability was not preserved") &&
        _Expect(settings.GetAttribute(TfToken("assetValue")).Get(&assetValue) &&
                    assetValue.GetAssetPath() == "textures/test.exr",
                "asset value was not parsed as an SdfAssetPath") &&
        _Expect(settingsSpec && settingsSpec->GetAttributes().size() == 6,
                "duplicate destination created an unexpected attribute spec");
}

bool
_TestSessionLayerComposition()
{
    const SdfLayerRefPtr userSession =
        SdfLayer::CreateAnonymous("user-session.usda");
    userSession->ImportFromString(R"usda(#usda 1.0
over "Camera"
{
    float focalLength = 25
}
)usda");
    std::string before;
    userSession->ExportToString(&before);
    const SdfLayerRefPtr wrapper = SdfLayer::CreateAnonymous("wrapper.usda");
    wrapper->SetSubLayerPaths({userSession->GetIdentifier()});
    _StageFixture fixture = _OpenFixture(wrapper);
    std::string error;
    if (!_Expect(_Apply(&fixture, {"/Camera.focalLength = 75"}, &error),
                 error)) {
        return false;
    }
    float focalLength = 0.0f;
    std::string after;
    userSession->ExportToString(&after);
    return _Expect(after == before,
                   "user session layer was modified") &&
        _Expect(fixture.stage->GetAttributeAtPath(
                    SdfPath("/Camera.focalLength"))
                    .Get(&focalLength) &&
                    focalLength == 75.0f,
                "command-line override did not win over user session layer") &&
        _Expect(wrapper->GetSubLayerPaths().size() == 1,
                "user session layer was dropped from the wrapper");
}

bool
_ExpectFailure(const std::vector<std::string>& specs,
               const std::string& expectedMessage,
               const std::string& expectedSource = std::string(),
               const SdfPath& settingsPath = SdfPath("/Render/Settings"),
               const SdfPath& forbiddenPath = SdfPath())
{
    _StageFixture fixture = _OpenFixture();
    std::string error;
    const bool applied = ApplyAttributeOverrides(
        specs, fixture.stage, fixture.session, settingsPath, &error);
    return _Expect(!applied, "invalid override unexpectedly succeeded") &&
        _Expect(error.find(expectedSource.empty() ? specs.back()
                                                  : expectedSource) !=
                    std::string::npos,
                "error did not identify an offending --set: " + error) &&
        _Expect(error.find(expectedMessage) != std::string::npos,
                "unexpected error: " + error) &&
        _Expect(fixture.session->GetRootPrims().empty(),
                "failed override set modified the session layer") &&
        _Expect(forbiddenPath.IsEmpty() ||
                    !fixture.stage->GetPrimAtPath(forbiddenPath),
                "failed value injected a prim into the stage");
}

bool
_TestFailures()
{
    const std::string injection =
        "/Render/Product.resolution = (32, 16)\n}\n"
        "over \"Injected\"\n{\n    custom string x = \"y\"\n}\n"
        "over \"Z\"\n{";
    return _ExpectFailure({"/Camera.focalLength 35"}, "expected '='") &&
        _ExpectFailure({" = 35"}, "must not be empty") &&
        _ExpectFailure({"/Camera.focalLength = "}, "must not be empty") &&
        _ExpectFailure({"focalLength = 35"}, "target must be") &&
        _ExpectFailure({"/Missing.value = 1"}, "does not exist") &&
        _ExpectFailure({"int /Undefined.value = 1"}, "not defined") &&
        _ExpectFailure({"/Camera.focalLenght = 35"}, "specify its type") &&
        _ExpectFailure({"int /Camera.focalLength = 35"},
                       "disagrees with composed type") &&
        _ExpectFailure({"uniform float /Camera.focalLength = 35"},
                       "variability") &&
        _ExpectFailure({"flt /Camera.newValue = 1"}, "unknown value type") &&
        _ExpectFailure({"/Render/Product.resolution = [1, 2"},
                       "parse error") &&
        _ExpectFailure({"int /Bad[.value = 1"}, "invalid prim path") &&
        _ExpectFailure({"int /Render/Settings.metadata = 1 "
                        "(displayGroup = \"Injected\")"},
                       "additional layer content") &&
        _ExpectFailure({injection}, "additional layer content", std::string(),
                       SdfPath("/Render/Settings"), SdfPath("/Injected")) &&
        _ExpectFailure({"int /Render/Settings.a = 1",
                        "float /Render/Settings.a = 1"},
                       "Conflicting", "int /Render/Settings.a = 1") &&
        _ExpectFailure({"uniform int /Render/Settings.a = 1",
                        "varying int /Render/Settings.a = 1"},
                       "Conflicting", "uniform int /Render/Settings.a = 1") &&
        _ExpectFailure({"int /Render/Settings.valid = 1",
                        "/Camera.focalLenght = 35"},
                       "specify its type", "/Camera.focalLenght = 35") &&
        _ExpectFailure({"{settings}.existing = 3"}, "no RenderSettings",
                       std::string(), SdfPath()) &&
        _ExpectFailure({"int x{settings}.value = 1"}, "whole path prefix") &&
        _ExpectFailure({"/Instance/Child.value = 2"}, "native instance");
}

} // namespace

int
main()
{
    return _TestSuccessfulOverrides() && _TestSessionLayerComposition() &&
                   _TestFailures()
               ? 0
               : 1;
}
