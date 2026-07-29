//
// Copyright 2018 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "lightRegistry.h"

#include <renderer/renderer.h>

#include <algorithm>
#include <iterator>

PXR_NAMESPACE_OPEN_SCOPE

void
ty::LightRegistry::Add(
    SdfPath const& path,
    ty::LightData const* light)
{
    std::scoped_lock lock(_mutex);

    auto eraseDomeEntries = [this](ty::LightData const* toRemove) {
        if (toRemove) {
            _domes.erase(
                std::remove(_domes.begin(), _domes.end(), toRemove),
                _domes.end());
        }
    };

    auto it = _lights.find(path);
    if (it != _lights.end() && it->second == light) {
        if (!light ||
            !std::holds_alternative<ty::DomeLight>(light->lightVariant)) {
            return;
        }
        const auto firstDome = std::find(_domes.begin(), _domes.end(), light);
        if (firstDome != _domes.end() &&
            std::find(std::next(firstDome), _domes.end(), light) ==
                _domes.end()) {
            return;
        }
        eraseDomeEntries(light);
    } else {
        if (it != _lights.end()) {
            eraseDomeEntries(it->second);
            it->second = light;
        } else {
            _lights.emplace(path, light);
        }
        eraseDomeEntries(light);
    }

    if (light && std::holds_alternative<ty::DomeLight>(light->lightVariant)) {
        _domes.push_back(light);
    }
}

void
ty::LightRegistry::Remove(
    SdfPath const& path,
    ty::LightData const* light)
{
    std::scoped_lock lock(_mutex);
    _lights.erase(path);
    _domes.erase(
        std::remove(_domes.begin(), _domes.end(), light),
        _domes.end());
}

void
ty::LightRegistry::AddGeometry(
    unsigned int geometryId,
    ty::LightData const* light)
{
    if (geometryId == RTC_INVALID_GEOMETRY_ID || !light) {
        return;
    }
    std::scoped_lock lock(_mutex);
    _geometryLights[geometryId] = light;
}

void
ty::LightRegistry::RemoveGeometry(
    unsigned int geometryId,
    ty::LightData const* light)
{
    if (geometryId == RTC_INVALID_GEOMETRY_ID) {
        return;
    }
    std::scoped_lock lock(_mutex);
    auto it = _geometryLights.find(geometryId);
    if (it != _geometryLights.end() && (!light || it->second == light)) {
        _geometryLights.erase(it);
    }
}

ty::LightData const*
ty::LightRegistry::FindGeometry(unsigned int geometryId) const
{
    if (geometryId == RTC_INVALID_GEOMETRY_ID) {
        return nullptr;
    }
    std::scoped_lock lock(_mutex);
    auto it = _geometryLights.find(geometryId);
    return it == _geometryLights.end() ? nullptr : it->second;
}

void
ty::Renderer::AddLight(
    SdfPath const& lightPath,
    ty::LightData const* light)
{
    _lights.Add(lightPath, light);
}

void
ty::Renderer::RemoveLight(
    SdfPath const& lightPath,
    ty::LightData const* light)
{
    _lights.Remove(lightPath, light);
}

void
ty::Renderer::AddLightGeometry(
    unsigned int geometryId,
    ty::LightData const* light)
{
    _lights.AddGeometry(geometryId, light);
}

void
ty::Renderer::RemoveLightGeometry(
    unsigned int geometryId,
    ty::LightData const* light)
{
    _lights.RemoveGeometry(geometryId, light);
}

PXR_NAMESPACE_CLOSE_SCOPE
