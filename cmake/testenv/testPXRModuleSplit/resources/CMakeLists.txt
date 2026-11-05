set(PXR_PREFIX fixture)
set(PXR_PACKAGE resourceProbe)
# The manifest reader must not inherit unrelated caller variables.
set(classes mustNotLeak)
set(modules mustNotLeak.cpp)
set(resources mustNotLeak.txt)
pxr_library(resourceProbe INCLUDE_SCHEMA_FILES)
pxr_module(resourceProbe LIBRARY resourceProbe INCLUDE_SCHEMA_FILES PYTHON_FILES __init__.py)
