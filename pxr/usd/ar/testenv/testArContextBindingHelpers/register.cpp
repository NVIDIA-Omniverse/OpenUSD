// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
#include "native.h"
#include "pxr/usd/ar/boost/pyResolverContext.h"
#include "pxr/external/boost/python.hpp"
#include <string>
PXR_NAMESPACE_USING_DIRECTIVE
static std::string Repr(const LateContext& ctx)
{
    return "LateContext(" + std::to_string(ctx.value) + ")";
}
PXR_BOOST_PYTHON_MODULE(arContextRegister)
{
    using namespace pxr_boost::python;
    class_<LateContext>("LateContext", init<int>())
        .def_readwrite("value", &LateContext::value)
        .def("__repr__", Repr);
    PXR_NS::ArWrapResolverContextForPython<LateContext>();
}
