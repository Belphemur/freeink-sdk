/* FreeInkBook only needs miniz's low-level streaming inflate (tinfl). The
 * archive, stdio, time, and zlib-compatibility layers are compiled out so
 * the vendored library stays small and never touches the filesystem or clock.
 * Include this header instead of <miniz.h> so every translation unit sees the
 * same configuration. */
#pragma once

#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_ARCHIVE_WRITING_APIS
// v1.15 has no MINIZ_NO_DEFLATE_APIS; MINIZ_NO_ZLIB_APIS is the flag that
// gates the mz_deflate/mz_compress/tdefl-wrapper layer (full_miniz.h:181,
// 248-465; miniz.c:84-440, 741-758) and miniz_cores.c carries a matching
// guard around the tdefl compressor core, so this both drops the dead
// compressor code and keeps un-renamed tdefl_* out of the firmware.
#define MINIZ_NO_ZLIB_APIS
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES

// v1.15 scopes MZ_VERSION inside #ifndef MINIZ_NO_ZLIB_APIS (full_miniz.h:248,
// 256); anything that still references the version string would fail to link.
// Provide the upstream v1.15 value unconditionally.
#ifndef MZ_VERSION
#define MZ_VERSION "9.1.15"
#endif

// Provenance of the nested esp_full_miniz submodule (Belphemur fork at
// a6bf8fc, based on upstream v1.15 r4), reported by vendorVersions() so the
// active miniz lineage is observable at runtime.
#define FREEINK_MINIZ_FORK "esp_full_miniz a6bf8fc"

// The ESP32 mask ROM exports tinfl_* at fixed addresses via DIRECT linker
// script assignments (esp32s3.rom.ld: "tinfl_decompress = 0x40000828;"),
// which override object-file definitions — without these renames the
// firmware silently binds to the 2021 ROM build (TINFL_LESS_MEMORY, a
// different tinfl_decompressor layout) and corrupts inflate state on real
// data. PROVIDE()-style ROM symbols (like tjpgd's) lose to our
// definitions; these do not. Rename so the linker can never capture them.
#define tinfl_decompress freeink_tinfl_decompress
#define tinfl_decompress_mem_to_heap freeink_tinfl_decompress_mem_to_heap
#define tinfl_decompress_mem_to_mem freeink_tinfl_decompress_mem_to_mem
#define tinfl_decompress_mem_to_callback freeink_tinfl_decompress_mem_to_callback
#define mz_crc32 freeink_mz_crc32
#define mz_adler32 freeink_mz_adler32
#define mz_free freeink_mz_free
#define mz_error freeink_mz_error

// Include the fork header directly: ESP-IDF ships a ROM miniz.h with the
// SAME include guard but a different (TINFL_LESS_MEMORY) struct layout —
// resolving <miniz.h> through the platform include path would silently
// compile against the wrong structures. <full_miniz.h> exists only in the
// fork's include/ directory.
#include <full_miniz.h>
