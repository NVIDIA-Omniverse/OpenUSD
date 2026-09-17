//
// Copyright 2016 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//

#include "pxr/pxr.h"

#include "pxr/base/tf/pyArg.h"
#include "pxr/base/tf/stringUtils.h"

using std::string;
using std::vector;

PXR_NAMESPACE_OPEN_SCOPE

static void
_AddArgAndTypeDocStrings(
    const TfPyArg& arg, vector<string>* argStrs, vector<string>* typeStrs)
{
    argStrs->push_back(arg.GetName());
    if (!arg.GetDefaultValueDoc().empty()) {
        argStrs->back() += 
            TfStringPrintf(" = %s", arg.GetDefaultValueDoc().c_str());
    }

    typeStrs->push_back(
        TfStringPrintf("%s : %s", 
                       arg.GetName().c_str(), arg.GetTypeDoc().c_str()));
}

string 
TfPyCreateFunctionDocString(
    const string& functionName,
    const TfPyArgs& requiredArgs,
    const TfPyArgs& optionalArgs,
    const string& description)
{
    string rval = functionName + "(";

    vector<string> argStrs;
    vector<string> typeStrs;

    for (size_t i = 0; i < requiredArgs.size(); ++i) {
        _AddArgAndTypeDocStrings(requiredArgs[i], &argStrs, &typeStrs);
    }

    for (size_t i = 0; i < optionalArgs.size(); ++i) {
        _AddArgAndTypeDocStrings(optionalArgs[i], &argStrs, &typeStrs);
    }
    
    rval += TfStringJoin(argStrs.begin(), argStrs.end(), ", ");
    rval += ")";

    if (!typeStrs.empty()) {
        rval += "\n";
        rval += TfStringJoin(typeStrs.begin(), typeStrs.end(), "\n");
    }

    if (!description.empty()) {
        rval += "\n\n";
        rval += description;
    }

    return rval;
}

PXR_NAMESPACE_CLOSE_SCOPE
