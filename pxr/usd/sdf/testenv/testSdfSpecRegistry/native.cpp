// Copyright 2026 Pixar
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
#include "native.h"
#include "pxr/usd/sdf/layer.h"
#include "pxr/usd/sdf/primSpec.h"
#include "pxr/usd/sdf/attributeSpec.h"
#include "pxr/usd/sdf/relationshipSpec.h"
#include "pxr/usd/sdf/variantSpec.h"
#include "pxr/usd/sdf/variantSetSpec.h"

PXR_NAMESPACE_USING_DIRECTIVE
namespace {
SdfLayerRefPtr layer;
SdfPrimSpecHandle prim;
SdfAttributeSpecHandle attribute;
SdfRelationshipSpecHandle relationship;
SdfVariantSpecHandle variant;
}
bool InitializeNativeSpecs()
{
    layer = SdfLayer::CreateAnonymous("spec-registry.usda");
    if (!layer) return false;
    prim = SdfPrimSpec::New(layer, "Root", SdfSpecifierDef);
    if (!prim) return false;
    attribute = SdfAttributeSpec::New(prim, "value", SdfValueTypeNames->Int);
    relationship = SdfRelationshipSpec::New(prim, "target");
    auto variantSet = SdfVariantSetSpec::New(prim, "choices");
    variant = SdfVariantSpec::New(variantSet, "option");
    return attribute && relationship && variant;
}
const SdfSpec& NativePrim() { return prim.GetSpec(); }
const SdfSpec& NativeAttribute() { return attribute.GetSpec(); }
const SdfSpec& NativeVariant() { return variant.GetSpec(); }
const SdfSpec& NativeRelationship() { return relationship.GetSpec(); }
void RemoveNativePrim() { layer->RemoveRootPrim(prim); }
void ClearNativeSpecs()
{
    variant.Reset();
    relationship.Reset();
    attribute.Reset();
    prim.Reset();
    layer.Reset();
}
extern "C" HOLDER_API int InitializeSpecs() { return InitializeNativeSpecs(); }
