//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "nativeTypes.h"
#include <complex>
#include <string>

PXR_NAMESPACE_USING_DIRECTIVE
namespace {
// Constructed when this library loads, before a Python binding for NativeValue
// exists. These type tables must also work after the binding registers it.
NativeValue object(37);
VtValue value(object);
VtValue typed{NativeTypedProxy(&object)};
VtValue erased{NativeErasedProxy(&value)};
}
VtValue const &GetNativeValue() { return value; }
VtValue const &GetTypedProxy() { return typed; }
VtValue const &GetErasedProxy() { return erased; }
NativeValue const &GetNativeObject() { return object; }
VtValue GetNativePointer(bool null) {
    return VtValue(null ? nullptr : &object);
}
VtValue GetNativeBuiltin(int index) {
    switch (index) {
    case 0: return VtValue(true);
    case 1: return VtValue('x');
    case 2: return VtValue(static_cast<signed char>(-7));
    case 3: return VtValue(static_cast<unsigned char>(255));
    case 4: return VtValue(static_cast<short>(-12));
    case 5: return VtValue(static_cast<unsigned short>(65535));
    case 6: return VtValue(-123);
    case 7: return VtValue(123u);
    case 8: return VtValue(-123l);
    case 9: return VtValue(123ul);
    case 10: return VtValue(-9223372036854775807ll);
    case 11: return VtValue(18446744073709551615ull);
    case 12: return VtValue(1.25f);
    case 13: return VtValue(2.5);
    case 14: return VtValue(3.75l);
    case 15: return VtValue(std::string("a\0b", 3));
    case 16: return VtValue(std::wstring(L"wide"));
    case 17: return VtValue(std::complex<float>(1, 2));
    case 18: return VtValue(std::complex<double>(3, 4));
    case 19: return VtValue(std::complex<long double>(5, 6));
    default: return {};
    }
}
