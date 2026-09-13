/* FreeInkBook provenance wrapper for the nested esp_full_miniz submodule.
 * Keeps the classic miniz.h include path away from ESP-IDF's ROM miniz.h;
 * the real header is the fork's include/full_miniz.h. */
#pragma once

#include "full_miniz.h"
