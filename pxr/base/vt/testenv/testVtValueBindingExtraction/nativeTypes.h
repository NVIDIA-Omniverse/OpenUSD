//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef VT_VALUE_BINDING_TEST_TYPES_H
#define VT_VALUE_BINDING_TEST_TYPES_H
#include "pxr/base/vt/value.h"
#include "pxr/base/vt/valueRef.h"
#include <ostream>

#if defined(_WIN32)
# if defined(VT_NATIVE_TYPES_EXPORTS)
#  define VT_NATIVE_API __declspec(dllexport)
# else
#  define VT_NATIVE_API __declspec(dllimport)
# endif
#else
# define VT_NATIVE_API __attribute__((visibility("default")))
#endif

struct NativeValue {
    explicit NativeValue(int n = 0) : number(n) {}
    int number;
};
inline bool operator==(NativeValue const &a, NativeValue const &b) {
    return a.number == b.number;
}
inline std::ostream &operator<<(std::ostream &out, NativeValue const &value) {
    return out << value.number;
}
struct NativeTypedProxy : PXR_NS::VtTypedValueProxyBase {
    NativeValue const *value;
    explicit NativeTypedProxy(NativeValue const *p) : value(p) {}
};
inline NativeValue const &VtGetProxiedObject(NativeTypedProxy const &proxy) {
    return *proxy.value;
}
struct NativeErasedProxy : PXR_NS::VtErasedValueProxyBase {
    PXR_NS::VtValue const *value;
    explicit NativeErasedProxy(PXR_NS::VtValue const *p) : value(p) {}
};
inline PXR_NS::VtValue const *VtGetErasedProxiedVtValue(NativeErasedProxy const &p) {
    return p.value;
}
inline std::ostream &operator<<(std::ostream &out, NativeErasedProxy const &p) {
    return out << *p.value;
}
inline bool VtErasedProxyHoldsType(NativeErasedProxy const &p,
                                 std::type_info const &type) {
    return p.value->GetTypeid() == type;
}
inline PXR_NS::TfType VtGetErasedProxiedTfType(NativeErasedProxy const &p) {
    return p.value->GetType();
}

VT_NATIVE_API PXR_NS::VtValue const &GetNativeValue();
VT_NATIVE_API PXR_NS::VtValue const &GetTypedProxy();
VT_NATIVE_API PXR_NS::VtValue const &GetErasedProxy();
VT_NATIVE_API NativeValue const &GetNativeObject();
VT_NATIVE_API PXR_NS::VtValue GetNativePointer(bool null);
VT_NATIVE_API PXR_NS::VtValue GetNativeBuiltin(int index);
#endif
