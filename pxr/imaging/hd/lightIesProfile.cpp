/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */
//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/imaging/hd/lightIesProfile.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

constexpr float _Pi = 3.14159265358979323846f;
constexpr float _HemisphereFudgeFactor = 0.1f;

bool
_AngleClose(float a, float b)
{
    return std::fabs(a - b) < 1.0e-4f;
}

class _IesTextParser
{
public:
    explicit _IesTextParser(std::string const& str)
        : _text(str)
    {
        std::replace(_text.begin(), _text.end(), ',', ' ');
        _data = std::strstr(&_text[0], "\nTILT=");
    }

    bool Eof() const
    {
        return _data == nullptr || _data[0] == '\0';
    }

    bool HasError() const
    {
        return _error;
    }

    double GetDouble()
    {
        if (Eof()) {
            _error = true;
            return 0.0;
        }
        char* const oldData = _data;
        const double value = std::strtod(_data, &_data);
        if (_data == oldData) {
            _data = nullptr;
            _error = true;
            return 0.0;
        }
        return value;
    }

    long GetLong()
    {
        if (Eof()) {
            _error = true;
            return 0;
        }
        char* const oldData = _data;
        const long value = std::strtol(_data, &_data, 10);
        if (_data == oldData) {
            _data = nullptr;
            _error = true;
            return 0;
        }
        return value;
    }

    char*& Data()
    {
        return _data;
    }

private:
    std::string _text;
    char* _data = nullptr;
    bool _error = false;
};

} // anonymous namespace

bool
HdLightIesProfile::Load(std::string const& ies)
{
    Clear();
    if (!_Parse(ies) || !_Process()) {
        Clear();
        return false;
    }
    _ComputePower();
    return IsValid();
}

void
HdLightIesProfile::Clear()
{
    _intensity.clear();
    _vAngles.clear();
    _hAngles.clear();
    _power = 0.0f;
    _type = _TypeC;
}

bool
HdLightIesProfile::_Parse(std::string const& ies)
{
    if (ies.empty()) {
        return false;
    }

    _IesTextParser parser(ies);
    if (parser.Eof()) {
        return false;
    }

    if (std::strncmp(parser.Data(), "\nTILT=INCLUDE", 13) == 0) {
        parser.Data() += 13;
        parser.GetDouble();
        const int numTilt = parser.GetLong();
        for (int i = 0; i < 2 * numTilt; ++i) {
            parser.GetDouble();
        }
    } else {
        parser.Data() = std::strstr(parser.Data() + 1, "\n");
    }

    if (parser.Eof()) {
        return false;
    }
    parser.Data()++;

    parser.GetLong();
    parser.GetDouble();
    double factor = parser.GetDouble();
    const int vAnglesNum = parser.GetLong();
    const int hAnglesNum = parser.GetLong();
    _type = static_cast<_IESType>(parser.GetLong());

    if (_type != _TypeA && _type != _TypeB && _type != _TypeC) {
        return false;
    }
    if (vAnglesNum <= 0 || hAnglesNum <= 0) {
        return false;
    }

    parser.GetLong();
    parser.GetDouble();
    parser.GetDouble();
    parser.GetDouble();
    factor *= parser.GetDouble();
    factor *= parser.GetDouble();
    parser.GetDouble();

    _vAngles.reserve(static_cast<size_t>(vAnglesNum));
    for (int i = 0; i < vAnglesNum; ++i) {
        _vAngles.push_back(static_cast<float>(parser.GetDouble()));
    }

    _hAngles.reserve(static_cast<size_t>(hAnglesNum));
    for (int i = 0; i < hAnglesNum; ++i) {
        _hAngles.push_back(static_cast<float>(parser.GetDouble()));
    }

    _intensity.resize(static_cast<size_t>(hAnglesNum));
    for (int h = 0; h < hAnglesNum; ++h) {
        _intensity[h].reserve(static_cast<size_t>(vAnglesNum));
        for (int v = 0; v < vAnglesNum; ++v) {
            _intensity[h].push_back(
                static_cast<float>(factor * parser.GetDouble()));
        }
    }

    return !parser.HasError();
}

void
HdLightIesProfile::_ProcessTypeB()
{
    std::vector<std::vector<float>> newIntensity;
    newIntensity.resize(_vAngles.size());
    for (size_t i = 0; i < _vAngles.size(); ++i) {
        newIntensity[i].reserve(_hAngles.size());
        for (size_t j = 0; j < _hAngles.size(); ++j) {
            newIntensity[i].push_back(_intensity[j][i]);
        }
    }
    _intensity.swap(newIntensity);
    _hAngles.swap(_vAngles);

    if (_AngleClose(_hAngles[0], 0.0f)) {
        std::vector<float> newHAngles;
        std::vector<std::vector<float>> newIntensity;
        const int hnum = static_cast<int>(_hAngles.size());
        newHAngles.reserve(static_cast<size_t>(2 * hnum - 1));
        newIntensity.reserve(static_cast<size_t>(2 * hnum - 1));
        for (int i = hnum - 1; i > 0; --i) {
            newHAngles.push_back(90.0f - _hAngles[static_cast<size_t>(i)]);
            newIntensity.push_back(_intensity[static_cast<size_t>(i)]);
        }
        for (int i = 0; i < hnum; ++i) {
            newHAngles.push_back(90.0f + _hAngles[static_cast<size_t>(i)]);
            newIntensity.push_back(_intensity[static_cast<size_t>(i)]);
        }
        _hAngles.swap(newHAngles);
        _intensity.swap(newIntensity);
    } else {
        for (float& angle : _hAngles) {
            angle += 90.0f;
        }
    }

    if (_AngleClose(_vAngles[0], 0.0f)) {
        std::vector<float> newVAngles;
        const int hnum = static_cast<int>(_hAngles.size());
        const int vnum = static_cast<int>(_vAngles.size());
        newVAngles.reserve(static_cast<size_t>(2 * vnum - 1));
        for (int i = vnum - 1; i > 0; --i) {
            newVAngles.push_back(90.0f - _vAngles[static_cast<size_t>(i)]);
        }
        for (int i = 0; i < vnum; ++i) {
            newVAngles.push_back(90.0f + _vAngles[static_cast<size_t>(i)]);
        }
        for (int i = 0; i < hnum; ++i) {
            std::vector<float> newIntensityRow;
            newIntensityRow.reserve(static_cast<size_t>(2 * vnum - 1));
            for (int j = vnum - 1; j > 0; --j) {
                newIntensityRow.push_back(
                    _intensity[static_cast<size_t>(i)][static_cast<size_t>(j)]);
            }
            newIntensityRow.insert(newIntensityRow.end(),
                _intensity[static_cast<size_t>(i)].begin(),
                _intensity[static_cast<size_t>(i)].end());
            _intensity[static_cast<size_t>(i)].swap(newIntensityRow);
        }
        _vAngles.swap(newVAngles);
    } else {
        for (float& angle : _vAngles) {
            angle += 90.0f;
        }
    }
}

void
HdLightIesProfile::_ProcessTypeA()
{
    for (float& angle : _vAngles) {
        angle += 90.0f;
    }

    std::vector<float> newHAngles;
    std::vector<std::vector<float>> newIntensity;
    newHAngles.reserve(_hAngles.size());
    newIntensity.reserve(_hAngles.size());

    for (size_t i = _hAngles.size(); i-- > 0;) {
        newHAngles.push_back(180.0f - _hAngles[i]);
        newIntensity.push_back(_intensity[i]);
    }

    if (_AngleClose(_hAngles[0], 0.0f)) {
        newHAngles.reserve(2 * _hAngles.size() - 1);
        newIntensity.reserve(2 * _hAngles.size() - 1);
        for (size_t i = 1; i < _hAngles.size(); ++i) {
            newHAngles.push_back(180.0f + _hAngles[i]);
            newIntensity.push_back(_intensity[i]);
        }
    }

    _hAngles.swap(newHAngles);
    _intensity.swap(newIntensity);
}

void
HdLightIesProfile::_ProcessTypeC()
{
    if (_AngleClose(_hAngles[0], 90.0f)) {
        for (float& angle : _hAngles) {
            angle -= 90.0f;
        }
    }

    if (_hAngles.size() == 1) {
        _hAngles[0] = 0.0f;
        _hAngles.push_back(360.0f);
        _intensity.push_back(_intensity[0]);
    }

    if (_AngleClose(_hAngles.back(), 90.0f)) {
        const int hnum = static_cast<int>(_hAngles.size());
        for (int i = hnum - 2; i >= 0; --i) {
            _hAngles.push_back(180.0f - _hAngles[static_cast<size_t>(i)]);
            _intensity.push_back(_intensity[static_cast<size_t>(i)]);
        }
    }

    if (_AngleClose(_hAngles.back(), 180.0f)) {
        const int hnum = static_cast<int>(_hAngles.size());
        for (int i = hnum - 2; i >= 0; --i) {
            _hAngles.push_back(360.0f - _hAngles[static_cast<size_t>(i)]);
            _intensity.push_back(_intensity[static_cast<size_t>(i)]);
        }
    }

    if (_hAngles.size() > 1 &&
        _AngleClose(_hAngles[0], 0.0f) &&
        !_AngleClose(_hAngles.back(), 360.0f)) {
        const int hnum = static_cast<int>(_hAngles.size());
        const float lastStep = _hAngles[static_cast<size_t>(hnum - 1)] -
            _hAngles[static_cast<size_t>(hnum - 2)];
        const float firstStep = _hAngles[1] - _hAngles[0];
        const float gapStep = 360.0f - _hAngles.back();
        if (_AngleClose(lastStep, gapStep) || _AngleClose(firstStep, gapStep)) {
            _hAngles.push_back(360.0f);
            _intensity.push_back(_intensity[0]);
        }
    }
}

bool
HdLightIesProfile::_Process()
{
    if (_hAngles.empty() || _vAngles.empty() || _intensity.empty()) {
        return false;
    }

    if (_type == _TypeA) {
        _ProcessTypeA();
    } else if (_type == _TypeB) {
        _ProcessTypeB();
    } else if (_type == _TypeC) {
        _ProcessTypeC();
    } else {
        return false;
    }

    for (float& angle : _vAngles) {
        angle *= _Pi / 180.0f;
    }
    for (float& angle : _hAngles) {
        angle *= _Pi / 180.0f;
    }

    return !_hAngles.empty() && !_vAngles.empty() && !_intensity.empty();
}

void
HdLightIesProfile::_ComputePower()
{
    _power = 0.0f;
    if (_hAngles.size() < 2 || _vAngles.size() < 2 || _intensity.size() < 2) {
        return;
    }

    const auto [vAngleMin, vAngleMax] = std::minmax_element(
        _vAngles.cbegin(), _vAngles.cend());
    const bool isSphere = (*vAngleMax - *vAngleMin) >
        (_Pi / 2.0f + _HemisphereFudgeFactor);

    for (size_t h = 0; h + 1 < _hAngles.size(); ++h) {
        if (h + 1 >= _intensity.size()) {
            break;
        }
        for (size_t v = 0; v + 1 < _vAngles.size(); ++v) {
            if (v + 1 >= _intensity[h].size() ||
                v + 1 >= _intensity[h + 1].size()) {
                continue;
            }
            const float dh = _hAngles[h + 1] - _hAngles[h];
            const float dv = _vAngles[v + 1] - _vAngles[v];
            const float i0 = 0.5f * (_intensity[h][v] + _intensity[h][v + 1]);
            const float i1 = 0.5f * (_intensity[h + 1][v] + _intensity[h + 1][v + 1]);
            const float centerIntensity = 0.5f * (i0 + i1);
            const float dS = dh * dv * std::sin(_vAngles[v] + 0.5f * dv);
            _power += dS * centerIntensity;
        }
    }

    _power /= _Pi * (isSphere ? 4.0f : 2.0f);
}

PXR_NAMESPACE_CLOSE_SCOPE
