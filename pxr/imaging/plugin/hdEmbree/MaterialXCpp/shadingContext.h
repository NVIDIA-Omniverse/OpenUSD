//
// MaterialXCpp shading context — pxr-independent runtime state.
//
#ifndef MXCPP_SHADING_CONTEXT_H
#define MXCPP_SHADING_CONTEXT_H

#include "mathTypes.h"
#include "textureSystem.h"
#include "value.h"

#include <string>
#include <unordered_map>

namespace mxcpp {

/// Geometric context provided to node evaluation at each shading point.
struct ShadingContext
{
    enum class TransformSpaceType {
        Point,
        Vector,
        Normal
    };

    using TransformSpaceFn = bool(*)(
        const void* userData,
        const std::string& fromSpace,
        const std::string& toSpace,
        TransformSpaceType type,
        const Vec3f& in,
        Vec3f* out);

    // Object-space position, matching MaterialX's default Pobject behavior.
    Vec3f position = Vec3f(0.0f);
    // World-space shading frame, matching MaterialX defaults such as Nworld.
    Vec3f normal = Vec3f(0.0f, 0.0f, 1.0f);
    Vec3f tangent = Vec3f(1.0f, 0.0f, 0.0f);
    Vec3f bitangent = Vec3f(0.0f, 1.0f, 0.0f);
    Vec3f viewPosition = Vec3f(0.0f);
    Vec2f texcoord = Vec2f(0.0f);
    Vec3f displayColor = Vec3f(0.8f);
    float displayOpacity = 1.0f;
    float frame = 0.0f;
    float time = 0.0f;
    int faceId = 0;
    float baryU = 0.0f;
    float baryV = 0.0f;

    // World-space surface derivatives in the active parameterization basis.
    // With "st", these are dP/ds and dP/dt; otherwise they fall back to the
    // coarse triangle's local barycentric-edge basis.
    Vec3f dPdu = Vec3f(0.0f);
    Vec3f dPdv = Vec3f(0.0f);

    // World-space screen-space position derivatives (from ray differentials).
    Vec3f dPdx = Vec3f(0.0f);
    Vec3f dPdy = Vec3f(0.0f);

    // Object-space position derivatives used when reevaluating upstream nodes
    // that depend on MaterialX's default Pobject position.
    Vec3f dPositiondu = Vec3f(0.0f);
    Vec3f dPositiondv = Vec3f(0.0f);
    Vec3f dPositiondx = Vec3f(0.0f);
    Vec3f dPositiondy = Vec3f(0.0f);

    // Screen-space coefficients for the active derivative basis above.
    // These are true texture derivatives only when dPdu/dPdv represent st.
    float dudx = 0.0f, dvdx = 0.0f;
    float dudy = 0.0f, dvdy = 0.0f;

    // Named-space transform support for geometric and transform* nodes.
    // The callback can handle arbitrary renderer-defined spaces; when it is
    // absent or declines a transform, mxcpp falls back to built-in object/world
    // transforms using the matrices below.
    TransformSpaceFn transformSpace = nullptr;
    const void* transformUserData = nullptr;
    std::string workingSpace = "world";
    Mat4f objectToWorldMatrix;
    Mat4f worldToObjectMatrix;
    bool hasObjectToWorldTransform = false;
    bool hasWorldToObjectTransform = false;

    // Per-sample varying property lookup (geompropvalue).
    // Returns the named geometric property at the current shading point.
    // Returns std::monostate (empty Value) on failure.
    using GeomPropFn = Value(*)(const void* userData, const std::string& name);
    GeomPropFn geomPropLookup = nullptr;
    const void* geomPropUserData = nullptr;

    // Per-mesh uniform property map (geompropvalueuniform).
    // Points to a pre-built map; lifetime managed by the caller.
    const std::unordered_map<std::string, Value>* uniformProps = nullptr;

    // Optional color transform callback for MaterialX colortransform nodes.
    // The callback receives canonical lower-case color-space names and RGB
    // triplets only. color4 alpha passthrough is handled in mxcpp.
    using ColorTransformFn = bool(*)(
        const void* userData,
        const std::string& sourceColorSpace,
        const std::string& targetColorSpace,
        const Vec3f& in,
        Vec3f* out);
    ColorTransformFn colorTransform = nullptr;
    const void* colorTransformUserData = nullptr;

    // Texture lookup backend owned by the embedding renderer. The pointer is
    // non-owning and may be null, in which case texture nodes fall back to
    // their authored default values.
    const TextureSystem* textureSystem = nullptr;
};

}  // namespace mxcpp

#endif  // MXCPP_SHADING_CONTEXT_H
