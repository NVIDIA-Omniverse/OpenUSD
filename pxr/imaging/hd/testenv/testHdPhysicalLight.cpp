//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/imaging/hd/light.h"

#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/vt/value.h"
#include "pxr/imaging/hd/sceneDelegate.h"
#include "pxr/imaging/hd/tokens.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <unordered_map>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

constexpr float _pi = 3.14159265358979323846f;
const SdfPath _lightPath("/Light");

class _Delegate final : public HdSceneDelegate
{
public:
    _Delegate()
        : HdSceneDelegate(nullptr, SdfPath::AbsoluteRootPath())
    {
    }

    void Set(TfToken const& key, VtValue const& value)
    {
        _values[key] = value;
    }

    void SetTransform(GfMatrix4d const& transform)
    {
        _transform = transform;
    }

    VtValue GetLightParamValue(
        SdfPath const&, TfToken const& paramName) override
    {
        const std::unordered_map<
            TfToken, VtValue, TfToken::HashFunctor>::const_iterator it =
                _values.find(paramName);
        return it == _values.end() ? VtValue() : it->second;
    }

    GfMatrix4d GetTransform(SdfPath const&) override
    {
        return _transform;
    }

private:
    std::unordered_map<TfToken, VtValue, TfToken::HashFunctor> _values;
    GfMatrix4d _transform = GfMatrix4d(1.0);
};

void
_SetUnitEmission(_Delegate* delegate)
{
    delegate->Set(HdLightTokens->color, VtValue(GfVec3f(1.0f)));
    delegate->Set(HdLightTokens->intensity, VtValue(1.0f));
    delegate->Set(HdLightTokens->diffuse, VtValue(1.0f));
    delegate->Set(HdLightTokens->exposure, VtValue(0.0f));
    delegate->Set(HdLightTokens->enableColorTemperature, VtValue(false));
}

bool
_IsClose(float actual, float expected, float tolerance = 1.0e-5f)
{
    const float scale = std::max(
        1.0f, std::max(std::abs(actual), std::abs(expected)));
    return std::abs(actual - expected) <= tolerance * scale;
}

bool
_ExpectScale(
    _Delegate* delegate, TfToken const& lightType, float expected,
    char const* description)
{
    const float actual = HdLight::ComputePhysicalScalingFactor(
        delegate, _lightPath, lightType);
    if (_IsClose(actual, expected)) {
        return true;
    }
    std::cerr << description << ": expected " << expected
              << ", got " << actual << "\n";
    return false;
}

bool
_TestDefaultsAndAreaPower()
{
    _Delegate delegate;
    _SetUnitEmission(&delegate);
    delegate.Set(HdLightTokens->width, VtValue(2.0f));
    delegate.Set(HdLightTokens->height, VtValue(3.0f));
    if (!_ExpectScale(
            &delegate, HdSprimTypeTokens->rectLight, 1.0f,
            "default physical scale")) {
        return false;
    }

    delegate.Set(HdLightTokens->photometricPower, VtValue(600.0f));
    if (!_ExpectScale(
            &delegate, HdSprimTypeTokens->rectLight,
            600.0f / (_pi * 6.0f), "rect power")) {
        return false;
    }

    delegate.Set(HdLightTokens->normalize, VtValue(true));
    return _ExpectScale(
        &delegate, HdSprimTypeTokens->rectLight,
        600.0f / _pi, "normalized rect power");
}

bool
_TestSceneUnitsAndTransform()
{
    _Delegate delegate;
    _SetUnitEmission(&delegate);
    delegate.Set(HdLightTokens->width, VtValue(2.0f));
    delegate.Set(HdLightTokens->height, VtValue(3.0f));
    delegate.Set(HdLightTokens->metersPerUnit, VtValue(0.01));
    delegate.Set(HdLightTokens->photometricPower, VtValue(3600.0f));
    GfMatrix4d transform(1.0);
    transform.SetScale(GfVec3d(2.0, 3.0, 1.0));
    delegate.SetTransform(transform);

    return _ExpectScale(
        &delegate, HdSprimTypeTokens->rectLight,
        100.0f / (_pi * 0.0001f),
        "scene-unit and transform-scaled rect power");
}

bool
_TestAreaIlluminancePrecedence()
{
    _Delegate delegate;
    _SetUnitEmission(&delegate);
    delegate.Set(HdLightTokens->radius, VtValue(1.0f));
    delegate.Set(HdLightTokens->photometricPower, VtValue(1.0f));
    delegate.Set(HdLightTokens->photometricIlluminance, VtValue(100.0f));
    delegate.Set(
        HdLightTokens->photometricIlluminanceDistance, VtValue(2.0f));

    if (!_ExpectScale(
            &delegate, HdSprimTypeTokens->diskLight,
            500.0f / _pi, "disk illuminance")) {
        return false;
    }
    delegate.Set(HdLightTokens->normalize, VtValue(true));
    return _ExpectScale(
        &delegate, HdSprimTypeTokens->diskLight,
        500.0f, "normalized disk illuminance");
}

bool
_TestDistantNormalization()
{
    _Delegate delegate;
    _SetUnitEmission(&delegate);
    delegate.Set(HdLightTokens->angle, VtValue(60.0f));
    delegate.Set(HdLightTokens->photometricIlluminance, VtValue(100.0f));
    if (!_ExpectScale(
            &delegate, HdSprimTypeTokens->distantLight,
            400.0f / _pi, "distant illuminance")) {
        return false;
    }

    delegate.Set(HdLightTokens->normalize, VtValue(true));
    if (!_ExpectScale(
            &delegate, HdSprimTypeTokens->distantLight,
            100.0f, "normalized distant illuminance")) {
        return false;
    }

    delegate.Set(HdLightTokens->angle, VtValue(240.0f));
    const float legacyNormalizeMeasure = 1.25f * _pi;
    return _ExpectScale(
        &delegate, HdSprimTypeTokens->distantLight,
        100.0f * legacyNormalizeMeasure / _pi,
        "wide normalized distant illuminance compensation");
}

bool
_TestTexturelessDome()
{
    _Delegate delegate;
    _SetUnitEmission(&delegate);
    delegate.Set(HdLightTokens->photometricIlluminance, VtValue(10000.0f));
    return _ExpectScale(
        &delegate, HdSprimTypeTokens->domeLight,
        10000.0f / _pi, "textureless dome illuminance");
}

} // namespace

int
main()
{
    const bool defaultsAndAreaPower = _TestDefaultsAndAreaPower();
    const bool sceneUnitsAndTransform = _TestSceneUnitsAndTransform();
    const bool areaIlluminancePrecedence = _TestAreaIlluminancePrecedence();
    const bool distantNormalization = _TestDistantNormalization();
    const bool texturelessDome = _TestTexturelessDome();
    return defaultsAndAreaPower && sceneUnitsAndTransform &&
                   areaIlluminancePrecedence && distantNormalization &&
                   texturelessDome
        ? 0
        : 1;
}
