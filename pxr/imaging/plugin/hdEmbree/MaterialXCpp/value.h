//
// MaterialXCpp value type — pxr-independent, std::variant-based.
//
#ifndef MXCPP_VALUE_H
#define MXCPP_VALUE_H

#include "mathTypes.h"

#include <string>
#include <variant>

namespace mxcpp {

using Value = std::variant<
    std::monostate,
    float,
    int,
    bool,
    Vec2f,
    Vec3f,
    Vec4f,
    Mat3f,
    Mat4f,
    std::string>;

inline bool ValueIsEmpty(const Value& v) {
    return std::holds_alternative<std::monostate>(v);
}

template<typename T>
inline bool ValueHolds(const Value& v) {
    return std::holds_alternative<T>(v);
}

template<typename T>
inline const T& ValueGet(const Value& v) {
    return std::get<T>(v);
}

}  // namespace mxcpp

#endif  // MXCPP_VALUE_H
