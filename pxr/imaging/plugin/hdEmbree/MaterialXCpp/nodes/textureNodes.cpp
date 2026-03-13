//
// Copyright 2024 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "textureNodes.h"
#include "../nodeRegistry.h"

#include <string>

namespace mxcpp {

static const std::string _kFile = "file";
static const std::string _kTexcoord = "texcoord";
static const std::string _kOut = "out";
static const std::string _kDefaultVal = "default";

// Placeholder image node: returns the default value.
// Full CPU texture sampling (via hio) can be added in a follow-up.
// The architecture allows swapping in a real sampler without changing
// the graph or other nodes.

template<typename T>
static void
_EvalImage(const ParamMap& inputs, const ShadingContext& ctx,
           NodeOutputMap* outputs)
{
    // TODO: load and sample actual texture via hio.
    // For now, return the authored default value or a sensible fallback.
    T defaultVal = Get<T>(inputs, "default", Zero<T>());
    (*outputs)[_kOut] = Value(defaultVal);
}

// ---- Registration --------------------------------------------------------

#define _REG(name, fn) reg.Register(name, fn)

void
RegisterTextureNodes(NodeRegistry& reg)
{
    _REG("ND_image_float",   &_EvalImage<float>);
    _REG("ND_image_color3",  &_EvalImage<Vec3f>);
    _REG("ND_image_color4",  &_EvalImage<Vec4f>);
    _REG("ND_image_vector2", &_EvalImage<Vec2f>);
    _REG("ND_image_vector3", &_EvalImage<Vec3f>);

    // tiledimage uses the same placeholder for now.
    _REG("ND_tiledimage_float",  &_EvalImage<float>);
    _REG("ND_tiledimage_color3", &_EvalImage<Vec3f>);
    _REG("ND_tiledimage_color4", &_EvalImage<Vec4f>);
}

#undef _REG

} // namespace mxcpp
