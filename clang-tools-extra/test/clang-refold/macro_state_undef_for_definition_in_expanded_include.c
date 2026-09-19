// RUN: %clang-refold-tester-verify-off macro_state_undef_for_definition_in_expanded_include
// A payload naming a macro defined in a header is repaired even when the
// header's include has to be expanded.
//
// The gap carry used to move the whole `#include` past the payload to take its
// `#define` along, which also moved `int expanded_h;` and reordered tokens.  A
// macro-state repair may no longer move an include that contributes tokens, so
// the include is expanded instead.  An expanded include's definition has no
// movable surface, and it used to be invisible to both the liveness audit and
// the repairs, which emitted `int 3;`.  It is now bound from the include's
// site, and the payload gets a synthetic `#undef`; nothing after it reads
// `EXPANDED_V`, so no restore is needed.
int expanded_a;
#include "macro_state_definition_in_expanded_include.h"
int expanded_t;
