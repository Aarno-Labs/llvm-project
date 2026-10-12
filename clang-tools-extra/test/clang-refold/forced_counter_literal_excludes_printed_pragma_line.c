// RUN: %clang-refold-tester-verify-off forced_counter_literal_excludes_printed_pragma_line
// Regression: the edited first `__COUNTER__` forces the second to its B
// literal.  That literal was sliced up to the next B token, so it took the
// `#pragma` line the `_Pragma` operator printed, while the operator stayed in
// source and printed it again.  The literal is now the bytes of its own B
// tokens.
int f(void) {
  int a = __COUNTER__;
  int b = __COUNTER__ _Pragma("GCC diagnostic ignored \"-Wunused\"") ;
  return a + b;
}
