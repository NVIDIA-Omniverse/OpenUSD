//
// Copyright 2026 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_LIGHT_LINKING_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_LIGHT_LINKING_H

#include "pxr/pxr.h"
#include "pxr/base/tf/token.h"
#include "pxr/base/vt/array.h"

#include <algorithm>

PXR_NAMESPACE_OPEN_SCOPE

using HdEmbreeCategorySet = VtArray<TfToken>;

inline bool HdEmbreeMatchesLink(
    TfToken const& link, HdEmbreeCategorySet const& categories)
{
    return link.IsEmpty() ||
        std::find(categories.begin(), categories.end(), link) !=
            categories.end();
}

inline void HdEmbreeMergeCategories(
    HdEmbreeCategorySet const& source, HdEmbreeCategorySet* destination)
{
    if (!destination) {
        return;
    }
    for (TfToken const& category : source) {
        if (std::find(destination->begin(), destination->end(), category) ==
            destination->end()) {
            destination->push_back(category);
        }
    }
}

PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_IMAGING_PLUGIN_HD_EMBREE_LIGHT_LINKING_H
