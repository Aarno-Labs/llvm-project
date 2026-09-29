// RUN: %clang-refold-tester-verify-off header_decl_clipped_replace_leaves_no_stale_token
//
// Regression: one hunk replaces `1; int` across the two declarations of
// macro_gap.h, and the gap between them holds a zero-token call that the
// neutrality proof cannot discharge: CAT pastes a non-empty argument.  The
// header edit is clipped to the first declaration, so without the full source
// envelope it replaced only `1;` with `10,` and left the second declaration's
// `int` behind, emitting `int before = 10, WRAP_EMPTY() int after = 20;`.  A
// clip that leaves material of the include behind now requires the envelope,
// and the include is realized from B.
#define KEEP(x) ((x) + 1)
#define SEL_0
#define CAT(a, b) a##b
#define WRAP_EMPTY() CAT(SEL_, 0)

int untouched = KEEP(5);

#include "macro_gap.h"
