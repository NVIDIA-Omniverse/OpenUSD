//
// Copyright 2025 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/imaging/plugin/hdEmbree/light.h"
#include "pxr/imaging/plugin/hdEmbree/lightSamplers.h"

#include "pxr/base/gf/colorSpace.h"
#include "pxr/base/gf/matrix3f.h"
#include "pxr/base/gf/matrix4f.h"
#include "pxr/base/gf/vec2f.h"

#include <cmath>
#include <cstdio>
#include <exception>
#include <functional>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

int _totalTests = 0;
int _failedTests = 0;

struct _TestEntry {
    std::string name;
    std::function<bool()> fn;
};

std::vector<_TestEntry>&
_Tests()
{
    static std::vector<_TestEntry> tests;
    return tests;
}

void
_Register(const char* name, std::function<bool()> fn)
{
    _Tests().push_back({name, std::move(fn)});
}

bool
_IsClose(float a, float b, float eps = 1e-5f)
{
    return std::fabs(a - b) <= eps;
}

bool
_IsClose(const GfVec3f& a, const GfVec3f& b, float eps = 1e-5f)
{
    return _IsClose(a[0], b[0], eps) &&
           _IsClose(a[1], b[1], eps) &&
           _IsClose(a[2], b[2], eps);
}

GfVec3f
_LatLongUvToDirection(float s, float t)
{
    const float theta = static_cast<float>(M_PI) * t;
    const float sinTheta = std::sin(theta);
    const float phi = 2.0f * static_cast<float>(M_PI) * (0.5f - s);
    return GfVec3f(
        sinTheta * std::sin(phi),
        std::cos(theta),
        sinTheta * std::cos(phi));
}

HdEmbree_LightData
_MakeDomeLight(const std::vector<GfVec3f>& pixels, int width, int height)
{
    HdEmbree_LightData light;
    light.xformLightToWorld = GfMatrix4f(1.0f);
    light.xformWorldToLight = GfMatrix4f(1.0f);
    light.normalXformLightToWorld = GfMatrix3f(1.0f);
    light.color = GfVec3f(1.0f);
    light.texture.pixels = pixels;
    light.texture.width = width;
    light.texture.height = height;
    light.texture.colorSpaceName = GfColorSpaceNames->LinearRec709;
    light.lightVariant = HdEmbree_Dome{};
    HdEmbreeBuildDomeLightSamplingDistribution(&light.texture);
    return light;
}

HdEmbree_LightData
_MakeSphereLight(const GfVec3f& center, float radius)
{
    HdEmbree_LightData light;
    light.xformLightToWorld = GfMatrix4f(1.0f);
    light.xformLightToWorld.SetTranslate(center);
    light.xformWorldToLight = light.xformLightToWorld.GetInverse();
    light.normalXformLightToWorld = GfMatrix3f(1.0f);
    light.color = GfVec3f(1.0f);
    light.lightVariant = HdEmbree_Sphere{radius};
    return light;
}

HdEmbree_LightData
_MakeRectLight(const GfVec3f& center, float width, float height)
{
    HdEmbree_LightData light;
    light.xformLightToWorld = GfMatrix4f(1.0f);
    light.xformLightToWorld.SetTranslate(center);
    light.xformWorldToLight = light.xformLightToWorld.GetInverse();
    light.normalXformLightToWorld = GfMatrix3f(1.0f);
    light.color = GfVec3f(1.0f);
    light.lightVariant = HdEmbree_Rect{width, height};
    return light;
}

HdEmbree_LightData
_MakeDiskLight(const GfVec3f& center, float radius)
{
    HdEmbree_LightData light;
    light.xformLightToWorld = GfMatrix4f(1.0f);
    light.xformLightToWorld.SetTranslate(center);
    light.xformWorldToLight = light.xformLightToWorld.GetInverse();
    light.normalXformLightToWorld = GfMatrix3f(1.0f);
    light.color = GfVec3f(1.0f);
    light.lightVariant = HdEmbree_Disk{radius};
    return light;
}

HdEmbree_LightData
_MakeDistantLight(float angle)
{
    HdEmbree_LightData light;
    light.xformLightToWorld = GfMatrix4f(1.0f);
    light.xformWorldToLight = GfMatrix4f(1.0f);
    light.normalXformLightToWorld = GfMatrix3f(1.0f);
    light.color = GfVec3f(1.0f);
    light.intensity = 1.0f;
    light.diffuse = 1.0f;
    light.lightVariant = HdEmbree_Distant{angle};
    return light;
}

bool
TestDomeDistributionBuildsCdfs()
{
    const HdEmbree_LightData light = _MakeDomeLight(
        {
            GfVec3f(1.0f), GfVec3f(4.0f), GfVec3f(1.0f), GfVec3f(1.0f),
            GfVec3f(1.0f), GfVec3f(1.0f), GfVec3f(1.0f), GfVec3f(1.0f)
        },
        4,
        2);

    const HdEmbree_LightTexture& texture = light.texture;
    if (texture.weightSum <= 0.0f) {
        std::printf("    expected positive weight sum\n");
        return false;
    }
    if (texture.conditionalCdf.size() != 10 || texture.marginalCdf.size() != 3) {
        std::printf("    unexpected CDF sizes: conditional=%zu marginal=%zu\n",
                    texture.conditionalCdf.size(), texture.marginalCdf.size());
        return false;
    }
    if (!_IsClose(texture.marginalCdf.back(), 1.0f, 1e-6f)) {
        std::printf("    marginal CDF not normalized: %f\n",
                    texture.marginalCdf.back());
        return false;
    }
    for (int y = 0; y < texture.height; ++y) {
        const float end =
            texture.conditionalCdf[static_cast<size_t>(y) *
                                   static_cast<size_t>(texture.width + 1) +
                                   texture.width];
        if (!_IsClose(end, 1.0f, 1e-6f)) {
            std::printf("    row %d conditional CDF not normalized: %f\n", y, end);
            return false;
        }
    }
    return true;
}

bool
TestDomeDistributionRespectsTextureColorSpace()
{
    HdEmbree_LightTexture texture;
    texture.pixels = {GfVec3f(0.5f)};
    texture.width = 1;
    texture.height = 1;
    texture.colorSpaceName = GfColorSpaceNames->G22Rec709;

    HdEmbreeBuildDomeLightSamplingDistribution(&texture);

    const float expectedLuminance = std::pow(0.5f, 2.2f);
    const float measuredLuminance = texture.weightSum * 0.5f;
    if (!_IsClose(measuredLuminance, expectedLuminance, 1e-3f)) {
        std::printf(
            "    expected gamma-aware luminance %f, got %f\n",
            expectedLuminance,
            measuredLuminance);
        return false;
    }

    return true;
}

bool
TestDomeDirectionalPdfPrefersBrightTexel()
{
    const HdEmbree_LightData light = _MakeDomeLight(
        {
            GfVec3f(1.0f), GfVec3f(40.0f), GfVec3f(1.0f), GfVec3f(1.0f),
            GfVec3f(1.0f), GfVec3f(1.0f),  GfVec3f(1.0f), GfVec3f(1.0f)
        },
        4,
        2);

    const GfVec3f brightDir =
        _LatLongUvToDirection((1.0f + 0.5f) / 4.0f, (0.0f + 0.5f) / 2.0f);
    const GfVec3f darkDir =
        _LatLongUvToDirection((2.0f + 0.5f) / 4.0f, (0.0f + 0.5f) / 2.0f);

    const auto bright =
        HdEmbreeLightSampler::EvaluateDomeLightDirection(light, brightDir);
    const auto dark =
        HdEmbreeLightSampler::EvaluateDomeLightDirection(light, darkDir);

    const float brightPdf = (bright.invPdfW > 0.0f) ? (1.0f / bright.invPdfW) : 0.0f;
    const float darkPdf = (dark.invPdfW > 0.0f) ? (1.0f / dark.invPdfW) : 0.0f;
    if (!(brightPdf > darkPdf * 10.0f)) {
        std::printf("    expected bright texel PDF to dominate: bright=%f dark=%f\n",
                    brightPdf, darkPdf);
        return false;
    }

    return true;
}

bool
TestDomeSampleMatchesDirectionalEvaluation()
{
    const HdEmbree_LightData light = _MakeDomeLight(
        {
            GfVec3f(1.0f), GfVec3f(8.0f), GfVec3f(2.0f), GfVec3f(1.0f),
            GfVec3f(1.0f), GfVec3f(3.0f), GfVec3f(1.0f), GfVec3f(6.0f)
        },
        4,
        2);

    const std::vector<GfVec2f> samples = {
        GfVec2f(0.1f, 0.2f),
        GfVec2f(0.8f, 0.3f),
        GfVec2f(0.4f, 0.7f),
        GfVec2f(0.95f, 0.95f),
    };

    for (const GfVec2f& u : samples) {
        const auto sampled = HdEmbreeLightSampler::GetLightSample(
            light, GfVec3f(0.0f), GfVec3f::YAxis(), u[0], u[1]);
        const auto evaluated = HdEmbreeLightSampler::EvaluateDomeLightDirection(
            light, sampled.wI);

        if (!_IsClose(sampled.Li, evaluated.Li, 1e-5f)) {
            std::printf("    sampled/evaluated Li mismatch\n");
            return false;
        }
        if (!_IsClose(sampled.invPdfW, evaluated.invPdfW, 1e-4f)) {
            std::printf("    sampled/evaluated invPdf mismatch: %f vs %f\n",
                        sampled.invPdfW, evaluated.invPdfW);
            return false;
        }
    }

    return true;
}

bool
TestDomeReflectionHemisphereSampleUsesHemispherePdf()
{
    const HdEmbree_LightData light = _MakeDomeLight({}, 0, 0);
    const GfVec3f normal = GfVec3f::YAxis();
    const auto sampled = HdEmbreeLightSampler::GetLightSample(
        light,
        GfVec3f(0.0f),
        normal,
        0.25f,
        0.5f,
        HdEmbreeLightSampler::SamplingMode::ReflectionHemisphere);

    if (!sampled.valid) {
        std::printf("    expected valid hemisphere sample\n");
        return false;
    }
    if (!(GfDot(sampled.wI, normal) > 0.0f)) {
        std::printf("    sample was outside the reflection hemisphere\n");
        return false;
    }

    const float pdfW =
        (sampled.invPdfW > 0.0f) ? (1.0f / sampled.invPdfW) : 0.0f;
    const float expectedPdf = 1.0f / (2.0f * static_cast<float>(M_PI));
    if (!_IsClose(pdfW, expectedPdf, 1e-6f)) {
        std::printf("    expected hemisphere pdf %f, got %f\n",
                    expectedPdf, pdfW);
        return false;
    }

    return true;
}

bool
TestDomeReflectionHemisphereDirectionalPdf()
{
    const HdEmbree_LightData light = _MakeDomeLight({}, 0, 0);
    const GfVec3f normal = GfVec3f::YAxis();
    const auto above = HdEmbreeLightSampler::EvaluateDomeLightDirection(
        light,
        GfVec3f::YAxis(),
        normal,
        HdEmbreeLightSampler::SamplingMode::ReflectionHemisphere);
    const auto below = HdEmbreeLightSampler::EvaluateDomeLightDirection(
        light,
        -GfVec3f::YAxis(),
        normal,
        HdEmbreeLightSampler::SamplingMode::ReflectionHemisphere);

    if (!above.valid) {
        std::printf("    expected direction above the surface to be valid\n");
        return false;
    }
    const float abovePdf =
        (above.invPdfW > 0.0f) ? (1.0f / above.invPdfW) : 0.0f;
    const float expectedPdf = 1.0f / (2.0f * static_cast<float>(M_PI));
    if (!_IsClose(abovePdf, expectedPdf, 1e-6f)) {
        std::printf("    expected hemisphere directional pdf %f, got %f\n",
                    expectedPdf, abovePdf);
        return false;
    }
    if (below.valid || below.invPdfW != 0.0f) {
        std::printf("    expected direction below the surface to have zero pdf\n");
        return false;
    }

    return true;
}

bool
TestDomeReflectionHemisphereSampleMatchesDirectionalEvaluation()
{
    const HdEmbree_LightData light = _MakeDomeLight(
        {
            GfVec3f(1.0f), GfVec3f(8.0f), GfVec3f(2.0f), GfVec3f(1.0f),
            GfVec3f(1.0f), GfVec3f(3.0f), GfVec3f(1.0f), GfVec3f(6.0f)
        },
        4,
        2);
    const GfVec3f normal = GfVec3f::YAxis();
    const std::vector<GfVec2f> samples = {
        GfVec2f(0.1f, 0.2f),
        GfVec2f(0.8f, 0.3f),
        GfVec2f(0.4f, 0.7f),
        GfVec2f(0.95f, 0.95f),
    };

    for (const GfVec2f& u : samples) {
        const auto sampled = HdEmbreeLightSampler::GetLightSample(
            light,
            GfVec3f(0.0f),
            normal,
            u[0],
            u[1],
            HdEmbreeLightSampler::SamplingMode::ReflectionHemisphere);
        const auto evaluated = HdEmbreeLightSampler::EvaluateDomeLightDirection(
            light,
            sampled.wI,
            normal,
            HdEmbreeLightSampler::SamplingMode::ReflectionHemisphere);

        if (!sampled.valid || !(GfDot(sampled.wI, normal) > 0.0f)) {
            std::printf("    expected valid hemisphere sample\n");
            return false;
        }
        if (!_IsClose(sampled.Li, evaluated.Li, 1e-5f)) {
            std::printf("    sampled/evaluated hemisphere Li mismatch\n");
            return false;
        }
        if (!_IsClose(sampled.invPdfW, evaluated.invPdfW, 1e-4f)) {
            std::printf(
                "    sampled/evaluated hemisphere invPdf mismatch: %f vs %f\n",
                sampled.invPdfW,
                evaluated.invPdfW);
            return false;
        }
    }

    return true;
}

bool
TestSphereSampleMatchesDirectionalEvaluation()
{
    const HdEmbree_LightData light =
        _MakeSphereLight(GfVec3f(0.0f, 0.0f, 4.0f), 1.0f);
    const GfVec3f position(0.0f);
    const GfVec3f normal = GfVec3f::ZAxis();
    const std::vector<GfVec2f> samples = {
        GfVec2f(0.1f, 0.2f),
        GfVec2f(0.4f, 0.7f),
        GfVec2f(0.9f, 0.95f),
    };

    const float cosThetaMax = std::sqrt(1.0f - 1.0f / 16.0f);
    const float expectedInvPdfW =
        2.0f * static_cast<float>(M_PI) * (1.0f - cosThetaMax);

    for (const GfVec2f& u : samples) {
        const auto sampled = HdEmbreeLightSampler::GetLightSample(
            light, position, normal, u[0], u[1]);
        const auto evaluated = HdEmbreeLightSampler::EvaluateLightDirection(
            light, position, sampled.wI);

        if (!sampled.valid || !evaluated.valid) {
            std::printf("    expected valid sphere light samples\n");
            return false;
        }
        if (!_IsClose(sampled.Li, evaluated.Li, 1e-5f)) {
            std::printf("    sampled/evaluated sphere Li mismatch\n");
            return false;
        }
        if (!_IsClose(sampled.invPdfW, expectedInvPdfW, 1e-5f) ||
            !_IsClose(evaluated.invPdfW, expectedInvPdfW, 1e-5f)) {
            std::printf(
                "    sphere invPdf mismatch: sampled=%f evaluated=%f expected=%f\n",
                sampled.invPdfW,
                evaluated.invPdfW,
                expectedInvPdfW);
            return false;
        }
    }

    return true;
}

bool
TestDirectionalShapingDistributionPdfNormalizes()
{
    HdEmbree_Shaping shaping;
    shaping.coneAngle = 35.0f;
    shaping.coneSoftness = 0.25f;
    shaping.focus = 3.0f;
    HdEmbreeBuildDirectionalShapingDistribution(&shaping);

    if (!shaping.directionalDistribution.IsValid()) {
        std::printf("    expected valid directional shaping distribution\n");
        return false;
    }

    constexpr float cellSolidAngle =
        4.0f * static_cast<float>(M_PI) /
        static_cast<float>(
            HdEmbree_DirectionalShapingDistribution::NumCells);
    float integral = 0.0f;
    for (const float pdfW : shaping.directionalDistribution.cellPdfW) {
        integral += pdfW * cellSolidAngle;
    }
    if (!_IsClose(integral, 1.0f, 1e-4f)) {
        std::printf("    directional shaping PDF integral mismatch: %f\n",
                    integral);
        return false;
    }

    if (shaping.directionalDistribution.principalDirection[2] <= 0.8f) {
        std::printf("    expected principal direction near local +Z\n");
        return false;
    }

    return true;
}

bool
TestIesDirectionalDistributionBuildsAndSamples()
{
    static const char* const iesText =
        "IESNA:LM-63-1995\n"
        "TILT=NONE\n"
        "1 1000 1 3 1 1 1 1 1 1 1 1 1\n"
        "0 90 180\n"
        "0\n"
        "100 10 0\n";

    HdEmbree_Shaping shaping;
    if (!shaping.ies.iesFile.load(iesText)) {
        std::printf("    could not load synthetic IES profile\n");
        return false;
    }
    HdEmbreeBuildDirectionalShapingDistribution(&shaping);
    if (!shaping.directionalDistribution.IsValid()) {
        std::printf("    expected valid IES directional distribution\n");
        return false;
    }
    if (shaping.directionalDistribution.peakWeight <=
        shaping.directionalDistribution.averageWeight) {
        std::printf("    expected IES peak to exceed average weight\n");
        return false;
    }

    const HdEmbree_DirectionalShapingSample sample =
        HdEmbreeSampleDirectionalShaping(shaping, 0.25f, 0.75f);
    if (!sample.valid || sample.pdfW <= 0.0f) {
        std::printf("    expected valid IES directional sample\n");
        return false;
    }

    const float evaluatedPdf =
        HdEmbreeDirectionalShapingPdf(shaping, sample.localDirection);
    if (!_IsClose(sample.pdfW, evaluatedPdf, 1e-5f)) {
        std::printf("    sampled/evaluated IES pdf mismatch: %f vs %f\n",
                    sample.pdfW,
                    evaluatedPdf);
        return false;
    }

    return true;
}

bool
TestRectShapingAwareSampleMatchesDirectionalEvaluation()
{
    HdEmbree_LightData light =
        _MakeRectLight(GfVec3f(0.0f, 0.0f, 4.0f), 100.0f, 100.0f);
    light.shaping.coneAngle = 25.0f;
    light.shaping.coneSoftness = 0.1f;
    HdEmbreeBuildDirectionalShapingDistribution(&light.shaping);

    const GfVec3f position(0.0f);
    const GfVec3f normal = GfVec3f::ZAxis();
    const std::vector<GfVec2f> samples = {
        GfVec2f(0.25f, 0.35f), // finite area strategy
        GfVec2f(0.75f, 0.65f), // directional shaping strategy
    };

    for (const GfVec2f& u : samples) {
        const auto sampled = HdEmbreeLightSampler::GetLightSample(
            light, position, normal, u[0], u[1]);
        if (!sampled.valid) {
            std::printf("    expected valid rect light sample\n");
            return false;
        }

        const auto evaluated = HdEmbreeLightSampler::EvaluateLightDirection(
            light, position, sampled.wI);
        if (!evaluated.valid) {
            std::printf("    expected rect sample direction to evaluate\n");
            return false;
        }
        if (!_IsClose(sampled.Li, evaluated.Li, 1e-5f)) {
            std::printf("    sampled/evaluated rect Li mismatch\n");
            return false;
        }
        if (!_IsClose(sampled.invPdfW, evaluated.invPdfW, 1e-4f)) {
            std::printf(
                "    sampled/evaluated rect invPdf mismatch: %f vs %f\n",
                sampled.invPdfW,
                evaluated.invPdfW);
            return false;
        }
    }

    return true;
}

bool
TestRectFocusDirectionalSampleFoldsToEmissionHemisphere()
{
    HdEmbree_LightData light =
        _MakeRectLight(GfVec3f(0.0f, 0.0f, 4.0f), 100.0f, 100.0f);
    light.shaping.focus = 4.0f;
    HdEmbreeBuildDirectionalShapingDistribution(&light.shaping);

    float samplerU1 = 0.0f;
    bool foundBackHemisphereProposal = false;
    for (float shapingU1 : {0.55f, 0.65f, 0.75f, 0.85f, 0.95f}) {
        const HdEmbree_DirectionalShapingSample proposal =
            HdEmbreeSampleDirectionalShaping(light.shaping, shapingU1, 0.37f);
        if (proposal.valid && proposal.localDirection[2] < 0.0f) {
            samplerU1 = 0.5f + 0.5f * shapingU1;
            foundBackHemisphereProposal = true;
            break;
        }
    }

    if (!foundBackHemisphereProposal) {
        std::printf("    expected to find a focus proposal behind local +Z\n");
        return false;
    }

    const GfVec3f position(0.0f);
    const auto sampled = HdEmbreeLightSampler::GetLightSample(
        light, position, GfVec3f::ZAxis(), samplerU1, 0.37f);
    if (!sampled.valid || sampled.wI[2] <= 0.0f) {
        std::printf("    expected folded rect focus sample to hit +Z side\n");
        return false;
    }

    const auto evaluated = HdEmbreeLightSampler::EvaluateLightDirection(
        light, position, sampled.wI);
    if (!evaluated.valid) {
        std::printf("    expected folded rect focus direction to evaluate\n");
        return false;
    }
    if (!_IsClose(sampled.Li, evaluated.Li, 1e-5f)) {
        std::printf("    sampled/evaluated folded rect Li mismatch\n");
        return false;
    }
    if (!_IsClose(sampled.invPdfW, evaluated.invPdfW, 1e-4f)) {
        std::printf(
            "    sampled/evaluated folded rect invPdf mismatch: %f vs %f\n",
            sampled.invPdfW,
            evaluated.invPdfW);
        return false;
    }

    return true;
}

bool
TestDiskShapingAwareSampleMatchesDirectionalEvaluation()
{
    HdEmbree_LightData light =
        _MakeDiskLight(GfVec3f(0.0f, 0.0f, 4.0f), 50.0f);
    light.shaping.coneAngle = 25.0f;
    light.shaping.coneSoftness = 0.1f;
    HdEmbreeBuildDirectionalShapingDistribution(&light.shaping);

    const GfVec3f position(0.0f);
    const GfVec3f normal = GfVec3f::ZAxis();
    const std::vector<GfVec2f> samples = {
        GfVec2f(0.25f, 0.35f),
        GfVec2f(0.75f, 0.65f),
    };

    for (const GfVec2f& u : samples) {
        const auto sampled = HdEmbreeLightSampler::GetLightSample(
            light, position, normal, u[0], u[1]);
        if (!sampled.valid) {
            std::printf("    expected valid disk light sample\n");
            return false;
        }

        const auto evaluated = HdEmbreeLightSampler::EvaluateLightDirection(
            light, position, sampled.wI);
        if (!evaluated.valid) {
            std::printf("    expected disk sample direction to evaluate\n");
            return false;
        }
        if (!_IsClose(sampled.Li, evaluated.Li, 1e-5f)) {
            std::printf("    sampled/evaluated disk Li mismatch\n");
            return false;
        }
        if (!_IsClose(sampled.invPdfW, evaluated.invPdfW, 1e-4f)) {
            std::printf(
                "    sampled/evaluated disk invPdf mismatch: %f vs %f\n",
                sampled.invPdfW,
                evaluated.invPdfW);
            return false;
        }
    }

    return true;
}

bool
TestDistantDeltaSamplesLocalPositiveZ()
{
    const HdEmbree_LightData light = _MakeDistantLight(0.0f);

    const auto sampled = HdEmbreeLightSampler::GetLightSample(
        light,
        GfVec3f(0.0f),
        GfVec3f::ZAxis(),
        0.37f,
        0.91f);
    if (!sampled.valid || !sampled.delta) {
        std::printf("    expected valid delta distant light sample\n");
        return false;
    }
    if (!_IsClose(sampled.wI, GfVec3f::ZAxis(), 1e-6f)) {
        std::printf("    expected local +Z sample direction, got (%f, %f, %f)\n",
                    sampled.wI[0], sampled.wI[1], sampled.wI[2]);
        return false;
    }
    if (!_IsClose(sampled.invPdfW, 1.0f, 1e-6f)) {
        std::printf("    expected unit delta inverse pdf, got %f\n",
                    sampled.invPdfW);
        return false;
    }

    const auto evaluated = HdEmbreeLightSampler::EvaluateLightDirection(
        light, GfVec3f(0.0f), GfVec3f::ZAxis());
    const auto opposite = HdEmbreeLightSampler::EvaluateLightDirection(
        light, GfVec3f(0.0f), -GfVec3f::ZAxis());
    if (!evaluated.valid || !evaluated.delta || opposite.valid) {
        std::printf("    expected only the +Z delta direction to evaluate\n");
        return false;
    }

    return true;
}

bool
TestDistantConeSampleMatchesDirectionalEvaluation()
{
    const HdEmbree_LightData light = _MakeDistantLight(60.0f);
    const float thetaMax = 0.5f * 60.0f *
        static_cast<float>(M_PI) / 180.0f;
    const float expectedInvPdfW =
        2.0f * static_cast<float>(M_PI) * (1.0f - std::cos(thetaMax));

    const std::vector<GfVec2f> samples = {
        GfVec2f(0.0f, 0.0f),
        GfVec2f(0.25f, 0.5f),
        GfVec2f(0.8f, 0.9f),
    };

    for (const GfVec2f& u : samples) {
        const auto sampled = HdEmbreeLightSampler::GetLightSample(
            light, GfVec3f(0.0f), GfVec3f::ZAxis(), u[0], u[1]);
        const auto evaluated = HdEmbreeLightSampler::EvaluateLightDirection(
            light, GfVec3f(0.0f), sampled.wI);

        if (!sampled.valid || sampled.delta || !evaluated.valid ||
            evaluated.delta) {
            std::printf("    expected valid non-delta distant cone samples\n");
            return false;
        }
        if (!_IsClose(sampled.Li, evaluated.Li, 1e-5f)) {
            std::printf("    sampled/evaluated distant Li mismatch\n");
            return false;
        }
        if (!_IsClose(sampled.invPdfW, expectedInvPdfW, 1e-5f) ||
            !_IsClose(evaluated.invPdfW, expectedInvPdfW, 1e-5f)) {
            std::printf(
                "    distant invPdf mismatch: sampled=%f evaluated=%f expected=%f\n",
                sampled.invPdfW,
                evaluated.invPdfW,
                expectedInvPdfW);
            return false;
        }
    }

    return true;
}

bool
TestDistantNormalizeUsesUsdLuxSizeFactor()
{
    HdEmbree_LightData light = _MakeDistantLight(60.0f);
    light.normalize = true;
    light.intensity = 10.0f;

    const auto sampled = HdEmbreeLightSampler::GetLightSample(
        light, GfVec3f(0.0f), GfVec3f::ZAxis(), 0.5f, 0.25f);

    const float thetaMax = 0.5f * 60.0f *
        static_cast<float>(M_PI) / 180.0f;
    const float sinTheta = std::sin(thetaMax);
    const float sizeFactor =
        sinTheta * sinTheta * static_cast<float>(M_PI);
    const GfVec3f expected(10.0f / sizeFactor);
    if (!sampled.valid || !_IsClose(sampled.Li, expected, 1e-5f)) {
        std::printf(
            "    expected normalized distant Li %f, got (%f, %f, %f)\n",
            expected[0],
            sampled.Li[0],
            sampled.Li[1],
            sampled.Li[2]);
        return false;
    }

    return true;
}

bool
TestDomePdfApproximatelyNormalizes()
{
    const HdEmbree_LightData light = _MakeDomeLight(
        {
            GfVec3f(1.0f), GfVec3f(8.0f), GfVec3f(2.0f), GfVec3f(1.0f),
            GfVec3f(1.0f), GfVec3f(3.0f), GfVec3f(1.0f), GfVec3f(6.0f)
        },
        4,
        2);

    constexpr int nu = 256;
    constexpr int nv = 128;
    float integral = 0.0f;

    for (int y = 0; y < nv; ++y) {
        const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(nv);
        const float theta = static_cast<float>(M_PI) * v;
        const float jacobian =
            2.0f * static_cast<float>(M_PI) * static_cast<float>(M_PI) *
            std::sin(theta);
        for (int x = 0; x < nu; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(nu);
            const GfVec3f dir = _LatLongUvToDirection(u, v);
            const auto evaluated =
                HdEmbreeLightSampler::EvaluateDomeLightDirection(light, dir);
            const float pdfW =
                (evaluated.invPdfW > 0.0f) ? (1.0f / evaluated.invPdfW) : 0.0f;
            integral += pdfW * jacobian;
        }
    }

    integral /= static_cast<float>(nu * nv);
    if (!_IsClose(integral, 1.0f, 0.03f)) {
        std::printf("    PDF integral mismatch: %f\n", integral);
        return false;
    }

    return true;
}

} // namespace

int
main(int /*argc*/, char** /*argv*/)
{
    _Register("DomeDistributionBuildsCdfs", &TestDomeDistributionBuildsCdfs);
    _Register("DomeDistributionRespectsTextureColorSpace",
              &TestDomeDistributionRespectsTextureColorSpace);
    _Register("DomeDirectionalPdfPrefersBrightTexel",
              &TestDomeDirectionalPdfPrefersBrightTexel);
    _Register("DomeSampleMatchesDirectionalEvaluation",
              &TestDomeSampleMatchesDirectionalEvaluation);
    _Register("DomeReflectionHemisphereSampleUsesHemispherePdf",
              &TestDomeReflectionHemisphereSampleUsesHemispherePdf);
    _Register("DomeReflectionHemisphereDirectionalPdf",
              &TestDomeReflectionHemisphereDirectionalPdf);
    _Register("DomeReflectionHemisphereSampleMatchesDirectionalEvaluation",
              &TestDomeReflectionHemisphereSampleMatchesDirectionalEvaluation);
    _Register("DomePdfApproximatelyNormalizes",
              &TestDomePdfApproximatelyNormalizes);
    _Register("DirectionalShapingDistributionPdfNormalizes",
              &TestDirectionalShapingDistributionPdfNormalizes);
    _Register("IesDirectionalDistributionBuildsAndSamples",
              &TestIesDirectionalDistributionBuildsAndSamples);
    _Register("RectShapingAwareSampleMatchesDirectionalEvaluation",
              &TestRectShapingAwareSampleMatchesDirectionalEvaluation);
    _Register("RectFocusDirectionalSampleFoldsToEmissionHemisphere",
              &TestRectFocusDirectionalSampleFoldsToEmissionHemisphere);
    _Register("DiskShapingAwareSampleMatchesDirectionalEvaluation",
              &TestDiskShapingAwareSampleMatchesDirectionalEvaluation);
    _Register("SphereSampleMatchesDirectionalEvaluation",
              &TestSphereSampleMatchesDirectionalEvaluation);
    _Register("DistantDeltaSamplesLocalPositiveZ",
              &TestDistantDeltaSamplesLocalPositiveZ);
    _Register("DistantConeSampleMatchesDirectionalEvaluation",
              &TestDistantConeSampleMatchesDirectionalEvaluation);
    _Register("DistantNormalizeUsesUsdLuxSizeFactor",
              &TestDistantNormalizeUsesUsdLuxSizeFactor);

    for (const auto& entry : _Tests()) {
        ++_totalTests;
        std::printf("  [RUN ] %s\n", entry.name.c_str());
        bool passed = false;
        try {
            passed = entry.fn();
        } catch (const std::exception& e) {
            std::printf("  [EXCEPTION] %s: %s\n", entry.name.c_str(), e.what());
        }

        if (passed) {
            std::printf("  [PASS] %s\n", entry.name.c_str());
        } else {
            std::printf("  [FAIL] %s\n", entry.name.c_str());
            ++_failedTests;
        }
    }

    std::printf("\n%d/%d tests passed.\n", _totalTests - _failedTests, _totalTests);
    if (_failedTests > 0) {
        return 1;
    }
    return 0;
}
