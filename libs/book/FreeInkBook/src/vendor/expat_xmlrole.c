/* Compiles the vendored expat parser's prolog-state machine.
 *
 * With FREEINK_BOOK_EXTERNAL_EXPAT defined (e.g. CrossPoint firmware, which
 * builds its own hardened lib/expat with the same public XML_* API), this
 * file compiles to nothing so the vendored copy is excluded from the build
 * and the host's expat is linked instead. Without the flag the vendored
 * sources compile and FreeInkBook stays self-contained.
 */
#if !defined(FREEINK_BOOK_EXTERNAL_EXPAT)
#include "../../third_party/expat/xmlrole.c"
#endif
