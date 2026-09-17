//
// Copyright 2016 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//

#include "pxr/pxr.h"

#include "pxr/base/tf/pyWrapContext.h"
#include "pxr/base/tf/diagnosticLite.h"
#include "pxr/base/tf/instantiateSingleton.h"

PXR_NAMESPACE_OPEN_SCOPE

TF_INSTANTIATE_SINGLETON(Tf_PyWrapContextManager);

Tf_PyWrapContextManager::Tf_PyWrapContextManager()
{
    // initialize the stack of context names
    _contextStack.clear();
}

std::string
Tf_PyGetCurrentWrapContext()
{
    return Tf_PyWrapContextManager::GetInstance().GetCurrentContext();
}

void
Tf_PyPushWrapContext(std::string const &ctx)
{
    Tf_PyWrapContextManager::GetInstance().PushContext(ctx);
}

void
Tf_PyPopWrapContext()
{
    Tf_PyWrapContextManager::GetInstance().PopContext();
}

PXR_NAMESPACE_CLOSE_SCOPE
