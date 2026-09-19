// RUN: %clang-refold-tester-verify-off macro_state_undef_restore_for_later_include_observer
// A synthetic `#undef` for a B payload is restored when a later `#include`
// follows, even though no translation-unit byte after the payload names the
// macro.
//
// The only later reader of `LATER_INCLUDE_ZZ` is inside the header.  The
// observation scan reads the translation unit's identifiers and cannot see
// through the `#include`, so any directive after the payload counts as a
// possible observer and the definition is re-emitted after the payload's line.
#define LATER_INCLUDE_ZZ 3
int later_include_b = LATER_INCLUDE_ZZ;
int later_include_arr[] = { 1 };
#include "macro_state_later_include_observer.h"
