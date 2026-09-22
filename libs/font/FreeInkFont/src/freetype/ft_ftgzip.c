/* FreeInkFont build wrapper: compile the vendored FreeType amalgam with the
 * library build macro set, so PlatformIO compiles it as part of this lib while
 * FtFont.cpp stays a plain public-API consumer. Generated; do not edit. */
#define FT2_BUILD_LIBRARY
#include "../../freetype-config/include/freetype/config/ftoption.h"  /* resolves the
                        * FREEINK_FONT_ENABLE_* default before the gate below. */
#if FREEINK_FONT_ENABLE_GZIP
#include "../../third_party/freetype/src/gzip/ftgzip.c"
#endif
