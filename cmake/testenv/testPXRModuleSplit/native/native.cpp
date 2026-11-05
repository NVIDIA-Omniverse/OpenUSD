#include "probe.h"
#if defined(EXPECT_NATIVE_POLICY) && !defined(Py_LIMITED_API)
#error Native target did not enforce the Limited API
#endif
#ifdef BINDING_ONLY_MARKER
#error Binding dependencies must not affect native compilation
#endif
int NativeAnswer() { return 42; }
