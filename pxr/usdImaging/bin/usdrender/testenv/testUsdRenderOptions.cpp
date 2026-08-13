#include "options.h"

#include <cmath>
#include <iostream>

namespace {

bool
_Expect(bool condition, const char* message)
{
    if (!condition)
        std::cerr << message << "\n";
    return condition;
}

bool
_TestDefaults()
{
    char program[] = "usdrender";
    char stage[] = "scene.usda";
    char* argv[] = {program, stage};
    Options options;
    if (!ParseOptions(2, argv, &options))
        return false;
    const bool complexityMatches =
        _Expect(std::abs(options.complexity - 1.2f) < 1e-6f,
                "default complexity is not high");
    const bool rendererIsDeferred =
        _Expect(options.renderer.empty(),
                "omitted renderer should remain available for stage resolution");
    return complexityMatches && rendererIsDeferred;
}

bool
_TestExplicitValues()
{
    char program[] = "usdrender";
    char stage[] = "scene.usda";
    char complexityFlag[] = "--complexity";
    char complexity[] = "low";
    char rendererFlag[] = "--renderer";
    char renderer[] = "Storm";
    char* argv[] = {program, stage, complexityFlag, complexity,
                    rendererFlag, renderer};
    Options options;
    if (!ParseOptions(6, argv, &options))
        return false;
    const bool complexityMatches =
        _Expect(std::abs(options.complexity - 1.0f) < 1e-6f,
                "explicit complexity was ignored");
    const bool rendererMatches =
        _Expect(options.renderer == "Storm", "explicit renderer was ignored");
    return complexityMatches && rendererMatches;
}

} // namespace

int
main()
{
    return _TestDefaults() && _TestExplicitValues() ? 0 : 1;
}
