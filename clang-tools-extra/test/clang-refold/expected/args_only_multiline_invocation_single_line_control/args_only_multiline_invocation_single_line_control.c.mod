// RUN: %clang-refold-tester args_only_multiline_invocation_single_line_control
// Control for args_only_multiline_invocation_preserves_line_observer: the same
// edit inside the same macro's single actual, spelled on one line, must keep
// folding.  It pins that preserving the original interior spelling is a
// spelling choice and not a new refusal.
int fail(const char *, int);
int g(int);

#define CHECK(e) ((e) ? 0 : fail(#e, __LINE__))

int f(int x) {
  return CHECK(g_r0(x) > 0);
}
