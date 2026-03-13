//
// MaterialXCpp value type — pxr-independent, std::any-based.
//
#ifndef MXCPP_VALUE_H
#define MXCPP_VALUE_H

#include <any>

namespace mxcpp {

using Value = std::any;

template<typename T>
inline bool ValueHolds(const Value& v) {
    return v.type() == typeid(T);
}

template<typename T>
inline const T& ValueGet(const Value& v) {
    return std::any_cast<const T&>(v);
}

} // namespace mxcpp

#endif // MXCPP_VALUE_H
