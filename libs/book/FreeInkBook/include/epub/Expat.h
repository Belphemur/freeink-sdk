#pragma once

// Public shim over FreeInkBook's expat.
//
// By default the engine vendors and compiles expat itself
// (src/vendor/expat_*.c, configured by third_party/expat/expat_config.h).
// Applications that need an XML parser of their own — OPDS feeds, sync
// protocols — should include this header instead of carrying a second expat
// copy: one configuration, one set of symbols, no duplicate-definition links.
//
// With FREEINK_BOOK_EXTERNAL_EXPAT defined (e.g. CrossPoint firmware, which
// builds its own hardened lib/expat), the vendored copy is excluded from the
// build and the host's expat provides the XML_* symbols — so this shim
// resolves to the host's <expat.h>, keeping declarations and definitions in
// the same copy.

#ifdef FREEINK_BOOK_EXTERNAL_EXPAT
#include <expat.h>
#else
#include "../../third_party/expat/expat.h"
#endif
