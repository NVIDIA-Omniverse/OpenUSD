//
// Copyright 2025 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include <delegate/light.h>
#include <renderer/lights/lightSampler.h>
#include <renderer/lights/lightSamplerCommon.h>

#include "pxr/base/gf/color.h"
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

bool
TestPhysicalScaleMultipliesEmission()
{
    ty::LightData light;
    light.color = GfVec3f(1.0f);
    light.intensity = 1.0f;
    light.diffuse = 1.0f;
    light.physicalScale = 3.0f;
    const GfVec3f radiance =
        ty::EvalLightBasic(light, ty::RenderColorSpace::Data);
    if (!_IsClose(radiance, GfVec3f(3.0f))) {
        std::printf(
            "    expected physical scale to produce (3, 3, 3), got "
            "(%f, %f, %f)\n",
            radiance[0], radiance[1], radiance[2]);
        return false;
    }
    return true;
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

ty::LightData
_MakeDomeLight(const std::vector<GfVec3f>& pixels, int width, int height)
{
    ty::LightData light;
    light.xformLightToWorld = GfMatrix4f(1.0f);
    light.xformWorldToLight = GfMatrix4f(1.0f);
    light.normalXformLightToWorld = GfMatrix3f(1.0f);
    light.color = GfVec3f(1.0f);
    light.texture.pixels = pixels;
    light.texture.width = width;
    light.texture.height = height;
    light.texture.colorSpaceName = GfColorSpaceNames->LinearRec709;
    light.lightVariant = ty::DomeLight{};
    ty::BuildDomeLightSamplingDistribution(&light.texture);
    return light;
}

ty::LightData
_MakeSphereLight(const GfVec3f& center, float radius)
{
    ty::LightData light;
    light.xformLightToWorld = GfMatrix4f(1.0f);
    light.xformLightToWorld.SetTranslate(center);
    light.xformWorldToLight = light.xformLightToWorld.GetInverse();
    light.normalXformLightToWorld = GfMatrix3f(1.0f);
    light.color = GfVec3f(1.0f);
    light.lightVariant = ty::SphereLight{radius};
    return light;
}

ty::LightData
_MakeRectLight(const GfVec3f& center, float width, float height)
{
    ty::LightData light;
    light.xformLightToWorld = GfMatrix4f(1.0f);
    light.xformLightToWorld.SetTranslate(center);
    light.xformWorldToLight = light.xformLightToWorld.GetInverse();
    light.normalXformLightToWorld = GfMatrix3f(1.0f);
    light.color = GfVec3f(1.0f);
    light.lightVariant = ty::RectLight{width, height};
    return light;
}

bool
TestDomeTextureConvertsToRenderColorSpace()
{
    const GfVec3f source(0.25f, 0.5f, 0.75f);
    const ty::LightData light =
        _MakeDomeLight({source}, 1, 1);
    const GfVec3f direction(0.0f, 0.0f, 1.0f);

    const auto raw =
        ty::LightSampler::EvaluateDomeLightDirection(
            light,
            direction,
            ty::RenderColorSpace::Data);
    if (!raw.valid || !_IsClose(raw.radianceIn, source)) {
        std::printf("    raw dome texture values were transformed\n");
        return false;
    }

    const auto ap1 =
        ty::LightSampler::EvaluateDomeLightDirection(
            light,
            direction,
            ty::RenderColorSpace::LinearAP1);
    const GfVec3f expected = GfColorSpace(
        GfColorSpaceNames->LinearAP1).Convert(
            GfColorSpace(GfColorSpaceNames->LinearRec709), source).GetRGB();
    if (!ap1.valid || !_IsClose(ap1.radianceIn, expected, 1.0e-5f)) {
        std::printf("    dome texture was not converted to Linear AP1\n");
        return false;
    }
    return true;
}

ty::LightData
_MakeDiskLight(const GfVec3f& center, float radius)
{
    ty::LightData light;
    light.xformLightToWorld = GfMatrix4f(1.0f);
    light.xformLightToWorld.SetTranslate(center);
    light.xformWorldToLight = light.xformLightToWorld.GetInverse();
    light.normalXformLightToWorld = GfMatrix3f(1.0f);
    light.color = GfVec3f(1.0f);
    light.lightVariant = ty::DiskLight{radius};
    return light;
}

ty::LightData
_MakeDistantLight(float angle)
{
    ty::LightData light;
    light.xformLightToWorld = GfMatrix4f(1.0f);
    light.xformWorldToLight = GfMatrix4f(1.0f);
    light.normalXformLightToWorld = GfMatrix3f(1.0f);
    light.color = GfVec3f(1.0f);
    light.intensity = 1.0f;
    light.diffuse = 1.0f;
    light.lightVariant = ty::DistantLight{angle};
    return light;
}

ty::LightData
_MakeCylinderLight(
    const GfVec3f& center,
    float radius,
    float length)
{
    ty::LightData light;
    light.xformLightToWorld = GfMatrix4f(1.0f);
    light.xformLightToWorld.SetTranslate(center);
    light.xformWorldToLight = light.xformLightToWorld.GetInverse();
    light.normalXformLightToWorld = GfMatrix3f(1.0f);
    light.color = GfVec3f(1.0f);
    light.lightVariant = ty::CylinderLight{radius, length};
    return light;
}

bool
TestDomeDistributionBuildsCdfs()
{
    const ty::LightData light = _MakeDomeLight(
        {
            GfVec3f(1.0f), GfVec3f(4.0f), GfVec3f(1.0f), GfVec3f(1.0f),
            GfVec3f(1.0f), GfVec3f(1.0f), GfVec3f(1.0f), GfVec3f(1.0f)
        },
        4,
        2);

    const ty::LightTexture& texture = light.texture;
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
    ty::LightTexture texture;
    texture.pixels = {GfVec3f(0.5f)};
    texture.width = 1;
    texture.height = 1;
    texture.colorSpaceName = GfColorSpaceNames->G22Rec709;

    ty::BuildDomeLightSamplingDistribution(&texture);

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
    const ty::LightData light = _MakeDomeLight(
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
        ty::LightSampler::EvaluateDomeLightDirection(light, brightDir);
    const auto dark =
        ty::LightSampler::EvaluateDomeLightDirection(light, darkDir);

    const float brightPdf = (bright.pdfSolidAngleInverse > 0.0f)
                                ? (1.0f / bright.pdfSolidAngleInverse)
                                : 0.0f;
    const float darkPdf = (dark.pdfSolidAngleInverse > 0.0f)
                              ? (1.0f / dark.pdfSolidAngleInverse)
                              : 0.0f;
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
    const ty::LightData light = _MakeDomeLight(
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
        const auto sampled = ty::LightSampler::GetLightSample(
            light, GfVec3f(0.0f), GfVec3f::YAxis(), u[0], u[1]);
        const auto evaluated = ty::LightSampler::EvaluateDomeLightDirection(
            light, sampled.omegaInWld);

        if (!_IsClose(sampled.radianceIn, evaluated.radianceIn, 1e-5f)) {
            std::printf("    sampled/evaluated radianceIn mismatch\n");
            return false;
        }
        if (!_IsClose(sampled.pdfSolidAngleInverse,
                      evaluated.pdfSolidAngleInverse, 1e-4f)) {
            std::printf("    sampled/evaluated invPdf mismatch: %f vs %f\n",
                        sampled.pdfSolidAngleInverse,
                        evaluated.pdfSolidAngleInverse);
            return false;
        }
    }

    return true;
}

bool
TestDomeReflectionHemisphereSampleUsesHemispherePdf()
{
    const ty::LightData light = _MakeDomeLight({}, 0, 0);
    const GfVec3f normal = GfVec3f::YAxis();
    const auto sampled = ty::LightSampler::GetLightSample(
        light,
        GfVec3f(0.0f),
        normal,
        0.25f,
        0.5f,
        ty::LightSampler::SamplingMode::ReflectionHemisphere);

    if (!sampled.valid) {
        std::printf("    expected valid hemisphere sample\n");
        return false;
    }
    if (!(GfDot(sampled.omegaInWld, normal) > 0.0f)) {
        std::printf("    sample was outside the reflection hemisphere\n");
        return false;
    }

    const float pdfSolidAngle = (sampled.pdfSolidAngleInverse > 0.0f)
                                    ? (1.0f / sampled.pdfSolidAngleInverse)
                                    : 0.0f;
    const float expectedPdf = 1.0f / (2.0f * static_cast<float>(M_PI));
    if (!_IsClose(pdfSolidAngle, expectedPdf, 1e-6f)) {
        std::printf("    expected hemisphere pdf %f, got %f\n", expectedPdf,
                    pdfSolidAngle);
        return false;
    }

    return true;
}

bool
TestDomeReflectionHemisphereDirectionalPdf()
{
    const ty::LightData light = _MakeDomeLight({}, 0, 0);
    const GfVec3f normal = GfVec3f::YAxis();
    const auto above = ty::LightSampler::EvaluateDomeLightDirection(
        light,
        GfVec3f::YAxis(),
        normal,
        ty::LightSampler::SamplingMode::ReflectionHemisphere);
    const auto below = ty::LightSampler::EvaluateDomeLightDirection(
        light,
        -GfVec3f::YAxis(),
        normal,
        ty::LightSampler::SamplingMode::ReflectionHemisphere);

    if (!above.valid) {
        std::printf("    expected direction above the surface to be valid\n");
        return false;
    }
    const float abovePdf = (above.pdfSolidAngleInverse > 0.0f)
                               ? (1.0f / above.pdfSolidAngleInverse)
                               : 0.0f;
    const float expectedPdf = 1.0f / (2.0f * static_cast<float>(M_PI));
    if (!_IsClose(abovePdf, expectedPdf, 1e-6f)) {
        std::printf("    expected hemisphere directional pdf %f, got %f\n",
                    expectedPdf, abovePdf);
        return false;
    }
    if (below.valid || below.pdfSolidAngleInverse != 0.0f) {
        std::printf("    expected direction below the surface to have zero pdf\n");
        return false;
    }

    return true;
}

bool
TestDomeReflectionHemisphereSampleMatchesDirectionalEvaluation()
{
    const ty::LightData light = _MakeDomeLight(
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
        const auto sampled = ty::LightSampler::GetLightSample(
            light,
            GfVec3f(0.0f),
            normal,
            u[0],
            u[1],
            ty::LightSampler::SamplingMode::ReflectionHemisphere);
        const auto evaluated = ty::LightSampler::EvaluateDomeLightDirection(
            light, sampled.omegaInWld, normal,
            ty::LightSampler::SamplingMode::ReflectionHemisphere);

        if (!sampled.valid || !(GfDot(sampled.omegaInWld, normal) > 0.0f)) {
            std::printf("    expected valid hemisphere sample\n");
            return false;
        }
        if (!_IsClose(sampled.radianceIn, evaluated.radianceIn, 1e-5f)) {
            std::printf(
                "    sampled/evaluated hemisphere radianceIn mismatch\n");
            return false;
        }
        if (!_IsClose(sampled.pdfSolidAngleInverse,
                      evaluated.pdfSolidAngleInverse, 1e-4f)) {
            std::printf(
                "    sampled/evaluated hemisphere invPdf mismatch: %f vs %f\n",
                sampled.pdfSolidAngleInverse, evaluated.pdfSolidAngleInverse);
            return false;
        }
    }

    return true;
}

bool
TestSphereSampleMatchesDirectionalEvaluation()
{
    const ty::LightData light =
        _MakeSphereLight(GfVec3f(0.0f, 0.0f, 4.0f), 1.0f);
    const GfVec3f position(0.0f);
    const GfVec3f normal = GfVec3f::ZAxis();
    const std::vector<GfVec2f> samples = {
        GfVec2f(0.1f, 0.2f),
        GfVec2f(0.4f, 0.7f),
        GfVec2f(0.9f, 0.95f),
    };

    const float cosThetaMax = std::sqrt(1.0f - 1.0f / 16.0f);
    const float pdfSolidAngleInverseExpected =
        2.0f * static_cast<float>(M_PI) * (1.0f - cosThetaMax);

    for (const GfVec2f& u : samples) {
        const auto sampled = ty::LightSampler::GetLightSample(
            light, position, normal, u[0], u[1]);
        const auto evaluated = ty::LightSampler::EvaluateLightDirection(
            light, position, sampled.omegaInWld);

        if (!sampled.valid || !evaluated.valid) {
            std::printf("    expected valid sphere light samples\n");
            return false;
        }
        if (!_IsClose(sampled.radianceIn, evaluated.radianceIn, 1e-5f)) {
            std::printf("    sampled/evaluated sphere radianceIn mismatch\n");
            return false;
        }
        if (!_IsClose(sampled.pdfSolidAngleInverse,
                      pdfSolidAngleInverseExpected, 1e-5f) ||
            !_IsClose(evaluated.pdfSolidAngleInverse,
                      pdfSolidAngleInverseExpected, 1e-5f)) {
            std::printf("    sphere invPdf mismatch: sampled=%f evaluated=%f "
                        "expected=%f\n",
                        sampled.pdfSolidAngleInverse,
                        evaluated.pdfSolidAngleInverse,
                        pdfSolidAngleInverseExpected);
            return false;
        }
    }

    return true;
}

bool
TestCylinderSampleMatchesDirectionalEvaluation()
{
    ty::LightData light =
        _MakeCylinderLight(GfVec3f(0.0f, 0.0f, 4.0f), 1.0f, 2.0f);
    light.texture.pixels = {
        GfVec3f(1.0f, 0.0f, 0.0f), GfVec3f(0.0f, 1.0f, 0.0f),
        GfVec3f(0.0f, 0.0f, 1.0f), GfVec3f(1.0f, 1.0f, 1.0f),
        GfVec3f(1.0f, 1.0f, 0.0f), GfVec3f(0.0f, 1.0f, 1.0f),
        GfVec3f(1.0f, 0.0f, 1.0f), GfVec3f(0.25f, 0.5f, 0.75f)};
    light.texture.width = 4;
    light.texture.height = 2;
    light.texture.colorSpaceName = GfColorSpaceNames->LinearRec709;
    const GfVec3f position(0.0f);
    const GfVec3f normal = GfVec3f::ZAxis();
    const ty::LightSampler::LightSample centerSideSample =
        ty::LightSampler::GetLightSample(
            light, position, normal, 0.5f, 0.75f);
    const float expectedPdfSolidAngleInverse =
        4.0f * static_cast<float>(M_PI) / 9.0f;
    if (!centerSideSample.valid ||
        !_IsClose(centerSideSample.omegaInWld, GfVec3f::ZAxis()) ||
        !_IsClose(centerSideSample.distanceWld, 3.0f) ||
        !_IsClose(
            centerSideSample.radianceIn, GfVec3f(0.25f, 0.5f, 0.75f)) ||
        !_IsClose(centerSideSample.pdfSolidAngleInverse,
                  expectedPdfSolidAngleInverse)) {
        std::printf("    center-side cylinder sample was not analytic result\n");
        return false;
    }

    const ty::LightSampler::LightSample openEnd =
        ty::LightSampler::EvaluateLightDirection(
            light,
            GfVec3f(3.0f, 0.0f, 4.0f),
            GfVec3f(-1.0f, 0.0f, 0.1f));
    if (openEnd.valid) {
        std::printf("    axial ray unexpectedly hit the open cylinder end\n");
        return false;
    }

    const std::vector<GfVec2f> samples = {
        GfVec2f(0.1f, 0.70f),
        GfVec2f(0.4f, 0.75f),
        GfVec2f(0.9f, 0.80f),
    };

    for (const GfVec2f& u : samples) {
        const ty::LightSampler::LightSample sampled =
            ty::LightSampler::GetLightSample(
                light, position, normal, u[0], u[1]);
        const ty::LightSampler::LightSample evaluated =
            ty::LightSampler::EvaluateLightDirection(
                light, position, sampled.omegaInWld);

        if (!sampled.valid || !evaluated.valid) {
            std::printf("    expected valid cylinder light samples\n");
            return false;
        }
        if (!_IsClose(sampled.radianceIn, evaluated.radianceIn, 1e-5f)) {
            std::printf("    sampled/evaluated cylinder radianceIn mismatch\n");
            return false;
        }
        if (!_IsClose(sampled.pdfSolidAngleInverse,
                      evaluated.pdfSolidAngleInverse, 1e-4f)) {
            std::printf(
                "    sampled/evaluated cylinder invPdf mismatch: %f vs %f\n",
                sampled.pdfSolidAngleInverse,
                evaluated.pdfSolidAngleInverse);
            return false;
        }
    }

    return true;
}

bool
TestDirectionalShapingDistributionPdfNormalizes()
{
    ty::Shaping shaping;
    shaping.coneAngle = 35.0f;
    shaping.coneSoftness = 0.25f;
    shaping.focus = 3.0f;
    ty::BuildDirectionalShapingDistribution(&shaping);

    if (!shaping.directionalDistribution.IsValid()) {
        std::printf("    expected valid directional shaping distribution\n");
        return false;
    }

    const ty::DirectionalShapingDistribution& distribution =
        shaping.directionalDistribution;
    constexpr int numPhi = ty::DirectionalShapingDistribution::NumPhi;
    float integral = 0.0f;
    for (int v = 0; v < distribution.NumRows(); ++v) {
        const float cellSolidAngle =
            2.0f * static_cast<float>(M_PI) *
            (distribution.rowCosThetaBounds[v] -
             distribution.rowCosThetaBounds[v + 1]) /
            static_cast<float>(numPhi);
        for (int h = 0; h < numPhi; ++h) {
            integral +=
                distribution.cellPdfSolidAngle[v * numPhi + h] * cellSolidAngle;
        }
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

    ty::Shaping shaping;
    if (!shaping.ies.iesFile.load(iesText)) {
        std::printf("    could not load synthetic IES profile\n");
        return false;
    }
    ty::BuildDirectionalShapingDistribution(&shaping);
    if (!shaping.directionalDistribution.IsValid()) {
        std::printf("    expected valid IES directional distribution\n");
        return false;
    }
    if (shaping.directionalDistribution.peakWeight <=
        shaping.directionalDistribution.averageWeight) {
        std::printf("    expected IES peak to exceed average weight\n");
        return false;
    }

    const ty::DirectionalShapingSample sample =
        ty::SampleDirectionalShaping(shaping, 0.25f, 0.75f);
    if (!sample.valid || sample.pdfSolidAngle <= 0.0f) {
        std::printf("    expected valid IES directional sample\n");
        return false;
    }

    const float evaluatedPdf =
        ty::DirectionalShapingPdf(shaping, sample.dirLight);
    if (!_IsClose(sample.pdfSolidAngle, evaluatedPdf, 1e-5f)) {
        std::printf("    sampled/evaluated IES pdf mismatch: %f vs %f\n",
                    sample.pdfSolidAngle, evaluatedPdf);
        return false;
    }

    return true;
}

// Checks that every stratified directional sample is valid, lies within
// the z range [zMin, zMax] (with slack), and that the sampled PDF matches
// the PDF lookup for the sampled direction.
bool
_CheckDirectionalSamplesConfined(
    const ty::Shaping& shaping,
    float zMin,
    float zMax,
    const char* label)
{
    constexpr int numU1 = 16;
    constexpr int numU2 = 16;
    for (int i = 0; i < numU1; ++i) {
        for (int j = 0; j < numU2; ++j) {
            const float u1 = (i + 0.5f) / numU1;
            const float u2 = (j + 0.5f) / numU2;
            const ty::DirectionalShapingSample sample =
                ty::SampleDirectionalShaping(shaping, u1, u2);
            if (!sample.valid || sample.pdfSolidAngle <= 0.0f) {
                std::printf("    %s: expected valid sample at u1=%f u2=%f\n",
                            label, u1, u2);
                return false;
            }
            const float z = sample.dirLight[2];
            if (z < zMin - 1e-4f || z > zMax + 1e-4f) {
                std::printf("    %s: sample outside support: z=%f\n",
                            label, z);
                return false;
            }
            const float evaluatedPdf = ty::DirectionalShapingPdf(
                shaping, sample.dirLight);
            if (!_IsClose(sample.pdfSolidAngle, evaluatedPdf,
                          1e-4f * sample.pdfSolidAngle)) {
                std::printf("    %s: sampled/evaluated pdf mismatch: "
                            "%f vs %f\n",
                            label, sample.pdfSolidAngle, evaluatedPdf);
                return false;
            }
        }
    }
    return true;
}

// Integrates the public PDF lookup over the sphere with a Riemann sum on
// an independent grid; the result must be ~1 for any valid distribution.
bool
_CheckDirectionalPdfIntegratesToOne(
    const ty::Shaping& shaping,
    const char* label)
{
    constexpr int numZ = 20000;
    constexpr int numPhi = 16;
    const float pi = static_cast<float>(M_PI);
    double integral = 0.0;
    for (int i = 0; i < numZ; ++i) {
        const float z = 1.0f - 2.0f * (i + 0.5f) / numZ;
        const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
        for (int j = 0; j < numPhi; ++j) {
            const float phi = 2.0f * pi * (j + 0.5f) / numPhi;
            const GfVec3f direction(
                r * std::cos(phi), r * std::sin(phi), z);
            integral += ty::DirectionalShapingPdf(shaping, direction);
        }
    }
    integral *= 4.0 * pi / (static_cast<double>(numZ) * numPhi);
    if (std::fabs(integral - 1.0) > 0.02) {
        std::printf("    %s: pdf integral mismatch: %f\n", label,
                    static_cast<float>(integral));
        return false;
    }
    return true;
}

bool
TestNarrowConeDirectionalDistributionSamplesWithinCone()
{
    // A hard 10 degree cone is narrower than a base distribution cell, so
    // the build must not lose it to discretization (weightSum == 0) and
    // every directional sample must land inside the cone.
    ty::Shaping shaping;
    shaping.coneAngle = 10.0f;
    shaping.coneSoftness = 0.0f;
    ty::BuildDirectionalShapingDistribution(&shaping);

    if (!shaping.directionalDistribution.IsValid()) {
        std::printf("    expected valid distribution for 10 degree cone\n");
        return false;
    }

    const float pi = static_cast<float>(M_PI);
    const float cosCone = std::cos(10.0f * pi / 180.0f);
    if (!_CheckDirectionalSamplesConfined(
            shaping, cosCone, 1.0f, "hard cone")) {
        return false;
    }

    // The PDF must be exactly zero outside the cone.
    const float thetaOutside = 12.0f * pi / 180.0f;
    const GfVec3f outsideDirection(
        std::sin(thetaOutside), 0.0f, std::cos(thetaOutside));
    const float outsidePdf =
        ty::DirectionalShapingPdf(shaping, outsideDirection);
    if (outsidePdf != 0.0f) {
        std::printf("    expected zero pdf outside cone, got %f\n",
                    outsidePdf);
        return false;
    }

    if (!_CheckDirectionalPdfIntegratesToOne(shaping, "hard cone")) {
        return false;
    }

    // A soft narrow cone must behave the same way; softness only narrows
    // the full-intensity core, so the support stays within the cone angle.
    ty::Shaping softShaping;
    softShaping.coneAngle = 10.0f;
    softShaping.coneSoftness = 0.8f;
    ty::BuildDirectionalShapingDistribution(&softShaping);
    if (!softShaping.directionalDistribution.IsValid()) {
        std::printf("    expected valid distribution for soft cone\n");
        return false;
    }
    if (!_CheckDirectionalSamplesConfined(
            softShaping, cosCone, 1.0f, "soft cone")) {
        return false;
    }

    // A degenerate zero-angle cone has no sampleable support; the build
    // must fall back to an invalid distribution rather than produce NaNs.
    ty::Shaping degenerateShaping;
    degenerateShaping.coneAngle = 0.0f;
    degenerateShaping.coneSoftness = 0.0f;
    ty::BuildDirectionalShapingDistribution(&degenerateShaping);
    if (degenerateShaping.directionalDistribution.IsValid()) {
        std::printf("    expected invalid distribution for zero cone\n");
        return false;
    }

    return true;
}

bool
TestNarrowIesBeamDirectionalDistributionIsValid()
{
    // A 1 degree IES beam is far narrower than a base distribution cell
    // and lies below the first quadrature sub-sample of the first base
    // row, so only the profile-declared row boundaries can preserve it.
    static const char* const iesText =
        "IESNA:LM-63-1995\n"
        "TILT=NONE\n"
        "1 1000 1 3 1 1 1 1 1 1 1 1 1\n"
        "0 1 180\n"
        "0\n"
        "1000 0 0\n";

    ty::Shaping shaping;
    if (!shaping.ies.iesFile.load(iesText)) {
        std::printf("    could not load synthetic IES profile\n");
        return false;
    }
    ty::BuildDirectionalShapingDistribution(&shaping);

    if (!shaping.directionalDistribution.IsValid()) {
        std::printf("    expected valid distribution for 1 degree beam\n");
        return false;
    }

    const float pi = static_cast<float>(M_PI);
    const float cosBeam = std::cos(1.0f * pi / 180.0f);
    if (!_CheckDirectionalSamplesConfined(
            shaping, cosBeam, 1.0f, "1 degree beam")) {
        return false;
    }

    const float thetaOutside = 3.0f * pi / 180.0f;
    const GfVec3f outsideDirection(
        std::sin(thetaOutside), 0.0f, std::cos(thetaOutside));
    const float outsidePdf =
        ty::DirectionalShapingPdf(shaping, outsideDirection);
    if (outsidePdf != 0.0f) {
        std::printf("    expected zero pdf outside beam, got %f\n",
                    outsidePdf);
        return false;
    }

    return true;
}

bool
TestIesAngleScaleCompressesBeamKnots()
{
    // angleScale > 0 rescales the profile toward theta == 0; a wide
    // profile compressed to a sub-degree beam is only sampleable if the
    // profile knots are remapped through the same transform the eval
    // uses. The 40 degree edge maps to 0.8 degrees with angleScale 0.02,
    // below the first quadrature sub-sample of the first base row.
    static const char* const iesText =
        "IESNA:LM-63-1995\n"
        "TILT=NONE\n"
        "1 1000 1 3 1 1 1 1 1 1 1 1 1\n"
        "0 40 180\n"
        "0\n"
        "1000 0 0\n";

    ty::Shaping shaping;
    if (!shaping.ies.iesFile.load(iesText)) {
        std::printf("    could not load synthetic IES profile\n");
        return false;
    }
    shaping.ies.angleScale = 0.02f;
    ty::BuildDirectionalShapingDistribution(&shaping);

    if (!shaping.directionalDistribution.IsValid()) {
        std::printf("    expected valid distribution for scaled beam\n");
        return false;
    }

    const float pi = static_cast<float>(M_PI);
    const float cosBeam = std::cos(0.8f * pi / 180.0f);
    return _CheckDirectionalSamplesConfined(
        shaping, cosBeam, 1.0f, "scaled beam");
}

bool
TestIesNegativeAngleScaleAnchorsBeamAtTop()
{
    // angleScale < 0 rescales the profile anchored at theta == pi; the
    // 0..40 degree profile beam lands in [179.1, 179.3] degrees with
    // angleScale -0.005, between the last quadrature sub-sample of the
    // bottom base row and the pole, so it again requires remapped knots.
    static const char* const iesText =
        "IESNA:LM-63-1995\n"
        "TILT=NONE\n"
        "1 1000 1 3 1 1 1 1 1 1 1 1 1\n"
        "0 40 180\n"
        "0\n"
        "1000 0 0\n";

    ty::Shaping shaping;
    if (!shaping.ies.iesFile.load(iesText)) {
        std::printf("    could not load synthetic IES profile\n");
        return false;
    }
    shaping.ies.angleScale = -0.005f;
    ty::BuildDirectionalShapingDistribution(&shaping);

    if (!shaping.directionalDistribution.IsValid()) {
        std::printf(
            "    expected valid distribution for negative-scale beam\n");
        return false;
    }

    const float pi = static_cast<float>(M_PI);
    const float zBeamMax = std::cos(179.1f * pi / 180.0f);
    return _CheckDirectionalSamplesConfined(
        shaping, -1.0f, zBeamMax, "negative-scale beam");
}

bool
TestRectShapingAwareSampleMatchesDirectionalEvaluation()
{
    ty::LightData light =
        _MakeRectLight(GfVec3f(0.0f, 0.0f, 4.0f), 100.0f, 100.0f);
    light.shaping.coneAngle = 25.0f;
    light.shaping.coneSoftness = 0.1f;
    ty::BuildDirectionalShapingDistribution(&light.shaping);

    const GfVec3f position(0.0f);
    const GfVec3f normal = GfVec3f::ZAxis();
    const std::vector<GfVec2f> samples = {
        GfVec2f(0.25f, 0.35f), // finite area strategy
        GfVec2f(0.75f, 0.65f), // directional shaping strategy
    };

    for (const GfVec2f& u : samples) {
        const auto sampled = ty::LightSampler::GetLightSample(
            light, position, normal, u[0], u[1]);
        if (!sampled.valid) {
            std::printf("    expected valid rect light sample\n");
            return false;
        }

        const auto evaluated = ty::LightSampler::EvaluateLightDirection(
            light, position, sampled.omegaInWld);
        if (!evaluated.valid) {
            std::printf("    expected rect sample direction to evaluate\n");
            return false;
        }
        if (!_IsClose(sampled.radianceIn, evaluated.radianceIn, 1e-5f)) {
            std::printf("    sampled/evaluated rect radianceIn mismatch\n");
            return false;
        }
        if (!_IsClose(sampled.pdfSolidAngleInverse,
                      evaluated.pdfSolidAngleInverse, 1e-4f)) {
            std::printf(
                "    sampled/evaluated rect invPdf mismatch: %f vs %f\n",
                sampled.pdfSolidAngleInverse, evaluated.pdfSolidAngleInverse);
            return false;
        }
    }

    return true;
}

bool
TestRectTextureOriginUsesLocalPositiveXY()
{
    ty::LightData light =
        _MakeRectLight(GfVec3f(0.0f, 0.0f, 4.0f), 2.0f, 2.0f);
    light.texture.width = 2;
    light.texture.height = 2;
    light.texture.pixels = {
        GfVec3f(1.0f, 0.0f, 0.0f),
        GfVec3f(0.0f, 1.0f, 0.0f),
        GfVec3f(0.0f, 0.0f, 1.0f),
        GfVec3f(1.0f, 1.0f, 0.0f),
    };

    const GfVec3f position(0.0f);
    const GfVec3f normal = GfVec3f::ZAxis();
    const struct {
        GfVec3f hitPoint;
        GfVec2f sampleUv;
        GfVec3f expectedLi;
    } cases[] = {
        {GfVec3f(0.5f, 0.5f, 4.0f),
         GfVec2f(0.75f, 0.75f),
         GfVec3f(1.0f, 0.0f, 0.0f)},
        {GfVec3f(-0.5f, 0.5f, 4.0f),
         GfVec2f(0.25f, 0.75f),
         GfVec3f(0.0f, 1.0f, 0.0f)},
        {GfVec3f(0.5f, -0.5f, 4.0f),
         GfVec2f(0.75f, 0.25f),
         GfVec3f(0.0f, 0.0f, 1.0f)},
        {GfVec3f(-0.5f, -0.5f, 4.0f),
         GfVec2f(0.25f, 0.25f),
         GfVec3f(1.0f, 1.0f, 0.0f)},
    };

    for (const auto& testCase : cases) {
        const GfVec3f direction =
            (testCase.hitPoint - position).GetNormalized();
        const auto evaluated = ty::LightSampler::EvaluateLightDirection(
            light, position, direction);
        if (!evaluated.valid) {
            std::printf("    expected textured rect direction to evaluate\n");
            return false;
        }
        if (!_IsClose(evaluated.radianceIn, testCase.expectedLi, 1e-5f)) {
            std::printf("    expected evaluated radianceIn (%f, %f, %f), got "
                        "(%f, %f, %f)\n",
                        testCase.expectedLi[0], testCase.expectedLi[1],
                        testCase.expectedLi[2], evaluated.radianceIn[0],
                        evaluated.radianceIn[1], evaluated.radianceIn[2]);
            return false;
        }

        const auto sampled = ty::LightSampler::GetLightSample(
            light, position, normal, testCase.sampleUv[0], testCase.sampleUv[1]);
        if (!sampled.valid) {
            std::printf("    expected textured rect sample to be valid\n");
            return false;
        }
        if (!_IsClose(sampled.radianceIn, testCase.expectedLi, 1e-5f)) {
            std::printf("    expected sampled radianceIn (%f, %f, %f), got "
                        "(%f, %f, %f)\n",
                        testCase.expectedLi[0], testCase.expectedLi[1],
                        testCase.expectedLi[2], sampled.radianceIn[0],
                        sampled.radianceIn[1], sampled.radianceIn[2]);
            return false;
        }
    }

    return true;
}

bool
TestRectFocusDirectionalSampleFoldsToEmissionHemisphere()
{
    ty::LightData light =
        _MakeRectLight(GfVec3f(0.0f, 0.0f, 4.0f), 100.0f, 100.0f);
    light.shaping.focus = 4.0f;
    ty::BuildDirectionalShapingDistribution(&light.shaping);

    float samplerU1 = 0.0f;
    bool foundBackHemisphereProposal = false;
    for (float shapingU1 : {0.55f, 0.65f, 0.75f, 0.85f, 0.95f}) {
        const ty::DirectionalShapingSample proposal =
            ty::SampleDirectionalShaping(light.shaping, shapingU1, 0.37f);
        if (proposal.valid && proposal.dirLight[2] < 0.0f) {
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
    const auto sampled = ty::LightSampler::GetLightSample(
        light, position, GfVec3f::ZAxis(), samplerU1, 0.37f);
    if (!sampled.valid || sampled.omegaInWld[2] <= 0.0f) {
        std::printf("    expected folded rect focus sample to hit +Z side\n");
        return false;
    }

    const auto evaluated = ty::LightSampler::EvaluateLightDirection(
        light, position, sampled.omegaInWld);
    if (!evaluated.valid) {
        std::printf("    expected folded rect focus direction to evaluate\n");
        return false;
    }
    if (!_IsClose(sampled.radianceIn, evaluated.radianceIn, 1e-5f)) {
        std::printf("    sampled/evaluated folded rect radianceIn mismatch\n");
        return false;
    }
    if (!_IsClose(sampled.pdfSolidAngleInverse, evaluated.pdfSolidAngleInverse,
                  1e-4f)) {
        std::printf(
            "    sampled/evaluated folded rect invPdf mismatch: %f vs %f\n",
            sampled.pdfSolidAngleInverse, evaluated.pdfSolidAngleInverse);
        return false;
    }

    return true;
}

bool
TestDiskShapingAwareSampleMatchesDirectionalEvaluation()
{
    ty::LightData light =
        _MakeDiskLight(GfVec3f(0.0f, 0.0f, 4.0f), 50.0f);
    light.shaping.coneAngle = 25.0f;
    light.shaping.coneSoftness = 0.1f;
    ty::BuildDirectionalShapingDistribution(&light.shaping);

    const GfVec3f position(0.0f);
    const GfVec3f normal = GfVec3f::ZAxis();
    const std::vector<GfVec2f> samples = {
        GfVec2f(0.25f, 0.35f),
        GfVec2f(0.75f, 0.65f),
    };

    for (const GfVec2f& u : samples) {
        const auto sampled = ty::LightSampler::GetLightSample(
            light, position, normal, u[0], u[1]);
        if (!sampled.valid) {
            std::printf("    expected valid disk light sample\n");
            return false;
        }

        const auto evaluated = ty::LightSampler::EvaluateLightDirection(
            light, position, sampled.omegaInWld);
        if (!evaluated.valid) {
            std::printf("    expected disk sample direction to evaluate\n");
            return false;
        }
        if (!_IsClose(sampled.radianceIn, evaluated.radianceIn, 1e-5f)) {
            std::printf("    sampled/evaluated disk radianceIn mismatch\n");
            return false;
        }
        if (!_IsClose(sampled.pdfSolidAngleInverse,
                      evaluated.pdfSolidAngleInverse, 1e-4f)) {
            std::printf(
                "    sampled/evaluated disk invPdf mismatch: %f vs %f\n",
                sampled.pdfSolidAngleInverse, evaluated.pdfSolidAngleInverse);
            return false;
        }
    }

    return true;
}

bool
TestDistantDeltaSamplesLocalPositiveZ()
{
    const ty::LightData light = _MakeDistantLight(0.0f);

    const auto sampled = ty::LightSampler::GetLightSample(
        light,
        GfVec3f(0.0f),
        GfVec3f::ZAxis(),
        0.37f,
        0.91f);
    if (!sampled.valid || !sampled.delta) {
        std::printf("    expected valid delta distant light sample\n");
        return false;
    }
    if (!_IsClose(sampled.omegaInWld, GfVec3f::ZAxis(), 1e-6f)) {
        std::printf(
            "    expected local +Z sample direction, got (%f, %f, %f)\n",
            sampled.omegaInWld[0], sampled.omegaInWld[1],
            sampled.omegaInWld[2]);
        return false;
    }
    if (!_IsClose(sampled.pdfSolidAngleInverse, 1.0f, 1e-6f)) {
        std::printf("    expected unit delta inverse pdf, got %f\n",
                    sampled.pdfSolidAngleInverse);
        return false;
    }

    const auto evaluated = ty::LightSampler::EvaluateLightDirection(
        light, GfVec3f(0.0f), GfVec3f::ZAxis());
    const auto opposite = ty::LightSampler::EvaluateLightDirection(
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
    const ty::LightData light = _MakeDistantLight(60.0f);
    const float thetaMax = 0.5f * 60.0f *
        static_cast<float>(M_PI) / 180.0f;
    const float pdfSolidAngleInverseExpected =
        2.0f * static_cast<float>(M_PI) * (1.0f - std::cos(thetaMax));

    const std::vector<GfVec2f> samples = {
        GfVec2f(0.0f, 0.0f),
        GfVec2f(0.25f, 0.5f),
        GfVec2f(0.8f, 0.9f),
    };

    for (const GfVec2f& u : samples) {
        const auto sampled = ty::LightSampler::GetLightSample(
            light, GfVec3f(0.0f), GfVec3f::ZAxis(), u[0], u[1]);
        const auto evaluated = ty::LightSampler::EvaluateLightDirection(
            light, GfVec3f(0.0f), sampled.omegaInWld);

        if (!sampled.valid || sampled.delta || !evaluated.valid ||
            evaluated.delta) {
            std::printf("    expected valid non-delta distant cone samples\n");
            return false;
        }
        if (!_IsClose(sampled.radianceIn, evaluated.radianceIn, 1e-5f)) {
            std::printf("    sampled/evaluated distant radianceIn mismatch\n");
            return false;
        }
        if (!_IsClose(sampled.pdfSolidAngleInverse,
                      pdfSolidAngleInverseExpected, 1e-5f) ||
            !_IsClose(evaluated.pdfSolidAngleInverse,
                      pdfSolidAngleInverseExpected, 1e-5f)) {
            std::printf("    distant invPdf mismatch: sampled=%f evaluated=%f "
                        "expected=%f\n",
                        sampled.pdfSolidAngleInverse,
                        evaluated.pdfSolidAngleInverse,
                        pdfSolidAngleInverseExpected);
            return false;
        }
    }

    return true;
}

bool
TestDistantNormalizeUsesUsdLuxSizeFactor()
{
    ty::LightData light = _MakeDistantLight(60.0f);
    light.normalize = true;
    light.intensity = 10.0f;

    const auto sampled = ty::LightSampler::GetLightSample(
        light, GfVec3f(0.0f), GfVec3f::ZAxis(), 0.5f, 0.25f);

    const float thetaMax = 0.5f * 60.0f *
        static_cast<float>(M_PI) / 180.0f;
    const float sinTheta = std::sin(thetaMax);
    const float sizeFactor =
        sinTheta * sinTheta * static_cast<float>(M_PI);
    const GfVec3f expected(10.0f / sizeFactor);
    if (!sampled.valid || !_IsClose(sampled.radianceIn, expected, 1e-5f)) {
        std::printf(
            "    expected normalized distant radianceIn %f, got (%f, %f, %f)\n",
            expected[0], sampled.radianceIn[0], sampled.radianceIn[1],
            sampled.radianceIn[2]);
        return false;
    }

    return true;
}

bool
TestDomePdfApproximatelyNormalizes()
{
    const ty::LightData light = _MakeDomeLight(
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
                ty::LightSampler::EvaluateDomeLightDirection(light, dir);
            const float pdfSolidAngle =
                (evaluated.pdfSolidAngleInverse > 0.0f)
                    ? (1.0f / evaluated.pdfSolidAngleInverse)
                    : 0.0f;
            integral += pdfSolidAngle * jacobian;
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
    _Register("PhysicalScaleMultipliesEmission",
              &TestPhysicalScaleMultipliesEmission);
    _Register("DomeDistributionBuildsCdfs", &TestDomeDistributionBuildsCdfs);
    _Register("DomeTextureConvertsToRenderColorSpace",
              &TestDomeTextureConvertsToRenderColorSpace);
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
    _Register("NarrowConeDirectionalDistributionSamplesWithinCone",
              &TestNarrowConeDirectionalDistributionSamplesWithinCone);
    _Register("NarrowIesBeamDirectionalDistributionIsValid",
              &TestNarrowIesBeamDirectionalDistributionIsValid);
    _Register("IesAngleScaleCompressesBeamKnots",
              &TestIesAngleScaleCompressesBeamKnots);
    _Register("IesNegativeAngleScaleAnchorsBeamAtTop",
              &TestIesNegativeAngleScaleAnchorsBeamAtTop);
    _Register("RectShapingAwareSampleMatchesDirectionalEvaluation",
              &TestRectShapingAwareSampleMatchesDirectionalEvaluation);
    _Register("RectTextureOriginUsesLocalPositiveXY",
              &TestRectTextureOriginUsesLocalPositiveXY);
    _Register("RectFocusDirectionalSampleFoldsToEmissionHemisphere",
              &TestRectFocusDirectionalSampleFoldsToEmissionHemisphere);
    _Register("DiskShapingAwareSampleMatchesDirectionalEvaluation",
              &TestDiskShapingAwareSampleMatchesDirectionalEvaluation);
    _Register("SphereSampleMatchesDirectionalEvaluation",
              &TestSphereSampleMatchesDirectionalEvaluation);
    _Register("CylinderSampleMatchesDirectionalEvaluation",
              &TestCylinderSampleMatchesDirectionalEvaluation);
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
