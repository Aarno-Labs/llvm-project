// RUN: %clang-refold-tester nested_parens_and_commas FIRST
// RUN: %clang-refold-tester nested_parens_and_commas SECOND
// RUN: %clang-refold-tester nested_parens_and_commas THIRD
// RUN: %clang-refold-tester nested_parens_and_commas EXPAND
// Test: nested_parens_and_commas
// Refold intent: preserve nested arguments containing parentheses and comma-like structure

#define F(a, b) ((a) + (b))
#define G(x, y) F(x, (y))
#define H(z) (G z)
int main(void) {
  int v = H((2, ((4 + 1) * 2)));
  return v == ((1) + ((4 + 1))) ? 0 : 1;
}
