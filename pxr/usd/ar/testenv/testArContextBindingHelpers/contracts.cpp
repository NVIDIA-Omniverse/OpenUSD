// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
#include "native.h"
#include <cstdlib>
int main()
{
    const auto& early = GetEarlyContext();
    auto copy = early;
    PXR_NS::ArResolverContext merged(std::vector<PXR_NS::ArResolverContext>{early});
    return early.Get<LateContext>()->value == 17 && copy == early &&
        merged == early && hash_value(copy) == hash_value(early) &&
        !early.GetDebugString().empty() ? EXIT_SUCCESS : EXIT_FAILURE;
}
