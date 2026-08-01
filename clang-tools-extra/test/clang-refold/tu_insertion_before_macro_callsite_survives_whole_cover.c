// RUN: %clang-refold-tester tu_insertion_before_macro_callsite_survives_whole_cover
// Regression: `*d` becomes `d[0]` in plain TU text immediately before a macro
// callsite whose own argument also changed.  The subscript arrives as a pure
// zero-width insertion hunk anchored one token *before* the invocation's cover.
//
// Whole-cover realization replaces the callsite with the invocation's edited
// expansion, so it can only emit B material that expansion produces.  Admitting
// it for a hunk reaching outside the cover silently dropped the `[0]`, leaving
// `if (d != ...)` -- a pointer compared against an integer that still compiles.
// The direct-callee phase already refused this shape; whole-cover must too.
#define TOLE(x) ((x) + 1)

int cmp(const char *d, const int *s)
{
  if (*d != TOLE(*s))
    return 1;
  return 0;
}
