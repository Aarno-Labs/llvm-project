// RUN: %clang-refold-tester repeated_include_xmacro_distinct_definition_per_inclusion

// Regression: one header included twice under two different definitions of the
// macro it invokes must get one realization per inclusion.
//
// The two invocations are spelled at identical byte offsets of an identical
// path, so a producer test that decides "is this token inside that invocation"
// from (path, byte range) alone cannot separate them.  Under such a test the
// second inclusion's expansion tokens were attributed to the first inclusion's
// invocation as well, giving it an A-token envelope that spanned both
// expansions and the translation-unit source between them.  With that envelope
// the first invocation admitted no candidate and its hunks anchored on the
// shared macro-name token instead of the invocation extent.  Both inclusions
// carry an edit here, so those two mis-anchored edits collided on one span and
// the whole translation unit fell back to raw B.
//
// The companion test repeated_include_xmacro_single_edit_in_first_inclusion
// covers the one-edit shape, where nothing collides.

double acos(double);
static double wrap_error(double x) { return x; }

// Inclusion #1: ROW emits the wrapper definition.
#define ROW(name) static double f_##name(double x) { return wrap_error(name(x)); }
#include "xmacro_row.h"
#undef ROW

// Inclusion #2: ROW emits a table row naming that same wrapper.
struct entry { double (*fptr)(double); const char *name; };
static const struct entry table[] = {
#define ROW(name) {f_##name, #name},
#include "xmacro_row.h"
#undef ROW
};
