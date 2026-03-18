//
// MaterialXCpp core types — pxr-independent.
//
#ifndef MXCPP_TYPES_H
#define MXCPP_TYPES_H

#include "mxcpp_math.h"
#include "slots.h"
#include "mxcpp_value.h"

#include <algorithm>
#include <deque>
#include <vector>

namespace mxcpp {

/// Geometric context provided to node evaluation at each shading point.
struct ShadingContext
{
    Vec3f position = Vec3f(0.0f);
    Vec3f normal = Vec3f(0.0f, 0.0f, 1.0f);
    Vec3f tangent = Vec3f(1.0f, 0.0f, 0.0f);
    Vec3f bitangent = Vec3f(0.0f, 1.0f, 0.0f);
    Vec2f texcoord = Vec2f(0.0f);
    Vec3f displayColor = Vec3f(0.8f);
    float displayOpacity = 1.0f;
    int faceId = 0;
    float baryU = 0.0f;
    float baryV = 0.0f;
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
    SlotId slot = InvalidSlotId;
    const Value* value = nullptr;
    Value* mutableValue = nullptr;
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
        _entries.push_back({slot, value});
    }

    template<typename NameT>
    Value& operator[](const NameT& name) {
        const SlotId slot = AsSlotId(name);
        for (auto& entry : _entries) {
            if (entry.slot == slot) {
                if (entry.mutableValue) {
                    return *entry.mutableValue;
                }

                _ownedValues.push_back(*entry.value);
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

/// Zero value helpers for template-based node implementations.
template<typename T> inline T Zero();
template<> inline float Zero<float>() { return 0.0f; }
template<> inline int Zero<int>() { return 0; }
template<> inline bool Zero<bool>() { return false; }
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
