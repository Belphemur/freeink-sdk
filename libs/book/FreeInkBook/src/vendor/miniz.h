/* FreeInkBook provenance wrapper for the nested esp_full_miniz submodule.
 * MinizConfig.h must come first: it installs the freeink_* symbol renames
 * that keep the engine away from ESP-IDF's ROM miniz and supplies the build
 * configuration before the fork declarations. The fork itself supplies
 * include/full_miniz.h. */
#pragma once

#include "../../include/epub/MinizConfig.h"
#include "full_miniz.h"
