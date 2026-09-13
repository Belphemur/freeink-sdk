/* Compiles the vendored esp_full_miniz tinfl/tdefl cores (miniz_cores.c,
 * v1.15 fork @ 7c3d708) for the FreeInkBook engine. The fork guards this file
 * with !ESP_PLATFORM because its intended consumers bind the cores to the
 * ESP32 mask ROM; the engine deliberately compiles its own renamed cores in
 * RAM instead (see MinizConfig.h for the ROM-binding hazard), so the guard is
 * neutralized for this TU only. Other translation units — including the
 * ROM-bound firmware reader — keep ESP_PLATFORM intact. */
#include "epub/MinizConfig.h"

#ifdef ESP_PLATFORM
#undef ESP_PLATFORM
#endif
#include "../../third_party/miniz/miniz_cores.c"