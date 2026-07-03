/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */
//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_IMAGING_HD_LIGHT_IES_PROFILE_H
#define PXR_IMAGING_HD_LIGHT_IES_PROFILE_H

#include "pxr/pxr.h"

#include <string>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

/// Small private IES profile reader used by HdLight physical-lighting helpers.
class HdLightIesProfile
{
public:
    bool Load(std::string const& ies);
    void Clear();

    bool IsValid() const
    {
        return !_intensity.empty();
    }

    float GetPower() const
    {
        return _power;
    }

private:
    enum _IESType { _TypeA = 3, _TypeB = 2, _TypeC = 1 };

    bool _Parse(std::string const& ies);
    bool _Process();

    void _ProcessTypeA();
    void _ProcessTypeB();
    void _ProcessTypeC();
    void _ComputePower();

    std::vector<float> _vAngles;
    std::vector<float> _hAngles;
    std::vector<std::vector<float>> _intensity;
    _IESType _type = _TypeC;
    float _power = 0.0f;
};

PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_IMAGING_HD_LIGHT_IES_PROFILE_H
