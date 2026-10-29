//
// Copyright 2020 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_BASE_TF_PY_SAFE_PYTHON_H
#define PXR_BASE_TF_PY_SAFE_PYTHON_H

#include "pxr/base/arch/pragmas.h"

/// \file tf/pySafePython.h
/// Intended to replace a direct include of Python.h, which causes several
/// build problems with certain configurations and platforms (e.g., debug 
/// builds on Windows, Qt slots keyword, etc.)

// This include is a hack to avoid build errors due to incompatible
// macro definitions in pyport.h on MacOS for older versions of Python.
// See: https://bugs.python.org/issue10910
#include <locale>

// Python 3 has a conflict with the slots macro defined by the Qt library,
// so we're undef'ing here temporarily before including Python.
ARCH_PRAGMA_PUSH_MACRO(slots)
#undef slots

// Include the CRT headers with the caller's debug settings before temporarily
// hiding _DEBUG from Python.h. A debug C++ build normally uses release Python;
// PXR_USE_DEBUG_PYTHON explicitly requests the debug Python runtime instead.
#if defined(_MSC_VER) && !defined(PXR_USE_DEBUG_PYTHON)
#if defined(_DEBUG)
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <errno.h>
#include <ctype.h>
#include <wchar.h>
#include <basetsd.h>
#include <io.h>
#include <limits.h>
#include <float.h>
#include <string.h>
#include <math.h>
#include <time.h>
#endif
ARCH_PRAGMA_PUSH_MACRO(_DEBUG)
#undef _DEBUG
#endif

#include <Python.h>

#if defined(_MSC_VER) && !defined(PXR_USE_DEBUG_PYTHON)
ARCH_PRAGMA_POP_MACRO(_DEBUG)
#endif

ARCH_PRAGMA_POP_MACRO(slots)

#endif // PXR_BASE_TF_PY_SAFE_PYTHON_H
