// Copyright 2026 Pixar
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
#ifndef TEST_SDF_SPEC_REGISTRY_NATIVE_H
#define TEST_SDF_SPEC_REGISTRY_NATIVE_H
#include "pxr/usd/sdf/spec.h"
#include "pxr/base/arch/export.h"
#ifdef HOLDER_NATIVE_EXPORTS
#define HOLDER_API ARCH_EXPORT
#else
#define HOLDER_API ARCH_IMPORT
#endif
HOLDER_API bool InitializeNativeSpecs();
HOLDER_API const PXR_NS::SdfSpec& NativePrim();
HOLDER_API const PXR_NS::SdfSpec& NativeAttribute();
HOLDER_API const PXR_NS::SdfSpec& NativeVariant();
HOLDER_API const PXR_NS::SdfSpec& NativeRelationship();
HOLDER_API void RemoveNativePrim();
HOLDER_API void ClearNativeSpecs();
#endif
