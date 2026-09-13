/* Compiles the vendored esp_full_miniz (v1.15 fork @ 7c3d708) archive/zlib
 * layer with FreeInkBook's configuration. The fork strips the tinfl/tdefl
 * cores into miniz_cores.c — on ESP targets the fork leaves those to the
 * mask ROM, but this engine must NOT bind the ROM cores (TINFL_LESS_MEMORY
 * layout hazard, see MinizConfig.h), so src/vendor/miniz_cores_impl.c
 * compiles the cores locally with the freeink_* renames active. */
#include "epub/MinizConfig.h"
#include "../../third_party/miniz/miniz.c"