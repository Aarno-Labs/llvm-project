// RUN: %clang-refold-tester-with-lines-verify-off multiline_invocation_line_observer_gapless_actual_expands_call
// Fail-closed floor for the multi-line invocation line collapse.
//
// `__LINE__` expands inside `CHECK` and takes the line of its closing paren.
// The new actual `q(y)` shares no token with `g(x) > 0` and has no whitespace
// between its tokens, so the original line break has no gap to move into: a
// break inside `q(y)` would change what `#e` stringifies to.  The callsite
// rewrite would span no line break where the recorded spelling spans one, so it
// is refused and this one invocation is expanded.
//
// Two independent layers refuse it: the line-count floor on the callsite
// rewrite, and the final audit of preserved line observers, which sends the
// invocation to the expansion ladder.  Disabling either one alone leaves this
// output unchanged.  Verification is off, so the pin is on the planner.
//
// The expansion is not the best sound answer: a break before the closing
// paren, `CHECK(q(y)` / `)`, is trailing whitespace of the argument, which
// stringification deletes.  Nothing places line breaks at an argument's edge
// yet.
int fail(const char *, int);
int g(int), q(int);

#define CHECK(e) ((e) ? 0 : fail(#e, __LINE__))

int f(int x, int y) {
  return CHECK(g(x)
               > 0);
}
