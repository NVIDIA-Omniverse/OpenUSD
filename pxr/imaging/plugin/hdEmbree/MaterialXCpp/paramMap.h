//
// MaterialXCpp parameter maps — pxr-independent node IO containers.
//
#ifndef MXCPP_PARAM_MAP_H
#define MXCPP_PARAM_MAP_H

#include "shadingContext.h"
#include "slots.h"
#include "value.h"

#include <deque>
#include <vector>

namespace mxcpp {

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
        if (!value) {
            return defaultVal;
        }
        if (ValueHolds<float>(*value)) {
            return ValueGet<float>(*value);
        }
        if (ValueHolds<int>(*value)) {
            return static_cast<float>(ValueGet<int>(*value));
        }
        return defaultVal;
    }
};

/// Extract a typed value from a parameter map with a default fallback.
template<typename T, typename NameT>
inline T Get(const ParamMap& params,
             const NameT& name,
             const T& defaultVal)
{
    return ValueGetter<T>::Get(params, name, defaultVal);
}

}  // namespace mxcpp

#endif  // MXCPP_PARAM_MAP_H
