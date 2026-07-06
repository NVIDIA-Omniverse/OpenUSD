//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//

#include "pxr/pxr.h"

#include "pxr/base/tf/diagnostic.h"
#include "pxr/base/tf/diagnosticTrap.h"
#include "pxr/base/tf/errorMark.h"
#include "pxr/base/tf/stringUtils.h"
#include "pxr/base/vt/dictionary.h"
#include "pxr/usd/sdf/layer.h"
#include "pxr/usd/sdf/primSpec.h"
#include "pxr/usd/sdf/reference.h"

#include <algorithm>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

static bool
_HasWarningContaining(
    const std::vector<TfWarning>& warnings,
    const std::string& text)
{
    return std::any_of(
        warnings.begin(), warnings.end(),
        [&text](const TfWarning& warning) {
            return TfStringContains(warning.GetCommentary(), text);
        });
}

static const VtValue&
_GetValue(const VtDictionary& dictionary, const char* key)
{
    const auto it = dictionary.find(key);
    TF_AXIOM(it != dictionary.end());
    return it->second;
}

static void
_VerifyStrictDictionaryParsing(const char* fileName)
{
    TfErrorMark mark;
    SdfLayerRefPtr layer = SdfLayer::FindOrOpen(fileName);
    TF_AXIOM(!layer);
    TF_AXIOM(!mark.IsClean());
    mark.Clear();
}

int
main()
{
    TfDiagnosticTrap trap;
    SdfLayerRefPtr layer = SdfLayer::FindOrOpen(
        "testSdfMalformedCustomData.testenv/malformed.usda");
    TF_AXIOM(layer);

    const std::vector<TfWarning> warnings = trap.GetWarnings();
    TF_AXIOM(warnings.size() == 4);
    TF_AXIOM(_HasWarningContaining(warnings, "malformed_layer="));
    TF_AXIOM(_HasWarningContaining(warnings, "malformed_nested="));
    TF_AXIOM(_HasWarningContaining(warnings, "malformed_prim="));
    TF_AXIOM(_HasWarningContaining(warnings, "malformed_reference="));
    TF_AXIOM(_HasWarningContaining(
        warnings,
        "custom data entries require a type, key, and value"));
    trap.ClearWarnings();
    TF_AXIOM(trap.IsClean());

    const VtDictionary layerData = layer->GetCustomLayerData();
    TF_AXIOM(_GetValue(layerData, "before") == VtValue(std::string("before")));
    TF_AXIOM(_GetValue(layerData, "after") == VtValue(std::string("after")));
    TF_AXIOM(layerData.count("malformed_layer") == 0);
    TF_AXIOM(_GetValue(layerData, "nested").IsHolding<VtDictionary>());
    const VtDictionary& nested =
        _GetValue(layerData, "nested").Get<VtDictionary>();
    TF_AXIOM(_GetValue(nested, "retained") == VtValue(7));
    TF_AXIOM(nested.count("malformed_nested") == 0);

    TF_AXIOM(layer->GetDefaultPrim() == TfToken("Root"));
    SdfPrimSpecHandle root = layer->GetPrimAtPath(SdfPath("/Root"));
    TF_AXIOM(root);

    const VtDictionary primData = root->GetCustomData();
    TF_AXIOM(_GetValue(primData, "before") == VtValue(std::string("before")));
    TF_AXIOM(_GetValue(primData, "after") == VtValue(std::string("after")));
    TF_AXIOM(primData.count("malformed_prim") == 0);

    const std::vector<SdfReference> references =
        root->GetReferenceList().GetAddedOrExplicitItems();
    TF_AXIOM(references.size() == 1);
    const VtDictionary& referenceData = references.front().GetCustomData();
    TF_AXIOM(_GetValue(referenceData, "retained") == VtValue(42));
    TF_AXIOM(referenceData.count("malformed_reference") == 0);

    _VerifyStrictDictionaryParsing(
        "testSdfMalformedCustomData.testenv/"
        "malformedExpressionVariables.usda");
    _VerifyStrictDictionaryParsing(
        "testSdfMalformedCustomData.testenv/malformedAssetInfo.usda");
}
