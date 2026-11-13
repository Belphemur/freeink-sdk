/* Compiles the vendored JPEGDEC (third_party/jpegdec/src) for HOST builds.
 * On device the firmware's JPEGDEC libdep — symlink:// pointed at this same
 * vendored submodule (platformio.ini) — compiles JPEGDEC.cpp plus the
 * ESP32-S3/P4 SIMD kernels; compiling it here too would give two definitions
 * of every JPEGDEC symbol. JPEGDEC works inside its own JPEGIMAGE object —
 * no caller workspace, no allocation. */
// JPEGDEC's host builds select the stdlib branch with -D__LINUX__ (its own
// Makefile does the same); without it the header wants Arduino.h.
#if !defined(ARDUINO) && !defined(__MACH__) && !defined(ESP_PLATFORM) && !defined(__LINUX__)
#define __LINUX__ 1
#endif
// Host (x86 SSE) builds must take the scalar JPEGFilter path: upstream's SSE
// marker-filter desyncs some entropy streams (verified against the same
// fixtures under -DNO_SIMD and against upstream 1.2.7 — the device build
// never compiles the SSE filter, so NO_SIMD makes the host decode path
// byte-identical to the device's). This whole TU is host-only anyway.
#if !defined(ARDUINO)
#define NO_SIMD 1
#endif
#if defined(ARDUINO)
// Device: the JPEGDEC libdep builds the vendored sources; keep this TU empty
// so no JPEGDEC symbol is defined twice.
typedef int jpegdec_host_only_tu;
#else
#include "../../third_party/jpegdec/src/JPEGDEC.cpp"
#endif
