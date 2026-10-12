// RUN: %clang-refold-tester-verify-off macro_state_undef_before_forced_counter_whole_cover
// Regression: the edited first `__COUNTER__` forces `M()` to its whole cover
// to keep the later counter value, and that B text holds the painted `X` of
// `#define X (X + 1)`.  The forced counter patch carried no B payload
// certificate, so `X` expanded again.  It now certifies the B tokens it
// spells, and the `#undef X` partition covers it.
int X = 1;
#define X (X + 1)
#define M() __COUNTER__ + X
int f(void) {
  int a = __COUNTER__;
  int b = M();
  return a + b;
}
