/* Vendored esp_full_miniz v1.15 — tree copy of Belphemur/esp_full_miniz at
 * 7c3d708 (upstream richgel999/miniz v1.15 r4, 28f5066e3325, the vintage the
 * ESP32 mask ROM cores were built from). The engine inflates EPUB entries
 * through this copy via epub/MinizConfig.h, which renames the tinfl and mz
 * symbols so the linker can never bind them to the ESP32 mask ROM.
 *
 * This wrapper keeps the classic miniz.h include path (used by MinizConfig.h
 * and the patched pngle.c); the real vendored header is full_miniz.h. */
#pragma once

#include "full_miniz.h"