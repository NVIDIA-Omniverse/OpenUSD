//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//

#include "pxr/pxr.h"

#include "pxr/base/tf/diagnostic.h"
#include "pxr/base/tf/diagnosticTrap.h"
#include "pxr/base/tf/stringUtils.h"
#include "pxr/usd/sdf/layer.h"

PXR_NAMESPACE_USING_DIRECTIVE

int
main()
{
    TfDiagnosticTrap trap;
    SdfLayerRefPtr layer = SdfLayer::FindOrOpen(
        "testSdfMalformedCustomData.testenv/malformed.usda");
    TF_AXIOM(layer);

    TF_AXIOM(trap.GetWarnings().size() == 1);
    const std::string& message = trap.GetWarnings().front().GetCommentary();
    TF_AXIOM(TfStringContains(
        message,
        "Skipping malformed custom data entry 'malformed_item='"));
    TF_AXIOM(TfStringContains(
        message,
        "custom data entries require a type, key, and value"));
    trap.ClearWarnings();
    TF_AXIOM(trap.IsClean());

    TF_AXIOM(layer->GetCustomLayerData().empty());
    TF_AXIOM(layer->GetDefaultPrim() == TfToken("Root"));
}
