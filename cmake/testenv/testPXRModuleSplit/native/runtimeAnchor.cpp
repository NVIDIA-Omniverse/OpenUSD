// The Python-disabled build still has a native library, with no Python calls.
#ifdef _WIN32
__declspec(dllexport)
#endif
int RuntimeAnchor() { return 1; }
