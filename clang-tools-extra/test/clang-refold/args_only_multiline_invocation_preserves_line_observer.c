// RUN: %clang-refold-tester args_only_multiline_invocation_preserves_line_observer
// Regression: an args-only rewrite of a function-like macro invocation whose
// spelling spans two physical lines must re-emit it on two lines.  The new
// actual is sliced out of B, where the whole expansion sits on one line, so
// the invocation used to collapse.  That is observable: __LINE__ in the
// callee's replacement list takes the line of the invocation's *closing
// paren*, so the collapse moved the observer from 16 to 15 and the closing
// check refused the whole translation unit back to the raw edited stream.
int fail(const char *, int);
int g(int);

#define CHECK(e) ((e) ? 0 : fail(#e, __LINE__))

int f(int x) {
  return CHECK(g(x)
               > 0);
}
