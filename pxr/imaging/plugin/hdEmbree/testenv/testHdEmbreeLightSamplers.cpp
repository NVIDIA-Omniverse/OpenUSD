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
    _Register("SphereSampleMatchesDirectionalEvaluation",
              &TestSphereSampleMatchesDirectionalEvaluation);

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
