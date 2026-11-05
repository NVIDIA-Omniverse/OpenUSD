#include "probe.h"
#ifdef BINDING_ONLY_MARKER
#error Binding dependencies must not affect native compilation
#endif
int NativeAnswer() { return 42; }
