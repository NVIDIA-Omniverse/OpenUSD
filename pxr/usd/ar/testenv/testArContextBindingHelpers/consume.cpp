// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
#include "native.h"
#include "pxr/external/boost/python.hpp"
PXR_NAMESPACE_USING_DIRECTIVE
PXR_NS::ArResolverContext GetEarly() { return GetEarlyContext(); }
PXR_BOOST_PYTHON_MODULE(arContextConsume)
{
    pxr_boost::python::def("GetEarly", GetEarly);
}
