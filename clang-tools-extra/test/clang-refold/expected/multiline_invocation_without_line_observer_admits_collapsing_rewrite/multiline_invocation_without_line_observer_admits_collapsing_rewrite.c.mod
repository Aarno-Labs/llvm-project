// RUN: %clang-refold-tester multiline_invocation_without_line_observer_admits_collapsing_rewrite
// Companion to multiline_invocation_line_observer_carries_line_break_into_new_actual:
// the same unspliceable whole-actual edit on the same multi-line invocation
// shape, but the callee spells no line observer.  Nothing observes the physical
// line inside the invocation, so the collapsed rewrite is admitted as it is and
// the callsite is still preserved.  This pins both the line-count floor and the
// line-break carry on the __LINE__ containment fact rather than on the
// invocation being multi-line.
int fail(const char *, int);
int g(int), q(int);

#define CHECK(e) ((e) ? 0 : fail(#e, 0))

int f(int x, int y) {
  return CHECK(q(y) < 1);
}
