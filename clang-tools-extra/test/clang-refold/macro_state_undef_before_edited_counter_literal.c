// RUN: %clang-refold-tester-verify-off macro_state_undef_before_edited_counter_literal
// Regression: B replaces the counter's value with the variable `X`, and the
// counter literal patch spells that B hunk where `#define X (X + 1)` is live.
// The patch carried no B payload certificate, so `X` expanded.  It now
// certifies the B tokens it spells, and the `#undef X` partition covers it.
int X = 1;
#define X (X + 1)
int f(void) {
  int z = X;
  int a = __COUNTER__;
  return z + a;
}
