// RUN: %clang-refold-tester repeated_include_xmacro_single_edit_in_first_inclusion

// Regression: the quiet half of the shape covered by
// repeated_include_xmacro_distinct_definition_per_inclusion.
//
// Same double inclusion under two different definitions of ROW, but only one
// identifier inside the first inclusion's expansion is edited, so the edit that
// the producer's (path, byte range) invocation test mis-anchored on the shared
// macro-name token had no duplicate to collide with.  Nothing failed closed:
// the replacement was written over `ROW(acos)` itself, the `#include`
// disappeared, and clang-refold exited clean with a bogus call expression at
// file scope where the wrapper definition belonged.  Whether the defect
// surfaced loudly or silently was decided only by how many identifiers the
// edit happened to touch, so both shapes are pinned.
//
// The second inclusion needs no edit and must keep its `#include`.

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
