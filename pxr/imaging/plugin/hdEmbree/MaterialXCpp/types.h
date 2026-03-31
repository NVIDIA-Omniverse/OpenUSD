//
// MaterialXCpp core types — pxr-independent.
//
#ifndef MXCPP_TYPES_H
#define MXCPP_TYPES_H

#include "mxcpp_math.h"
#include "slots.h"
#include "mxcpp_value.h"

#include <algorithm>
#include <cctype>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

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
    Vec2f texcoord = Vec2f(0.0f);
    Vec3f displayColor = Vec3f(0.8f);
    float displayOpacity = 1.0f;
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
};

/// Surface closure produced by material model evaluation.
/// Contains all parameters needed for BSDF evaluation.
struct SurfaceClosure
{
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
    float coat = 0.0f;
    float coatRoughness = 0.1f;
    float coatIor = 1.5f;
    float sheen = 0.0f;
    Vec3f sheenColor = Vec3f(1.0f);
    float sheenRoughness = 0.3f;
    Vec3f normal = Vec3f(0.0f, 0.0f, 1.0f);
    bool thinWalled = false;

    /// Regularize the surface closure by widening narrow specular lobes.
    void Regularize() {
        if (roughness < 0.3f) {
            roughness = std::clamp(2.0f * roughness, 0.1f, 0.3f);
        }
        if (coatRoughness < 0.3f) {
            coatRoughness = std::clamp(2.0f * coatRoughness, 0.1f, 0.3f);
        }
    }
};

struct ParamEntry
{
    using ReevaluateFn = bool (*)(
        const void* userData,
        int sourceNodeIndex,
        SlotId sourceOutputSlot,
        const ShadingContext& ctx,
        Value* out);

    SlotId slot = InvalidSlotId;
    const Value* value = nullptr;
    Value* mutableValue = nullptr;
    ReevaluateFn reevaluate = nullptr;
    const void* reevaluateUserData = nullptr;
    int reevaluateNodeIndex = -1;
    SlotId reevaluateOutputSlot = InvalidSlotId;
};

/// Named parameter map used for node inputs/outputs.
class ParamMap
{
public:
    void Clear() {
        _entries.clear();
        _ownedValues.clear();
    }
    void Reserve(size_t count) { _entries.reserve(count); }

    void Add(SlotId slot, const Value* value) {
        Add(slot, value, nullptr, nullptr, -1, InvalidSlotId);
    }

    void Add(SlotId slot,
             const Value* value,
             ParamEntry::ReevaluateFn reevaluate,
             const void* reevaluateUserData,
             int reevaluateNodeIndex,
             SlotId reevaluateOutputSlot) {
        ParamEntry entry;
        entry.slot = slot;
        entry.value = value;
        entry.reevaluate = reevaluate;
        entry.reevaluateUserData = reevaluateUserData;
        entry.reevaluateNodeIndex = reevaluateNodeIndex;
        entry.reevaluateOutputSlot = reevaluateOutputSlot;
        _entries.push_back(entry);
    }

    template<typename NameT>
    Value& operator[](const NameT& name) {
        const SlotId slot = AsSlotId(name);
        for (auto& entry : _entries) {
            if (entry.slot == slot) {
                if (entry.mutableValue) {
                    return *entry.mutableValue;
                }

                _ownedValues.push_back(entry.value ? *entry.value : Value());
                Value* value = &_ownedValues.back();
                entry.value = value;
                entry.mutableValue = value;
                return *value;
            }
        }

        _ownedValues.emplace_back();
        Value* value = &_ownedValues.back();
        _entries.push_back({slot, value, value});
        return _ownedValues.back();
    }

    template<typename NameT>
    const Value* Find(const NameT& name) const {
        const SlotId slot = AsSlotId(name);
        for (const auto& entry : _entries) {
            if (entry.slot == slot) {
                return entry.value;
            }
        }
        return nullptr;
    }

    template<typename NameT>
    bool Evaluate(const NameT& name,
                  const ShadingContext& ctx,
                  Value* out) const
    {
        const SlotId slot = AsSlotId(name);
        for (const auto& entry : _entries) {
            if (entry.slot != slot) {
                continue;
            }

            if (entry.reevaluate &&
                entry.reevaluate(
                    entry.reevaluateUserData,
                    entry.reevaluateNodeIndex,
                    entry.reevaluateOutputSlot,
                    ctx,
                    out)) {
                return true;
            }

            if (!entry.value) {
                return false;
            }

            if (out) {
                *out = *entry.value;
            }
            return true;
        }
        return false;
    }

private:
    std::vector<ParamEntry> _entries;
    std::deque<Value> _ownedValues;
};

struct NodeOutputEntry
{
    SlotId slot = InvalidSlotId;
    Value value;
};

/// Output map produced by a node evaluation.
class NodeOutputMap
{
public:
    void Clear() { _entries.clear(); }
    void Reserve(size_t count) { _entries.reserve(count); }

    template<typename NameT>
    Value& operator[](const NameT& name) {
        const SlotId slot = AsSlotId(name);
        for (auto& entry : _entries) {
            if (entry.slot == slot) {
                return entry.value;
            }
        }

        _entries.push_back({slot, Value()});
        return _entries.back().value;
    }

    template<typename NameT>
    const Value* Find(const NameT& name) const {
        const SlotId slot = AsSlotId(name);
        for (const auto& entry : _entries) {
            if (entry.slot == slot) {
                return &entry.value;
            }
        }
        return nullptr;
    }

private:
    std::vector<NodeOutputEntry> _entries;
};

template<typename T>
struct ValueGetter
{
    template<typename NameT>
    static T Get(const ParamMap& params,
                 const NameT& name,
                 const T& defaultVal)
    {
        const Value* value = params.Find(name);
        if (value && ValueHolds<T>(*value)) {
            return ValueGet<T>(*value);
        }
        return defaultVal;
    }
};

template<>
struct ValueGetter<float>
{
    template<typename NameT>
    static float Get(const ParamMap& params,
                     const NameT& name,
                     const float& defaultVal)
    {
        const Value* value = params.Find(name);
        if (!value) return defaultVal;
        if (ValueHolds<float>(*value))
            return ValueGet<float>(*value);
        if (ValueHolds<int>(*value))
            return static_cast<float>(ValueGet<int>(*value));
        return defaultVal;
    }
};

/// Extract a typed value from a parameter map with a default fallback.
template<typename T, typename NameT>
T Get(const ParamMap& params,
      const NameT& name,
      const T& defaultVal)
{
    return ValueGetter<T>::Get(params, name, defaultVal);
}

inline std::string
NormalizeSpaceName(const std::string& space,
                   const std::string& workingSpace)
{
    std::string normalized = space.empty() ? workingSpace : space;
    std::transform(
        normalized.begin(), normalized.end(), normalized.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (normalized == "model") {
        return "object";
    }
    if (normalized == "pobject" ||
        normalized == "nobject" ||
        normalized == "tobject" ||
        normalized == "bobject") {
        return "object";
    }
    if (normalized == "pworld" ||
        normalized == "nworld" ||
        normalized == "tworld" ||
        normalized == "bworld") {
        return "world";
    }
    return normalized;
}

inline bool
TransformNamedVec3(const ShadingContext& ctx,
                   const std::string& fromSpace,
                   const std::string& toSpace,
                   ShadingContext::TransformSpaceType type,
                   const Vec3f& in,
                   Vec3f* out)
{
    const std::string from = NormalizeSpaceName(fromSpace, ctx.workingSpace);
    const std::string to = NormalizeSpaceName(toSpace, ctx.workingSpace);

    if (from == to) {
        if (out) {
            *out = in;
        }
        return true;
    }

    if (ctx.transformSpace &&
        ctx.transformSpace(ctx.transformUserData, from, to, type, in, out)) {
        return true;
    }

    const bool fromObject = from == "object";
    const bool fromWorld = from == "world";
    const bool toObject = to == "object";
    const bool toWorld = to == "world";
    if ((!fromObject && !fromWorld) || (!toObject && !toWorld)) {
        return false;
    }

    Vec3f result(0.0f);
    if (type == ShadingContext::TransformSpaceType::Point) {
        if (fromObject && toWorld && ctx.hasObjectToWorldTransform) {
            ctx.objectToWorldMatrix.multVecMatrix(in, result);
        } else if (fromWorld && toObject && ctx.hasWorldToObjectTransform) {
            ctx.worldToObjectMatrix.multVecMatrix(in, result);
        } else {
            return false;
        }
    } else if (type == ShadingContext::TransformSpaceType::Vector) {
        if (fromObject && toWorld && ctx.hasObjectToWorldTransform) {
            ctx.objectToWorldMatrix.multDirMatrix(in, result);
        } else if (fromWorld && toObject && ctx.hasWorldToObjectTransform) {
            ctx.worldToObjectMatrix.multDirMatrix(in, result);
        } else {
            return false;
        }
    } else {
        if (fromObject && toWorld && ctx.hasWorldToObjectTransform) {
            ctx.worldToObjectMatrix.transposed().multDirMatrix(in, result);
        } else if (fromWorld && toObject && ctx.hasObjectToWorldTransform) {
            ctx.objectToWorldMatrix.transposed().multDirMatrix(in, result);
        } else {
            return false;
        }

        if (Dot(result, result) > 0.0f) {
            result.normalize();
        }
    }

    if (out) {
        *out = result;
    }
    return true;
}

/// Zero value helpers for template-based node implementations.
template<typename T> inline T Zero();
template<> inline float Zero<float>() { return 0.0f; }
template<> inline int Zero<int>() { return 0; }
template<> inline bool Zero<bool>() { return false; }
template<> inline std::string Zero<std::string>() { return std::string(); }
template<> inline Vec2f Zero<Vec2f>() { return Vec2f(0.0f); }
template<> inline Vec3f Zero<Vec3f>() { return Vec3f(0.0f); }
template<> inline Vec4f Zero<Vec4f>() { return Vec4f(0.0f); }
template<> inline Mat3f Zero<Mat3f>() { return Mat3f(0.0f); }
template<> inline Mat4f Zero<Mat4f>() { return Mat4f(0.0f); }

/// One value helpers for template-based node implementations.
template<typename T> inline T One();
template<> inline float One<float>() { return 1.0f; }
template<> inline int One<int>() { return 1; }
template<> inline bool One<bool>() { return true; }
template<> inline Vec2f One<Vec2f>() { return Vec2f(1.0f); }
template<> inline Vec3f One<Vec3f>() { return Vec3f(1.0f); }
template<> inline Vec4f One<Vec4f>() { return Vec4f(1.0f); }
template<> inline Mat3f One<Mat3f>() { return Mat3f(); }
template<> inline Mat4f One<Mat4f>() { return Mat4f(); }

/// Component-wise multiplication (scalars use operator*, vectors use
/// per-element multiplication).
template<typename T>
inline T CompMul(const T& a, const T& b) { return a * b; }

template<>
inline Vec2f
CompMul<Vec2f>(const Vec2f& a, const Vec2f& b)
{
    return Vec2f(a[0] * b[0], a[1] * b[1]);
}

template<>
inline Vec3f
CompMul<Vec3f>(const Vec3f& a, const Vec3f& b)
{
    return CompMult(a, b);
}

template<>
inline Vec4f
CompMul<Vec4f>(const Vec4f& a, const Vec4f& b)
{
    return Vec4f(a[0] * b[0], a[1] * b[1], a[2] * b[2], a[3] * b[3]);
}

/// Component-wise division.
template<typename T>
inline T CompDiv(const T& a, const T& b) {
    return (b != T(0)) ? a / b : Zero<T>();
}
template<>
inline Vec2f
CompDiv<Vec2f>(const Vec2f& a, const Vec2f& b)
{
    return Vec2f(b[0] != 0.0f ? a[0] / b[0] : 0.0f,
                 b[1] != 0.0f ? a[1] / b[1] : 0.0f);
}

template<>
inline Vec3f
CompDiv<Vec3f>(const Vec3f& a, const Vec3f& b)
{
    return Vec3f(b[0] != 0.0f ? a[0] / b[0] : 0.0f,
                 b[1] != 0.0f ? a[1] / b[1] : 0.0f,
                 b[2] != 0.0f ? a[2] / b[2] : 0.0f);
}

template<>
inline Vec4f
CompDiv<Vec4f>(const Vec4f& a, const Vec4f& b)
{
    return Vec4f(b[0] != 0.0f ? a[0] / b[0] : 0.0f,
                 b[1] != 0.0f ? a[1] / b[1] : 0.0f,
                 b[2] != 0.0f ? a[2] / b[2] : 0.0f,
                 b[3] != 0.0f ? a[3] / b[3] : 0.0f);
}

}  // namespace mxcpp

#endif  // MXCPP_TYPES_H
