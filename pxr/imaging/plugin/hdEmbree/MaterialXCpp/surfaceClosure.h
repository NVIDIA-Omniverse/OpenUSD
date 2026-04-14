//
// MaterialXCpp surface closure — pxr-independent material output.
//
#ifndef MXCPP_SURFACE_CLOSURE_H
#define MXCPP_SURFACE_CLOSURE_H

#include "../medium.h"
#include "mathTypes.h"
#include "materials/closureTree.h"

#include <algorithm>
#include <cmath>
#include <type_traits>
#include <variant>

namespace mxcpp {

inline float
_RegularizeAlphaRoughness(float alphaRoughness)
{
    const float clampedAlpha = std::clamp(alphaRoughness, 1.0e-5f, 1.0f);
    const float perceptualRoughness = std::sqrt(clampedAlpha);
    if (perceptualRoughness >= 0.3f) {
        return clampedAlpha;
    }

    const float widenedRoughness = std::clamp(
        2.0f * perceptualRoughness, 0.1f, 0.3f);
    return widenedRoughness * widenedRoughness;
}

/// Surface closure produced by material model evaluation.
/// Contains all parameters needed for BSDF evaluation.
struct SurfaceClosure
{
    // Legacy summary fields kept during the transition to tree-based BSDFs.
    // New rendering code should treat `bsdfTree` as the source of truth.
    Vec3f baseColor = Vec3f(0.8f);
    float roughness = 0.5f;
    float metallic = 0.0f;
    float specular = 1.0f;
    float specularIor = 1.5f;
    Vec3f specularColor = Vec3f(1.0f);
    Vec3f emissiveColor = Vec3f(0.0f);
    float transmission = 0.0f;
    Vec3f transmissionColor = Vec3f(1.0f);
    float opacity = 1.0f;
    float presence = 1.0f;
    float coat = 0.0f;
    float coatRoughness = 0.1f;
    float coatIor = 1.5f;
    float sheen = 0.0f;
    Vec3f sheenColor = Vec3f(1.0f);
    float sheenRoughness = 0.3f;
    Vec3f normal = Vec3f(0.0f, 0.0f, 1.0f);
    bool thinWalled = false;
    bool hasInteriorMedium = false;
    MediumProperties interiorMedium;
    float subsurfaceWeight = 0.0f;
    Vec3f subsurfaceColor = Vec3f(1.0f);
    Vec3f subsurfaceRadius = Vec3f(1.0f);
    Vec3f subsurfaceRadiusScale = Vec3f(1.0f);
    float subsurfaceAnisotropy = 0.0f;
    bool hasSubsurfaceMedium = false;
    MediumProperties subsurfaceMedium;
    Bsdf::ClosureTree bsdfTree;

    bool HasBsdfTree() const {
        return !bsdfTree.Empty();
    }

    bool HasDispersion() const {
        for (const auto& node : bsdfTree.nodes) {
            if (const auto* dielectric =
                    std::get_if<Bsdf::DielectricData>(&node.data)) {
                if (dielectric->dispersionAbbe > 0.0f) {
                    return true;
                }
            }
        }
        return false;
    }

    /// Regularize the surface closure by widening narrow specular lobes.
    void Regularize() {
        if (roughness < 0.3f) {
            roughness = std::clamp(2.0f * roughness, 0.1f, 0.3f);
        }
        if (coatRoughness < 0.3f) {
            coatRoughness = std::clamp(2.0f * coatRoughness, 0.1f, 0.3f);
        }
        for (auto& node : bsdfTree.nodes) {
            std::visit([](auto& data) {
                using T = std::decay_t<decltype(data)>;
                if constexpr (std::is_same_v<T, Bsdf::DielectricData> ||
                              std::is_same_v<T, Bsdf::ConductorData> ||
                              std::is_same_v<T, Bsdf::GeneralizedSchlickData>) {
                    for (int i = 0; i < 2; ++i) {
                        data.roughness[i] =
                            _RegularizeAlphaRoughness(data.roughness[i]);
                    }
                } else if constexpr (std::is_same_v<T, Bsdf::SheenData>) {
                    if (data.roughness < 0.3f) {
                        data.roughness = std::clamp(
                            2.0f * data.roughness, 0.1f, 0.3f);
                    }
                }
            }, node.data);
        }
    }
};

}  // namespace mxcpp

#endif  // MXCPP_SURFACE_CLOSURE_H
