#pragma once
#ifdef _WIN32
#ifdef NATIVEPROBE_EXPORTS
#define PROBE_API __declspec(dllexport)
#else
#define PROBE_API __declspec(dllimport)
#endif
#else
#define PROBE_API __attribute__((visibility("default")))
#endif
extern "C" PROBE_API int NativeAnswer();
