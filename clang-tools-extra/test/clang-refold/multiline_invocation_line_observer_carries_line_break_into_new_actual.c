// RUN: %clang-refold-tester-with-lines-verify-off multiline_invocation_line_observer_carries_line_break_into_new_actual
// Regression: a whole-actual replacement in a multi-line invocation keeps the
// macro by carrying the original line break into the new actual.
//
// `__LINE__` expands inside `CHECK` and takes the line of its closing paren,
// so a callsite rewrite must span as many line breaks as the recorded
// spelling.  The new actual `q(y) < 1` shares no token with `g(x) > 0`, so no
// original bytes survive the splice and the rewrite would collapse onto one
// line.  Instead the original gap that holds the line break is copied over
// the new actual's gap at the same token index.  That gap already held
// whitespace, so `#e` still stringifies to "q(y) < 1".
//
// This input used to be `multiline_invocation_line_observer_refuses_collapsing_rewrite`,
// which expanded the invocation.  Verification is off so the planner alone
// must place the line break.
int fail(const char *, int);
int g(int), q(int);

#define CHECK(e) ((e) ? 0 : fail(#e, __LINE__))

int f(int x, int y) {
  return CHECK(g(x)
               > 0);
}
